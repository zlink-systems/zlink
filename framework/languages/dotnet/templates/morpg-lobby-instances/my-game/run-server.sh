#!/usr/bin/env bash
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$ROOT/scripts/run-helpers.sh"

if [[ $# -ne 0 ]]; then
  echo 'Usage: ./run-server.sh' >&2
  exit 1
fi
echo 'Preparing: building the server (the first run restores NuGet packages)...'
build_local
echo 'Build complete. Starting servers...'
exec dotnet "$runner" --root "$ROOT" --serve
