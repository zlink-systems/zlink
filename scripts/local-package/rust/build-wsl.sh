#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [--core-prefix ABSOLUTE_DIR]

Creates zlink-0.11.0.crate with the Core 0.11.0 Linux runtime.
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
package_version="$(sed -n 's/^version = "\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)"/\1/p' "$repo_root/bindings/rust/Cargo.toml" | head -n1)"
[[ "$package_version" = "$version" ]] || {
  echo "Rust package version $package_version does not match Core $version" >&2
  exit 1
}

"$repo_root/scripts/local-package/native/sync-local-core-libs.sh" rust
native_dir="$repo_root/bindings/rust/native/linux-x86_64"
[[ -e "$native_dir/libzlink.so.0" && -e "$native_dir/libzlink.so.$version" ]] || {
  echo "Rust native payload is incomplete" >&2
  exit 1
}
(
  cd "$repo_root/bindings/rust"
  LD_LIBRARY_PATH="$native_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    cargo test --locked --workspace --all-targets
  cargo package --locked --allow-dirty --no-verify
)

crate="$repo_root/bindings/rust/target/package/zlink-$version.crate"
[[ -f "$crate" ]] || { echo "Rust crate is missing: $crate" >&2; exit 1; }
out_dir="$artifact_root/rust"
mkdir -p "$out_dir"
cp "$crate" "$out_dir/"
echo "Rust local package: $out_dir/zlink-$version.crate"
