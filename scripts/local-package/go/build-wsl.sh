#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
python3 "$repo_root/scripts/local-package/sync-version.py" --check >/dev/null
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [--core-prefix ABSOLUTE_DIR]

Creates zlink-go-0.11.1.tar.gz containing the Go module source, headers, and
the Core 0.11.1 Linux runtime. The Go module path is zlink.systems/zlink.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be absolute" >&2; exit 2; }
core_prefix="$(readlink -f "$core_prefix")"
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
export ZLINK_CORE_PACKAGE_PREFIX="$core_prefix"
export ZLINK_CORE_VERSION="$version"
module_path="$(sed -n 's/^module //p' "$repo_root/bindings/go/go.mod" | head -n1)"
[[ "$module_path" = "zlink.systems/zlink" ]] || {
  echo "Go module path is not zlink.systems/zlink" >&2
  exit 1
}

"$repo_root/scripts/local-package/native/sync-local-core-libs.sh" go
native_dir="$repo_root/bindings/go/native/linux-x86_64"
for file in libzlink.so libzlink.so.0 "libzlink.so.$version"; do
  [[ -e "$native_dir/$file" ]] || { echo "Missing Go runtime: $native_dir/$file" >&2; exit 1; }
done

(
  cd "$repo_root/bindings/go"
  LD_LIBRARY_PATH="$native_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" go test ./...
)

out_dir="$artifact_root/go"
stage="$out_dir/stage/zlink-$version"
archive="$out_dir/zlink-go-$version.tar.gz"
rm -rf "$stage" "$archive"
mkdir -p "$stage"
cp -a "$repo_root/bindings/go/." "$stage/"
rm -rf "$stage/perf/results" "$stage/.git"
mkdir -p "$stage/provenance"
cp "$core_prefix/share/zlink/core-package-provenance.json" "$stage/provenance/core-package-provenance.json"
tar -C "$out_dir/stage" -czf "$archive" "zlink-$version"
rm -rf "$out_dir/stage"
echo "Go local package: $archive"
