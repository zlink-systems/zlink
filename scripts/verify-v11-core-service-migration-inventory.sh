#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

node "$repo_root/scripts/generate-v11-core-service-migration-inventory.mjs" --check
