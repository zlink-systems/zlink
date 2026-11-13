#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/wire
while ! test -f "$job/ownership-before.log" || ! rg -q 'Assertion.*retained.parts.*payload.parts' "$job/ownership-before.log"; do sleep 1; done
cd /root/worktree/zlink-3867w
python3 "$job/edit_codec.py"
python3 "$job/edit_paths.py"
python3 "$job/test_native.py"
echo applied
