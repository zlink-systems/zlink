#!/bin/bash
# A/A 20 runs: exclusive measure lock (gate closes, builds paused), sender/receiver pinned (8,10), cpu8-15 governor performance during the run only.
J=/root/jobs/fx/cx/v21/s033/f4162
set_gov() { for c in 8 9 10 11 12 13 14 15; do echo $1 > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor; done; }
flock /root/jobs/fx/cx/measure.lock bash -c "
  $(declare -f set_gov)
  trap 'set_gov powersave' EXIT
  set_gov performance
  { date -u +%FT%TZ; uptime; free -g | sed -n 2p; cat /sys/devices/system/cpu/cpu8/cpufreq/scaling_governor; } > $J/raw/aa-clean-conditions-before.txt
  cd $J && python3 observe.py --runs 20 --label aa-clean --placement separate > $J/raw/aa-clean-driver.log 2>&1
  { date -u +%FT%TZ; uptime; free -g | sed -n 2p; } > $J/raw/aa-clean-conditions-after.txt
"
cat /sys/devices/system/cpu/cpu8/cpufreq/scaling_governor
