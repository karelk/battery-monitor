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

#define ALERT_THRESHOLD 12
#define SHUTDOWN_THRESHOLD 8
#define ALERT_SOUND "/usr/local/share/sounds/warning-loud-shrill-chime.wav"
#define UEVENT_BUFFER_SIZE 8192

static volatile int running = 1;

void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

int read_battery_status(char *status, int *capacity) {
    DIR *dir;
    struct dirent *entry;
    char bat_path[PATH_MAX];
    char file_path[PATH_MAX + 16];
    FILE *fp;
    int found = 0;

    dir = opendir("/sys/class/power_supply");
    if (!dir) {
        syslog(LOG_ERR, "Error opening /sys/class/power_supply: %m");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "BAT", 3) == 0) {
            snprintf(bat_path, sizeof(bat_path), "/sys/class/power_supply/%s", entry->d_name);
            found = 1;
            break;
        }
    }
    closedir(dir);

    if (!found) {
        fprintf(stderr, "No battery found\n");
        return -1;
    }

    snprintf(file_path, sizeof(file_path), "%s/status", bat_path);
    fp = fopen(file_path, "r");
    if (!fp) {
        perror("Error reading battery status");
        return -1;
    }
    if (fgets(status, 32, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    status[strcspn(status, "\n")] = 0;

    snprintf(file_path, sizeof(file_path), "%s/capacity", bat_path);
    fp = fopen(file_path, "r");
    if (!fp) {
        perror("Error reading battery capacity");
        return -1;
    }
    if (fscanf(fp, "%d", capacity) != 1) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return 0;
}

void play_alert(void) {
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork failed: %m");
        return;
    }

    if (pid == 0) {
        execl("/usr/local/bin/aplay", "aplay", "-q", ALERT_SOUND, NULL);
        syslog(LOG_ERR, "execl failed: %m");
        exit(1);
    }
}

int get_check_interval(int capacity) {
    if (capacity > 35)
        return 300;
    else if (capacity > 25)
        return 180;
    else if (capacity > 15)
        return 60;
    else
        return 30;
}

void monitor_battery_loop(void) {
    char status[32];
    int capacity;
    int last_alerted_capacity = -1;

    while (running) {
        if (read_battery_status(status, &capacity) != 0) {
            sleep(60);
            continue;
        }

        if (strcmp(status, "Discharging") != 0) {
            return;
        }

        if (capacity <= SHUTDOWN_THRESHOLD) {
            play_alert();
            syslog(LOG_CRIT, "Battery at %d%% - initiating immediate shutdown", capacity);
            system("/sbin/shutdown -h now 'Battery critically low - emergency shutdown'");
            closelog();
            exit(0);
        }

        if (capacity < ALERT_THRESHOLD) {
            if (capacity != last_alerted_capacity) {
                syslog(LOG_WARNING, "Battery at %d%%", capacity);
                play_alert();
                last_alerted_capacity = capacity;
            }
        }

        int interval = get_check_interval(capacity);
        sleep(interval);
    }
}

void listen_for_ac_events(int sock) {
    char buffer[UEVENT_BUFFER_SIZE];
    char status[32];
    int capacity;
    ssize_t len;

    while (running) {
        len = recv(sock, buffer, sizeof(buffer), 0);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (!running) break;
            continue;
        }
        if (len == 0) {
            continue;
        }

        if (strstr(buffer, "power_supply") || strstr(buffer, "POWER_SUPPLY")) {
            if (read_battery_status(status, &capacity) != 0)
                continue;

            if (strcmp(status, "Discharging") == 0) {
                monitor_battery_loop();
            }
        }
    }
}

int main(void) {
    int sock;
    struct sockaddr_nl addr;
    char status[32];
    int capacity;

    openlog("battery-monitor", LOG_PID, LOG_DAEMON);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    syslog(LOG_INFO, "Battery monitor starting (alert: %d%%, shutdown: %d%%)",
           ALERT_THRESHOLD, SHUTDOWN_THRESHOLD);

    sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (sock < 0) {
        syslog(LOG_ERR, "Error creating netlink socket (need root?): %m");
        closelog();
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = 1;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "Error binding netlink socket: %m");
        close(sock);
        closelog();
        return 1;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (read_battery_status(status, &capacity) == 0) {
        if (strcmp(status, "Discharging") == 0) {
            monitor_battery_loop();
        }
    }

    listen_for_ac_events(sock);

    close(sock);
    closelog();
    return 0;
}
