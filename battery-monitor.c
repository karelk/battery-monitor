#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <linux/netlink.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <linux/limits.h>

/* Configuration Constants */
#define ALERT_THRESHOLD 12
#define SHUTDOWN_THRESHOLD 8
#define ALERT_SOUND "/usr/local/share/sounds/warning-loud-shrill-chime.wav"
#define UEVENT_BUFFER_SIZE 8192
#define STATUS_BUFFER_SIZE 32

/* Global State */
static volatile sig_atomic_t running = 1;
static char cached_bat_path[PATH_MAX] = "";

/**
 * Handle termination signals gracefully.
 */
static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

/**
 * Find the first battery in sysfs and cache its path once.
 * Returns 0 on success, -1 if no battery is found.
 */
static int find_battery(void) {
    DIR *dir;
    struct dirent *entry;
    int found = 0;

    dir = opendir("/sys/class/power_supply");
    if (!dir) {
        syslog(LOG_ERR, "Error opening /sys/class/power_supply: %m");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "BAT", 3) == 0) {
            snprintf(cached_bat_path, sizeof(cached_bat_path), 
                     "/sys/class/power_supply/%s", entry->d_name);
            found = 1;
            break;
        }
    }
    closedir(dir);

    if (!found) {
        syslog(LOG_ERR, "No battery found in /sys/class/power_supply");
        return -1;
    }

    return 0;
}

/**
 * Read battery status and capacity using the cached path.
 */
static int read_battery_status(char *status, int *capacity) {
    char file_path[PATH_MAX + 16];
    FILE *fp;

    if (cached_bat_path[0] == '\0') return -1;

    /* Read Status */
    snprintf(file_path, sizeof(file_path), "%s/status", cached_bat_path);
    fp = fopen(file_path, "r");
    if (!fp) {
        syslog(LOG_ERR, "Error opening %s: %m", file_path);
        return -1;
    }
    if (fgets(status, STATUS_BUFFER_SIZE, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    status[strcspn(status, "\n")] = 0;

    /* Read Capacity */
    snprintf(file_path, sizeof(file_path), "%s/capacity", cached_bat_path);
    fp = fopen(file_path, "r");
    if (!fp) {
        syslog(LOG_ERR, "Error opening %s: %m", file_path);
        return -1;
    }
    if (fscanf(fp, "%d", capacity) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return 0;
}

/**
 * Fork and execute aplay to play the alert sound.
 * Parent does not wait; SIGCHLD is ignored in main to prevent zombies.
 */
static void play_alert(void) {
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork failed: %m");
        return;
    }

    if (pid == 0) {
        execl("/usr/local/bin/aplay", "aplay", "-q", ALERT_SOUND, (char *)NULL);
        exit(EXIT_FAILURE);
    }
}

/**
 * Determine polling interval based on current capacity.
 */
static int get_check_interval(int capacity) {
    if (capacity > 35) return 300;
    if (capacity > 25) return 180;
    if (capacity > 15) return 60;
    return 30;
}

/**
 * Active monitoring loop when the battery is discharging.
 */
static void monitor_battery_loop(void) {
    char status[STATUS_BUFFER_SIZE];
    int capacity;
    int last_alerted_capacity = -1;

    syslog(LOG_INFO, "Entering active discharge monitoring");

    while (running) {
        if (read_battery_status(status, &capacity) != 0) {
            sleep(60);
            continue;
        }

        if (strcmp(status, "Discharging") != 0) {
            syslog(LOG_INFO, "Power state changed to %s; stopping active monitor", status);
            return;
        }

        if (capacity <= SHUTDOWN_THRESHOLD) {
            syslog(LOG_CRIT, "Battery at %d%% - emergency shutdown initiated", capacity);
            play_alert();
            system("/sbin/shutdown -h now 'Battery critically low'");
            closelog();
            exit(EXIT_SUCCESS);
        }

        if (capacity < ALERT_THRESHOLD) {
            if (capacity != last_alerted_capacity) {
                syslog(LOG_WARNING, "Battery low: %d%%", capacity);
                play_alert();
                last_alerted_capacity = capacity;
            }
        }

        sleep(get_check_interval(capacity));
    }
}

/**
 * Listen for Netlink uevents from the kernel.
 */
static void listen_for_events(int sock) {
    char buffer[UEVENT_BUFFER_SIZE];
    char status[STATUS_BUFFER_SIZE];
    int capacity;

    while (running) {
        ssize_t len = recv(sock, buffer, sizeof(buffer), 0);
        if (len <= 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }

        if (strstr(buffer, "power_supply")) {
            if (read_battery_status(status, &capacity) == 0) {
                if (strcmp(status, "Discharging") == 0) {
                    monitor_battery_loop();
                }
            }
        }
    }
}

int main(void) {
    int sock;
    struct sockaddr_nl addr;
    char status[STATUS_BUFFER_SIZE];
    int capacity;

    openlog("battery-monitor", LOG_PID, LOG_DAEMON);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGCHLD, SIG_IGN);

    if (find_battery() != 0) {
        closelog();
        return EXIT_FAILURE;
    }

    syslog(LOG_INFO, "Battery monitor started (Alert: %d%%, Shutdown: %d%%)", 
           ALERT_THRESHOLD, SHUTDOWN_THRESHOLD);

    sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (sock < 0) {
        syslog(LOG_ERR, "Netlink socket creation failed: %m");
        closelog();
        return EXIT_FAILURE;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = 1;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "Netlink bind failed: %m");
        close(sock);
        closelog();
        return EXIT_FAILURE;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (read_battery_status(status, &capacity) == 0) {
        if (strcmp(status, "Discharging") == 0) {
            monitor_battery_loop();
        }
    }

    listen_for_events(sock);

    syslog(LOG_INFO, "Battery monitor stopping");
    close(sock);
    closelog();
    return EXIT_SUCCESS;
}
