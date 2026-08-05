#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
OUTPUT="${1:-}"

if [[ -z "$OUTPUT" ]]; then
  echo "Usage: $0 <output-manifest>" >&2
  exit 2
fi

version_value() {
  sed -n 's/^LIBZLINK_VERSION=//p' "$REPO_DIR/VERSION"
}

dir_hash() {
  local root="$1"
  (
    cd "$root"
    find . -type f -print0 | sort -z | while IFS= read -r -d '' file; do
      printf '%s  %s\n' "$(sha256sum "$file" | awk '{print $1}')" "${file#./}"
    done
  ) | sha256sum | awk '{print $1}'
}

repo_hash() {
  (
    cd "$REPO_DIR"
    find "$@" -type f -print0 | sort -z | while IFS= read -r -d '' file; do
      printf '%s  %s\n' "$(sha256sum "$file" | awk '{print $1}')" "$file"
    done
  ) | sha256sum | awk '{print $1}'
}

version="$(version_value)"
runtime="$(readlink -f "$REPO_DIR/core/build/lib/libzlink.so" 2>/dev/null || true)"
if [[ -z "$version" || -z "$runtime" || ! -f "$runtime" ]]; then
  echo "Core version or official runtime is missing." >&2
  exit 1
fi
if find "$REPO_DIR/core/include" "$REPO_DIR/core/src" -type f -newer "$runtime" -print -quit | grep -q .; then
  echo "Core runtime is older than core/include or core/src: $runtime" >&2
  exit 1
fi
if [[ "$(basename "$runtime")" != "libzlink.so.$version" ]]; then
  echo "Core runtime filename does not match VERSION: $(basename "$runtime") != libzlink.so.$version" >&2
  exit 1
fi

runtime_version="$(python3 - "$runtime" <<'PY'
import ctypes
import sys
lib = ctypes.CDLL(sys.argv[1])
major = ctypes.c_int()
minor = ctypes.c_int()
patch = ctypes.c_int()
lib.zlink_version(ctypes.byref(major), ctypes.byref(minor), ctypes.byref(patch))
print(f"{major.value}.{minor.value}.{patch.value}")
PY
)"
[[ "$runtime_version" == "$version" ]] || {
  echo "Runtime zlink_version does not match VERSION: $runtime_version != $version" >&2
  exit 1
}

layout_source="$(mktemp --suffix=.c)"
layout_bin="$(mktemp)"
tmp="$(mktemp)"
trap 'rm -f "$tmp" "$layout_source" "$layout_bin"' EXIT
cat >"$layout_source" <<'EOF'
#include <stddef.h>
#include <stdio.h>
#include <stdalign.h>
#include <zlink.h>
#define SHOW(T) printf(#T "=%zu/%zu\n", sizeof(T), alignof(T))
int main(void) {
  SHOW(zlink_msg_t); SHOW(zlink_routing_id_t);
  SHOW(zlink_monitor_event_t); SHOW(zlink_socket_monitor_open_options_t);
  SHOW(zlink_monitor_status_t); SHOW(zlink_pollitem_t);
  SHOW(zlink_poller_event_t); return 0;
}
EOF
cc -std=c11 -I"$REPO_DIR/core/include" "$layout_source" -o "$layout_bin"
layout_values="$($layout_bin | sort | paste -sd';' -)"

cat >"$tmp" <<EOF
FORMAT=1
CORE_VERSION=$version
CORE_REVISION=$(git -C "$REPO_DIR" rev-parse HEAD)
CORE_SPEC_SHA256=$(dir_hash "$REPO_DIR/core/doc/spec/core")
CORE_HEADER_SHA256=$(dir_hash "$REPO_DIR/core/include")
CORE_SOURCE_SHA256=$(repo_hash core/src core/include core/doc/spec/core)
CORE_RUNTIME_PATH=${runtime#"$REPO_DIR/"}
CORE_RUNTIME_SHA256=$(sha256sum "$runtime" | awk '{print $1}')
CORE_RUNTIME_VERSION=$runtime_version
CORE_SYMBOL_SHA256=$(nm -D --defined-only "$runtime" | awk '{print $3}' | sed -n '/^zlink_/p' | sort -u | sha256sum | awk '{print $1}')
CORE_SONAME=$(readelf -d "$runtime" | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p')
CORE_LAYOUTS=$layout_values
EOF
install -m 0644 "$tmp" "$OUTPUT"
echo "Candidate manifest: $OUTPUT"
