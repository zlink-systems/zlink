#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
sync_script="$script_dir/native/update-zlink-libs.sh"
build_script="$script_dir/build-wsl.sh"

usage() {
  cat <<'USAGE'
Usage:
  scripts/local-package/publish-all-wsl.sh <release-url-or-tag> [--repo owner/repo] [--expect-version X.Y.Z]

Examples:
  scripts/local-package/publish-all-wsl.sh core/v9.0.0
  scripts/local-package/publish-all-wsl.sh core/v9.0.0 --repo kairos-code-dev/zlink --expect-version 9.0.0

This command performs one local release preparation flow:
  1. downloads verified Core release binaries and aligns Core version markers;
  2. creates .NET, Java/Kotlin, Node.js, and C++ local packages.

Binding package versions remain owned by each binding's package metadata.
The existing environment variables are passed through unchanged, including
ZLINK_LOCAL_PACKAGE_ROOT, CONFIGURATION, ZLINK_SKIP_NPM_CI,
ZLINK_NODE_PACKAGE_MODE, ZLINK_CPP_LOCAL_BUILD_DIR, and ZLINK_CPP_INSTALL_PREFIX.
USAGE
}

if [ "$#" -eq 0 ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
  usage
  exit 0
fi

if [ ! -x "$sync_script" ]; then
  echo "Missing native sync script: $sync_script" >&2
  exit 1
fi
if [ ! -x "$build_script" ]; then
  echo "Missing local package build script: $build_script" >&2
  exit 1
fi

echo "==> Synchronizing bindings from release: $1"
"$sync_script" "$@"

echo "==> Building all local bindings packages"
"$build_script"
