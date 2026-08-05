#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LANGUAGE=""
MANIFEST=""
OUTPUT_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-$REPO_DIR/.artifacts/wsl}/bindings-candidate"
PACKAGE_VERSION=""
PYTHON_EXECUTABLE="${PYTHON_EXECUTABLE:-python3}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh --language python|go|rust --manifest FILE --package-version X.Y.Z [--output DIR]
       [--python-executable PATH]

Builds one non-release binding package and verifies it from a clean consumer.
Run languages separately in Python, Go, Rust order.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --language) LANGUAGE="${2:-}"; shift 2 ;;
    --manifest) MANIFEST="${2:-}"; shift 2 ;;
    --package-version) PACKAGE_VERSION="${2:-}"; shift 2 ;;
    --output) OUTPUT_ROOT="${2:-}"; shift 2 ;;
    --python-executable) PYTHON_EXECUTABLE="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$LANGUAGE" in python|go|rust) ;; *) echo "--language must be python, go, or rust" >&2; exit 2 ;; esac
[[ -f "$MANIFEST" ]] || { echo "Manifest not found: $MANIFEST" >&2; exit 1; }
[[ "$PACKAGE_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "--package-version must be X.Y.Z" >&2; exit 2; }

python_version="not_applicable"
python_executable_record="not_applicable"
if [[ "$LANGUAGE" == "python" ]]; then
  [[ -n "$PYTHON_EXECUTABLE" ]] || { echo "--python-executable must not be empty" >&2; exit 2; }
  command -v "$PYTHON_EXECUTABLE" >/dev/null 2>&1 || {
    echo "Python executable not found: $PYTHON_EXECUTABLE" >&2
    exit 1
  }
  python_version="$("$PYTHON_EXECUTABLE" -c 'import platform; print(platform.python_version())')"
  python_executable_record="$PYTHON_EXECUTABLE"
  "$PYTHON_EXECUTABLE" - <<'PY'
import sys

if sys.version_info < (3, 9):
    raise SystemExit("Python 3.9 or newer is required")
PY
fi

manifest_value() {
  local key="$1"
  sed -n "s/^${key}=//p" "$MANIFEST" | head -n1
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

layout_value() {
  local source binary values
  source="$(mktemp --suffix=.c)"
  binary="$(mktemp)"
  cat >"$source" <<'EOF'
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
  cc -std=c11 -I"$REPO_DIR/core/include" "$source" -o "$binary"
  values="$($binary | sort | paste -sd';' -)"
  rm -f "$source" "$binary"
  printf '%s\n' "$values"
}

verify_packaged_payload() {
  local packaged_payload="$1"
  [[ -f "$packaged_payload" ]] || {
    echo "Packaged native payload is missing: $packaged_payload" >&2
    exit 1
  }
  [[ "$(sha256sum "$packaged_payload" | awk '{print $1}')" == "$runtime_sha" ]] || {
    echo "Packaged native payload does not match candidate" >&2
    exit 1
  }
  printf 'PACKAGED_NATIVE_PAYLOAD_SHA256=%s\n' "$runtime_sha" >>"$OUTPUT_ROOT/$LANGUAGE/candidate-input.env"
}

verify_packaged_headers() {
  local packaged_headers="$1"
  local packaged_header_sha
  packaged_header_sha="$(dir_hash "$packaged_headers")"
  [[ "$packaged_header_sha" == "$header_sha" ]] || {
    echo "Packaged public headers do not match candidate" >&2
    exit 1
  }
  printf 'PACKAGED_HEADER_SHA256=%s\n' "$packaged_header_sha" >>"$OUTPUT_ROOT/$LANGUAGE/candidate-input.env"
}

core_version="$(manifest_value CORE_VERSION)"
core_revision="$(manifest_value CORE_REVISION)"
runtime_rel="$(manifest_value CORE_RUNTIME_PATH)"
runtime_sha="$(manifest_value CORE_RUNTIME_SHA256)"
header_sha="$(manifest_value CORE_HEADER_SHA256)"
spec_sha="$(manifest_value CORE_SPEC_SHA256)"
source_sha="$(manifest_value CORE_SOURCE_SHA256)"
symbol_sha="$(manifest_value CORE_SYMBOL_SHA256)"
soname="$(manifest_value CORE_SONAME)"
runtime_version="$(manifest_value CORE_RUNTIME_VERSION)"
layouts="$(manifest_value CORE_LAYOUTS)"
[[ "$(manifest_value FORMAT)" == 1 && -n "$core_version" && -n "$runtime_rel" ]] || {
  echo "Invalid candidate manifest: $MANIFEST" >&2
  exit 1
}

current_revision="$(git -C "$REPO_DIR" rev-parse HEAD)"
[[ "$current_revision" == "$core_revision" ]] || {
  echo "Core revision drift: manifest=$core_revision checkout=$current_revision" >&2
  exit 1
}

current_version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$REPO_DIR/VERSION")"
[[ "$current_version" == "$core_version" ]] || {
  echo "Core version drift: manifest=$core_version checkout=$current_version" >&2
  exit 1
}
runtime="$REPO_DIR/$runtime_rel"
[[ -f "$runtime" ]] || { echo "Manifest runtime is missing: $runtime" >&2; exit 1; }
[[ "$(sha256sum "$runtime" | awk '{print $1}')" == "$runtime_sha" ]] || {
  echo "Core runtime hash drift" >&2
  exit 1
}
current_header_sha="$(dir_hash "$REPO_DIR/core/include")"
[[ "$current_header_sha" == "$header_sha" ]] || { echo "Core public header hash drift" >&2; exit 1; }
current_spec_sha="$(dir_hash "$REPO_DIR/core/doc/spec/core")"
[[ "$current_spec_sha" == "$spec_sha" ]] || { echo "Core spec hash drift" >&2; exit 1; }
current_source_sha="$(repo_hash core/src core/include core/doc/spec/core)"
[[ "$current_source_sha" == "$source_sha" ]] || { echo "Core source snapshot drift" >&2; exit 1; }
[[ "$(nm -D --defined-only "$runtime" | awk '{print $3}' | sed -n '/^zlink_/p' | sort -u | sha256sum | awk '{print $1}')" == "$symbol_sha" ]] || { echo "Core symbol drift" >&2; exit 1; }
[[ "$(readelf -d "$runtime" | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p')" == "$soname" ]] || { echo "Core SONAME drift" >&2; exit 1; }
[[ "$(layout_value)" == "$layouts" ]] || { echo "Core public struct layout drift" >&2; exit 1; }
[[ "$runtime_version" == "$core_version" ]] || { echo "Manifest runtime version mismatch" >&2; exit 1; }
if find "$REPO_DIR/core/include" "$REPO_DIR/core/src" -type f -newer "$runtime" -print -quit | grep -q .; then
  echo "Core runtime became stale after manifest creation" >&2
  exit 1
fi

core_base="${core_version%.*}"
package_base="${PACKAGE_VERSION%.*}"
[[ "$core_base" == "$package_base" ]] || { echo "Binding package major.minor must match Core: $PACKAGE_VERSION vs $core_version" >&2; exit 1; }

rm -rf "$OUTPUT_ROOT/$LANGUAGE"
mkdir -p "$OUTPUT_ROOT/$LANGUAGE"
consumer="$(mktemp -d)"
trap 'rm -rf "$consumer"' EXIT

"$REPO_DIR/scripts/local-package/native/sync-local-core-libs.sh" "$LANGUAGE"
host_arch="$(uname -m)"
case "$host_arch" in x86_64|amd64) native_arch=x86_64 ;; aarch64|arm64) native_arch=aarch64 ;; *) echo "Unsupported host architecture: $host_arch" >&2; exit 2 ;; esac
case "$LANGUAGE" in
  python) payload_dir="$REPO_DIR/bindings/python/src/zlink/native/linux-$native_arch"; header_dir="" ;;
  go) payload_dir="$REPO_DIR/bindings/go/native/linux-$native_arch"; header_dir="$REPO_DIR/bindings/go/include" ;;
  rust) payload_dir="$REPO_DIR/bindings/rust/native/linux-$native_arch"; header_dir="$REPO_DIR/bindings/rust/include" ;;
esac
payload="$payload_dir/libzlink.so.$core_version"
[[ -f "$payload" && "$(sha256sum "$payload" | awk '{print $1}')" == "$runtime_sha" ]] || { echo "Binding native payload does not match candidate" >&2; exit 1; }

core_prefix=""
if [[ "$LANGUAGE" == "python" ]]; then
  core_prefix="$OUTPUT_ROOT/core-prefix/$core_version"
  rm -rf "$core_prefix"
  mkdir -p "$core_prefix/include" "$core_prefix/lib"
  cp -a "$REPO_DIR/core/include/." "$core_prefix/include/"
  cp "$runtime" "$core_prefix/lib/libzlink.so.$core_version"
  soname_name="${soname##*/}"
  ln -s "libzlink.so.$core_version" "$core_prefix/lib/$soname_name"
  ln -s "$soname_name" "$core_prefix/lib/libzlink.so"
  [[ "$(sha256sum "$core_prefix/lib/libzlink.so.$core_version" | awk '{print $1}')" == "$runtime_sha" ]] || {
    echo "Python Core build prefix does not match candidate" >&2
    exit 1
  }
fi

if [[ -n "$header_dir" ]]; then
  binding_header_sha="$(dir_hash "$header_dir")"
  core_direct_header_sha="$(dir_hash "$REPO_DIR/core/include")"
  [[ "$binding_header_sha" == "$core_direct_header_sha" ]] || { echo "Bundled header does not match Core" >&2; exit 1; }
fi

source_manifest=""
source_manifest_sha=""
source_aggregate_sha=""
if [[ "$LANGUAGE" == "python" ]]; then
  source_manifest="$OUTPUT_ROOT/python-source-manifest-$PACKAGE_VERSION.json"
  "$SCRIPT_DIR/create-python-source-manifest.sh" \
    --core-manifest "$MANIFEST" \
    --package-version "$PACKAGE_VERSION" \
    --output "$source_manifest"
  source_manifest_sha="$(sha256sum "$source_manifest" | awk '{print $1}')"
  source_aggregate_sha="$(python3 - "$source_manifest" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream)["aggregateSha256"])
PY
)"
fi

cat >"$OUTPUT_ROOT/$LANGUAGE/candidate-input.env" <<EOF
LANGUAGE=$LANGUAGE
PACKAGE_VERSION=$PACKAGE_VERSION
CORE_VERSION=$core_version
CORE_REVISION=$core_revision
CORE_SPEC_SHA256=$spec_sha
CORE_HEADER_SHA256=$header_sha
CORE_SOURCE_SHA256=$source_sha
CORE_RUNTIME_PATH=$runtime_rel
CORE_RUNTIME_SHA256=$runtime_sha
CORE_RUNTIME_VERSION=$runtime_version
CORE_SYMBOL_SHA256=$symbol_sha
CORE_SONAME=$soname
CORE_LAYOUTS=$layouts
CANDIDATE_MANIFEST_SHA256=$(sha256sum "$MANIFEST" | awk '{print $1}')
BINDING_HEADER_SHA256=${binding_header_sha:-not_applicable}
NATIVE_PAYLOAD=${payload#"$REPO_DIR/"}
NATIVE_PAYLOAD_SHA256=$(sha256sum "$payload" | awk '{print $1}')
PYTHON_CORE_PREFIX=${core_prefix:-not_applicable}
PYTHON_SOURCE_MANIFEST=${source_manifest:-not_applicable}
PYTHON_SOURCE_MANIFEST_SHA256=${source_manifest_sha:-not_applicable}
PYTHON_SOURCE_AGGREGATE_SHA256=${source_aggregate_sha:-not_applicable}
PYTHON_EXECUTABLE=$python_executable_record
PYTHON_VERSION=$python_version
EOF

case "$LANGUAGE" in
  python)
    package_version="$(sed -n 's/^version = "\(.*\)"/\1/p' "$REPO_DIR/bindings/python/pyproject.toml" | head -n1)"
    [[ "$package_version" == "$PACKAGE_VERSION" ]] || { echo "Python package version mismatch: $package_version != $PACKAGE_VERSION" >&2; exit 1; }
    (cd "$REPO_DIR/bindings/python" && PYTHON_EXECUTABLE="$PYTHON_EXECUTABLE" ZLINK_CORE_PREFIX="$core_prefix" ZLINK_LIBRARY_PATH="$payload" "$PYTHON_EXECUTABLE" setup.py build_ext --inplace)
    (cd "$REPO_DIR/bindings/python" && PYTHON_EXECUTABLE="$PYTHON_EXECUTABLE" ZLINK_CORE_PREFIX="$core_prefix" ZLINK_LIBRARY_PATH="$payload" ./tests/run_tests.sh)
    rm -rf "$OUTPUT_ROOT/$LANGUAGE/wheels"
    mkdir -p "$OUTPUT_ROOT/$LANGUAGE/wheels"
    (cd "$REPO_DIR/bindings/python" && PYTHON_EXECUTABLE="$PYTHON_EXECUTABLE" ZLINK_CORE_PREFIX="$core_prefix" ZLINK_LIBRARY_PATH="$payload" "$PYTHON_EXECUTABLE" -m pip wheel --no-deps --no-build-isolation --wheel-dir "$OUTPUT_ROOT/$LANGUAGE/wheels" .)
    wheel=("$OUTPUT_ROOT"/$LANGUAGE/wheels/*.whl)
    [[ ${#wheel[@]} -eq 1 && -f "${wheel[0]}" ]] || { echo "Expected exactly one Python wheel" >&2; exit 1; }
    mkdir -p "$consumer/wheel"
    python3 - "${wheel[0]}" "$consumer/wheel" <<'PY'
import pathlib
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1]) as wheel:
    wheel.extractall(pathlib.Path(sys.argv[2]))
PY
    python3 - "${wheel[0]}" "$core_version" "$native_arch" "$soname" <<'PY'
import pathlib
import sys
import zipfile

wheel_path = pathlib.Path(sys.argv[1])
core_version = sys.argv[2]
native_arch = sys.argv[3]
soname = sys.argv[4]
expected_prefix = f"zlink/native/linux-{native_arch}/"
with zipfile.ZipFile(wheel_path) as archive:
    names = set(archive.namelist())
    if "zlink/py.typed" not in names:
        raise SystemExit("Wheel is missing zlink/py.typed")
    native_names = {name for name in names if name.startswith("zlink/native/")}
    if not native_names or any(not name.startswith(expected_prefix) for name in native_names):
        raise SystemExit("Wheel contains an unexpected platform payload")
    forbidden = ("libzlink_c", ".so.10", ".so.9", ".dylib", ".dll", "core/build")
    if any(token in name for name in native_names for token in forbidden):
        raise SystemExit("Wheel contains an obsolete or cross-platform native payload")
    expected_runtime = f"{expected_prefix}libzlink.so.{core_version}"
    if expected_runtime not in names:
        raise SystemExit(f"Wheel is missing {expected_runtime}")
    metadata = [name for name in names if name.endswith(".dist-info/METADATA")]
    if len(metadata) != 1:
        raise SystemExit("Wheel metadata is missing or ambiguous")
PY
    packaged_payload="$consumer/wheel/zlink/native/linux-$native_arch/libzlink.so.$core_version"
    verify_packaged_payload "$packaged_payload"
    [[ "$(nm -D --defined-only "$packaged_payload" | awk '{print $3}' | sed -n '/^zlink_/p' | sort -u | sha256sum | awk '{print $1}')" == "$symbol_sha" ]] || {
      echo "Packaged Python payload symbol inventory does not match candidate" >&2
      exit 1
    }
    [[ "$(readelf -d "$packaged_payload" | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p')" == "$soname" ]] || {
      echo "Packaged Python payload SONAME does not match candidate" >&2
      exit 1
    }
    printf 'PACKAGED_HEADER_SHA256=not_applicable\n' >>"$OUTPUT_ROOT/$LANGUAGE/candidate-input.env"
    "$PYTHON_EXECUTABLE" -m venv "$consumer/venv"
    "$consumer/venv/bin/pip" install --no-deps "${wheel[0]}"
    (cd "$consumer" && env -u LD_LIBRARY_PATH -u ZLINK_LIBRARY_PATH PYTHONPATH= "$consumer/venv/bin/python" "$REPO_DIR/bindings/python/samples/run_samples.py" --installed)
    (cd "$consumer" && env -u LD_LIBRARY_PATH -u ZLINK_LIBRARY_PATH PYTHONPATH= "$consumer/venv/bin/python" - "$core_version" "$native_arch" <<'PY'
import pathlib
import sys

import zlink

expected_version = tuple(int(part) for part in sys.argv[1].split("."))
assert zlink.version() == expected_version
with zlink.create_context() as context:
    with zlink.create_pair_socket(context) as sender:
        with zlink.create_pair_socket(context) as receiver:
            endpoint = "inproc://python-candidate-clean-consumer"
            sender.bind(endpoint)
            receiver.connect(endpoint)
            assert sender.send().message(b"clean-consumer").submit()
            received = zlink.create_received()
            assert receiver.recv_into(received)
            with received:
                assert received.to_bytes_list() == [b"clean-consumer"]
maps = pathlib.Path("/proc/self/maps").read_text()
assert "/venv/" in maps and f"linux-{sys.argv[2]}" in maps and "libzlink" in maps
PY
    )
    source_recheck="$(mktemp)"
    "$SCRIPT_DIR/create-python-source-manifest.sh" \
      --core-manifest "$MANIFEST" \
      --package-version "$PACKAGE_VERSION" \
      --output "$source_recheck" >/dev/null
    cmp -s "$source_manifest" "$source_recheck" || {
      echo "Python source changed after source manifest creation" >&2
      exit 1
    }
    rm -f "$source_recheck"
    ;;
  go)
    archive="$OUTPUT_ROOT/go/zlink-go-$PACKAGE_VERSION.tar.gz"
    tar -C "$REPO_DIR/bindings" --exclude='go/.git' --exclude='go/perf/results' -czf "$archive" go
    mkdir -p "$consumer/pkg"
    tar -C "$consumer/pkg" -xzf "$archive"
    verify_packaged_payload "$consumer/pkg/go/native/linux-$native_arch/libzlink.so.$core_version"
    verify_packaged_headers "$consumer/pkg/go/include"
    (cd "$consumer/pkg/go" && go test ./... && go vet ./...)
    cat >"$consumer/go.mod" <<EOF
module zlink-candidate-consumer

go 1.25.12

require zlink.systems/zlink v0.0.0
replace zlink.systems/zlink => ./pkg/go
EOF
    cat >"$consumer/main.go" <<'EOF'
package main
import (
  "fmt"
  zlink "zlink.systems/zlink/contracts"
)
func main() { fmt.Println(zlink.RuntimeVersion()) }
EOF
    (cd "$consumer" && env -u LD_LIBRARY_PATH go build -o consumer . && ldd consumer | rg -F "$consumer/pkg/go/native/linux-$native_arch" && ./consumer)
    ;;
  rust)
    package_version="$(sed -n 's/^version = "\(.*\)"/\1/p' "$REPO_DIR/bindings/rust/Cargo.toml" | head -n1)"
    [[ "$package_version" == "$PACKAGE_VERSION" ]] || { echo "Rust package version mismatch: $package_version != $PACKAGE_VERSION" >&2; exit 1; }
    (cd "$REPO_DIR/bindings/rust" && cargo test --workspace --all-targets && cargo package --allow-dirty --no-verify)
    crate="$REPO_DIR/bindings/rust/target/package/zlink-$package_version.crate"
    cp "$crate" "$OUTPUT_ROOT/rust/"
    mkdir -p "$consumer/pkg"
    tar -C "$consumer/pkg" -xzf "$crate"
    verify_packaged_payload "$consumer/pkg/zlink-$package_version/native/linux-$native_arch/libzlink.so.$core_version"
    verify_packaged_headers "$consumer/pkg/zlink-$package_version/include"
    mkdir -p "$consumer/src"
    cat >"$consumer/Cargo.toml" <<EOF
[package]
name = "zlink-candidate-consumer"
version = "0.0.0"
edition = "2024"

[dependencies]
zlink = { path = "$consumer/pkg/zlink-$package_version" }
EOF
    cat >"$consumer/src/main.rs" <<'EOF'
fn main() { println!("{:?}", zlink::version()); }
EOF
    (cd "$consumer" && env -u LD_LIBRARY_PATH cargo build && ldd target/debug/zlink-candidate-consumer | rg -F "$consumer/pkg/zlink-$package_version/native/linux-$native_arch" && target/debug/zlink-candidate-consumer)
    ;;
esac

find "$OUTPUT_ROOT/$LANGUAGE" -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum >"$OUTPUT_ROOT/$LANGUAGE/SHA256SUMS"
echo "Candidate package and clean consumer passed: $LANGUAGE"
