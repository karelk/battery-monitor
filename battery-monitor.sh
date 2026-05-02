#!/bin/sh

### BEGIN INIT INFO
# Provides:          battery-monitor
# Required-Start:    $syslog
# Required-Stop:     $syslog
# Should-Start:      gradm
# Default-Start:     2 3
# Default-Stop:      0 1 6
# Short-Description: Battery low notification and emergency shutdown
### END INIT INFO

PATH=/usr/sbin:/usr/bin

. /lib/lsb/init-functions
  

case "$1" in

########################################################################################################################
  start) ###############################################################################################################
########################################################################################################################

	if [ -f /var/run/battery-monitor.pid ] && kill -0 $(cat /var/run/battery-monitor.pid) 2>/dev/null; then
		log_action_msg "battery-monitor already running"
		exit 0
	fi

	log_daemon_msg "Starting battery-monitor"
	/usr/local/sbin/battery-monitor >/dev/null 2>&1 &
	echo $! > /var/run/battery-monitor.pid
	log_end_msg 0
	;;

########################################################################################################################
  stop) ################################################################################################################
########################################################################################################################

	if [ ! -f /var/run/battery-monitor.pid ]; then
		log_action_msg "battery-monitor not running"
		exit 0
	fi

	log_daemon_msg "Stopping battery-monitor"
	kill $(cat /var/run/battery-monitor.pid) 2>/dev/null
	rm -f /var/run/battery-monitor.pid
	log_end_msg 0
	;;

########################################################################################################################
  status) ##############################################################################################################
########################################################################################################################

	if [ -f /var/run/battery-monitor.pid ] && kill -0 $(cat /var/run/battery-monitor.pid) 2>/dev/null; then
		log_action_msg "battery-monitor is running (PID $(cat /var/run/battery-monitor.pid))"
	else
		log_action_msg "battery-monitor is not running"
		exit 1
	fi
	;;

########################################################################################################################
  *) ###################################################################################################################
########################################################################################################################

	echo "Usage: $0 {start|stop|status}"
	exit 1
	;;

esac

exit 0

