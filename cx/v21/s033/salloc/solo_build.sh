#!/bin/bash
# Run inside cpp-build-slot.sh; reserve both slots for the 12 GiB memory floor.
set -euo pipefail
held=''
ancestor=$$
while [[ $ancestor -gt 1 ]]; do
  found=$(lslocks -n -o PID,PATH | awk -v p="$ancestor" '$1==p && $2 ~ /cpp-build(\.2)?\.lock$/ {print $2}')
  held="$held $found"
  ancestor=$(awk '{print $4}' "/proc/$ancestor/stat")
done
if [[ $held == *'/cpp-build.lock'* && $held == *'/cpp-build.2.lock'* ]]; then
  exec "$@"
elif [[ $held == *'/cpp-build.lock'* ]]; then
  other=/root/jobs/fx/cx/cpp-build.2.lock
elif [[ $held == *'/cpp-build.2.lock'* ]]; then
  other=/root/jobs/fx/cx/cpp-build.lock
else
  echo 'No inherited C++ build slot' >&2
  exit 2
fi
exec flock "$other" "$@"
