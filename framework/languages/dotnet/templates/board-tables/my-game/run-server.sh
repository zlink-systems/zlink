#!/usr/bin/env bash
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$ROOT/run-e2e.sh" --local --build-only
exec dotnet "$ROOT/e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll" --root "$ROOT" --serve
