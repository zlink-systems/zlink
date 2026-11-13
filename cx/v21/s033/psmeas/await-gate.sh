#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/psmeas
verdict=/root/jobs/fx/cx/v21/s033/core124b/verdict.md
while [[ ! -f "$verdict" ]]; do sleep 60; done
printf 'Core gate verdict found: %s\n' "$verdict"
bash "$job/build-after-gate.sh"
