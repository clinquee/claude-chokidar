#!/bin/sh
# Network watchdog for the Lightsail VM, run every minute by chokidar-netwatch.timer.
#
# On 2026-11-18 the VM lost its network ("network is unreachable") and never
# got it back; Lightsail has no automatic recovery, so the bot sat dead until a
# manual reboot. This script heals that on its own:
#   3 failed minutes in a row  -> restart systemd-networkd and systemd-resolved
#  10 failed minutes in a row  -> reboot the VM
# The counter lives in /run (tmpfs), so it starts from 0 after every boot.

STATE=/run/chokidar-netwatch.count

if ping -c 1 -W 5 1.1.1.1 >/dev/null 2>&1 || ping -c 1 -W 5 8.8.8.8 >/dev/null 2>&1; then
  if [ -s "$STATE" ] && [ "$(cat "$STATE")" != "0" ]; then
    logger -t chokidar-netwatch "network is back after $(cat "$STATE") failed checks"
  fi
  echo 0 > "$STATE"
  exit 0
fi

COUNT=$(( $(cat "$STATE" 2>/dev/null || echo 0) + 1 ))
echo "$COUNT" > "$STATE"
logger -t chokidar-netwatch "network check failed ($COUNT in a row)"

if [ "$COUNT" -eq 3 ]; then
  logger -t chokidar-netwatch "restarting systemd-networkd and systemd-resolved"
  systemctl restart systemd-networkd systemd-resolved
elif [ "$COUNT" -ge 10 ]; then
  logger -t chokidar-netwatch "network down for $COUNT minutes - rebooting"
  systemctl reboot
fi
