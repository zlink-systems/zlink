#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"
python_executable="${PYTHON_EXECUTABLE:-python3}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [--core-prefix ABSOLUTE_DIR] [--python-executable PATH]

Creates zlink-0.10.0 source and wheel packages with the Core 0.10.0 Linux
runtime. The current Python native target is Linux x86_64.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    --python-executable) python_executable="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be absolute" >&2; exit 2; }
core_prefix="$(readlink -f "$core_prefix")"
command -v "$python_executable" >/dev/null 2>&1 || {
  echo "Python executable not found: $python_executable" >&2
  exit 1
}
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
package_version="$(sed -n 's/^version = "\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)"/\1/p' "$repo_root/bindings/python/pyproject.toml" | head -n1)"
[[ "$package_version" = "$version" ]] || {
  echo "Python package version $package_version does not match Core $version" >&2
  exit 1
}

"$repo_root/scripts/local-package/native/sync-local-core-libs.sh" python
(
  cd "$repo_root/bindings/python"
  rm -rf build dist
  mkdir -p "$artifact_root/python"
  export ZLINK_CORE_PREFIX="$core_prefix"
  if "$python_executable" -c 'import build' >/dev/null 2>&1; then
    "$python_executable" -m build --sdist --wheel --outdir "$artifact_root/python"
  else
    "$python_executable" -m pip wheel --no-deps --no-build-isolation \
      --wheel-dir "$artifact_root/python" .
    "$python_executable" setup.py sdist --dist-dir "$artifact_root/python"
  fi
)

wheel="$artifact_root/python/zlink-$version-py3-none-any.whl"
if [[ ! -f "$wheel" ]]; then
  wheel="$(find "$artifact_root/python" -maxdepth 1 -type f -name "zlink-$version-*.whl" -print -quit)"
fi
[[ -n "$wheel" && -f "$wheel" ]] || { echo "Python wheel is missing" >&2; exit 1; }
echo "Python local packages: $artifact_root/python"
