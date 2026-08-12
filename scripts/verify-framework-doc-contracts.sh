#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Keep the documentation gate focused on the current public contracts. The
# former migration inventory and trace generator are no longer part of the
# release workflow.
bash "$script_dir/verify-framework-instance-spot-contracts.sh" --check
bash "$script_dir/verify-framework-submit-api.sh" --contract
bash "$script_dir/verify-framework-submit-api.sh" --implementation
python3 "$script_dir/verify-framework-runner-isolation.py" --check
node "$script_dir/run-framework-relocation-conformance.mjs" --list
