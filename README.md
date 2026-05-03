# battery-monitor

Event-driven battery monitor for Linux - netlink uevents, adaptive checking, syslog integration. For minimal sysvinit systems without systemd/upower.

## Features

- Event-driven AC detection via kernel netlink uevents
- Adaptive battery checking (5/3/1 minute intervals based on capacity)
- Audio alerts at configurable threshold (default: 12%)
- Emergency shutdown at critical level (default: 8%)
- Syslog integration for logging
- No dependencies on systemd, upower, or udev daemon
- Single compiled binary, minimal resource usage

## Requirements

- Linux kernel with ACPI battery support
- ALSA (`aplay` for audio alerts)
- Root privileges (for netlink socket)
- Sysvinit or compatible init system

## Installation

```bash
# Compile
make

# Install binary
sudo cp battery-monitor /usr/local/sbin/
sudo chmod 755 /usr/local/sbin/battery-monitor

# Install init script
sudo cp battery-monitor.sh /etc/init.d/battery-monitor
sudo chmod 755 /etc/init.d/battery-monitor

# Enable at boot
sudo update-rc.d battery-monitor defaults

# Start service
sudo /etc/init.d/battery-monitor start
```

## Configuration

Edit `battery-monitor.c` and recompile to adjust thresholds:

```c
#define ALERT_THRESHOLD 12      // Start audio alerts at 12%
#define SHUTDOWN_THRESHOLD 7    // Emergency shutdown at 7%
#define ALERT_SOUND "/usr/local/share/sounds/warning-loud-shrill-chime.wav"
```

Adaptive check intervals:
- Battery > 35%: check every 5 minutes
- Battery 20-35%: check every 3 minutes
- Battery < 20%: check every 60 seconds

## Behavior

**On AC power:** Daemon idles, listening for unplug events (zero CPU usage)

**On battery:**
- Monitors battery capacity at adaptive intervals
- Plays audio alert on each percentage drop below alert threshold
- Initiates immediate system shutdown at critical threshold
- Prevents filesystem corruption from hard power loss

**Logging:** All events logged via syslog (`/var/log/syslog` or `/var/log/messages`)

## Design

Follows Unix philosophy: simple, focused, minimal dependencies. Uses kernel netlink sockets directly instead of relying on higher-level power management daemons. Suitable for custom/minimal Linux installations.

## License

MIT
