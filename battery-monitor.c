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
#include <linux/limits.h>

#define ALERT_THRESHOLD 12
#define SHUTDOWN_THRESHOLD 7
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
        perror("Error opening /sys/class/power_supply");
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
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return;
    }

    if (pid == 0) {
        execl("/usr/local/bin/aplay", "aplay", ALERT_SOUND, NULL);
        perror("execl failed");
        exit(1);
    }
}

int get_check_interval(int capacity) {
    if (capacity > 35)
        return 300;
    else if (capacity > 25)
        return 180;
    else
        return 60;
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
            printf("CRITICAL: Battery at %d%% - initiating immediate shutdown\n", capacity);
            system("/sbin/shutdown -h now 'Battery critically low - emergency shutdown'");
            exit(0);
        }

        if (capacity < ALERT_THRESHOLD) {
            if (capacity != last_alerted_capacity) {
                printf("WARNING: Battery at %d%%\n", capacity);
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
        if (len <= 0) {
            if (!running) break;
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

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("Battery monitor starting (alert: %d%%, shutdown: %d%%)\n",
           ALERT_THRESHOLD, SHUTDOWN_THRESHOLD);

    sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (sock < 0) {
        perror("Error creating netlink socket (need root?)");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = 1;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Error binding netlink socket");
        close(sock);
        return 1;
    }

    if (read_battery_status(status, &capacity) == 0) {
        if (strcmp(status, "Discharging") == 0) {
            monitor_battery_loop();
        }
    }

    listen_for_ac_events(sock);

    close(sock);
    return 0;
}
