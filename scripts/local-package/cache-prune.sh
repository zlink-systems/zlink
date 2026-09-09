#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# --keep N (default 5); --dry-run performs no filesystem mutations.
exec python3 "$script_dir/package-cache.py" prune "$@"
