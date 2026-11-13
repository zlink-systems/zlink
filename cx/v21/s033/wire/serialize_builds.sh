#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/wire
while ! rg -q 'ninja: build stopped' "$job/implementation-build.log"; do sleep 1; done
touch "$job/memory-watch-stop"
echo 'Initial focused build finished; resuming the bench as the only build.'
