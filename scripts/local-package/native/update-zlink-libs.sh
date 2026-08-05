#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
SYNC_SCRIPT="${SCRIPT_DIR}/fetch-release-binaries.sh"
TAG_SCRIPT="${SCRIPT_DIR}/release-tag.sh"
# shellcheck source=release-tag.sh
source "${TAG_SCRIPT}"

usage() {
  cat <<'USAGE'
Usage:
  scripts/local-package/native/update-zlink-libs.sh <release-url-or-tag> [--repo owner/repo] [--expect-version X.Y.Z]

Examples:
  scripts/local-package/native/update-zlink-libs.sh core/v10.2.0-rc.1
  scripts/local-package/native/update-zlink-libs.sh https://github.com/kairos-code-dev/zlink/releases/tag/core/v10.2.0
  scripts/local-package/native/update-zlink-libs.sh core/v1.3.0 --repo kairos-code-dev/zlink --expect-version 1.3.0

Description:
  - Downloads zlink release assets.
  - Replaces internal native libraries in all bindings (cpp/dotnet/java/go/rust/node/python).
  - Updates copied Core headers and Core-version contract tests to expected version.
  - Does not change binding package versions. Each binding owns its package version.
  - Verifies linux-x64 native libraries exactly match expected Core version.
  - Does not run binding tests. Validate bindings separately after update.

Notes:
  - Requires GitHub CLI (`gh`) authentication.
  - `--repo` is optional. If omitted, repository is resolved from git origin.
  - Expected version is mandatory for safety:
      - Use --expect-version X.Y.Z, or
      - let the script infer numeric X.Y.Z from a stable or -rc.N tag/url.
  - Binding package versions may have a newer patch than the Core version.
USAGE
}

release_ref=""
repo_override="${ZLINK_RELEASE_REPO:-}"
expect_version=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --repo)
      if [[ $# -lt 2 ]]; then
        echo "Error: --repo requires owner/repo" >&2
        exit 1
      fi
      repo_override="$2"
      shift
      ;;
    --expect-version)
      if [[ $# -lt 2 ]]; then
        echo "Error: --expect-version requires X.Y.Z" >&2
        exit 1
      fi
      expect_version="$2"
      shift
      ;;
    --*)
      echo "Error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      if [[ -n "${release_ref}" ]]; then
        echo "Error: release reference already set: ${release_ref}" >&2
        usage >&2
        exit 1
      fi
      release_ref="$1"
      ;;
  esac
  shift
done

if [[ -z "${release_ref}" ]]; then
  echo "Error: release-url-or-tag is required" >&2
  usage >&2
  exit 1
fi

if [[ ! -f "${SYNC_SCRIPT}" ]]; then
  echo "Error: sync script not found: ${SYNC_SCRIPT}" >&2
  exit 1
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "Error: gh CLI is required." >&2
  exit 1
fi

resolve_repo() {
  if [[ -n "${repo_override}" ]]; then
    echo "${repo_override}"
    return
  fi

  local origin_url repo_path
  origin_url="$(git -C "${REPO_ROOT}" remote get-url origin 2>/dev/null || true)"
  if [[ -z "${origin_url}" ]]; then
    return
  fi
  repo_path="${origin_url}"
  repo_path="${repo_path#*://}"
  repo_path="${repo_path#*@}"
  if [[ "${repo_path}" == *:* ]]; then
    repo_path="${repo_path#*:}"
  else
    repo_path="${repo_path#*/}"
  fi
  repo_path="${repo_path%.git}"
  if [[ "${repo_path}" =~ ^[^/]+/[^/]+$ ]]; then
    echo "${repo_path}"
  fi
}

zlink_parse_core_release_ref "${release_ref}"
tag_name="${ZLINK_CORE_RELEASE_TAG}"
repo_name="$(resolve_repo)"
if [[ -z "${repo_name}" ]]; then
  echo "Error: unable to resolve repository. Use --repo owner/repo." >&2
  exit 1
fi

tag_version="${ZLINK_CORE_RUNTIME_VERSION}"
if [[ -z "${expect_version}" ]]; then
  expect_version="${tag_version}"
fi

if [[ ! "${expect_version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Error: invalid --expect-version '${expect_version}', expected X.Y.Z." >&2
  exit 1
fi
if [[ "${expect_version}" != "${tag_version}" ]]; then
  echo "Error: expected Core version ${expect_version} does not match release ${tag_version}." >&2
  exit 1
fi

assets="$(gh release view "${tag_name}" -R "${repo_name}" --json assets --jq '.assets[].name')"
required_assets=(
  "libzlink-linux-x64.tar.gz"
  "libzlink-linux-arm64.tar.gz"
  "libzlink-macos-x64.tar.gz"
  "libzlink-macos-arm64.tar.gz"
  "libzlink-windows-x64.tar.gz"
  "libzlink-windows-arm64.tar.gz"
  "libzlink_c-linux-x64.tar.gz"
  "libzlink_c-macos-x64.tar.gz"
  "libzlink_c-windows-x64.tar.gz"
  "checksums.txt"
  "release-provenance.txt"
  "zlink-${tag_version}-source.tar.gz"
)
missing_assets=()
optional_libzlink_c_assets_found=0
for a in "${required_assets[@]}"; do
  if grep -Fxq "${a}" <<< "${assets}"; then
    case "${a}" in
      libzlink_c-*.tar.gz)
        optional_libzlink_c_assets_found=$((optional_libzlink_c_assets_found + 1))
        ;;
    esac
    continue
  fi
  case "${a}" in
    libzlink_c-*.tar.gz)
      continue
      ;;
  esac
  missing_assets+=("${a}")
done
if [[ "${optional_libzlink_c_assets_found}" -eq 0 ]]; then
  echo "[WARN] libzlink_c assets not found in release, skipping"
fi
if [[ "${#missing_assets[@]}" -gt 0 ]]; then
  echo "Error: release '${tag_name}' (${repo_name}) is missing required core assets:" >&2
  printf '  - %s\n' "${missing_assets[@]}" >&2
  exit 1
fi

export ZLINK_RELEASE_REPO="${repo_name}"

py_bin=""
if command -v python3 >/dev/null 2>&1; then
  py_bin="python3"
elif command -v python >/dev/null 2>&1; then
  py_bin="python"
else
  echo "Error: python/python3 is required for version update/verification." >&2
  exit 1
fi

echo "[1/3] Syncing bindings native libraries from release: ${release_ref}"
bash "${SYNC_SCRIPT}" "${release_ref}"

echo "[2/3] Updating copied Core headers and Core-version tests to ${expect_version}"
REPO_ROOT="${REPO_ROOT}" EXPECT_VERSION="${expect_version}" "${py_bin}" - <<'PY'
import os
import re
import sys
from pathlib import Path

repo_root = Path(os.environ["REPO_ROOT"])
expect = os.environ["EXPECT_VERSION"]

parts = expect.split(".")
if len(parts) != 3 or not all(p.isdigit() for p in parts):
    print(f"Error: invalid EXPECT_VERSION '{expect}', expected X.Y.Z.", file=sys.stderr)
    sys.exit(1)
major, minor, patch = parts

errors = []
updated = []

def replace_regex(path: Path, pattern: str, repl: str, count: int = 1) -> None:
    if not path.exists():
        errors.append(f"{path}: file not found")
        return
    text = path.read_text(encoding="utf-8")
    new_text, n = re.subn(pattern, repl, text, count=count, flags=re.MULTILINE)
    if n == 0:
        errors.append(f"{path}: pattern not found ({pattern})")
        return
    if new_text != text:
        path.write_text(new_text, encoding="utf-8")
        updated.append(str(path.relative_to(repo_root)))

def replace_regex_optional(path: Path, pattern: str, repl: str,
                           count: int = 1) -> bool:
    if not path.exists():
        return False
    text = path.read_text(encoding="utf-8")
    new_text, n = re.subn(pattern, repl, text, count=count, flags=re.MULTILINE)
    if n == 0:
        return False
    if new_text != text:
        path.write_text(new_text, encoding="utf-8")
        updated.append(str(path.relative_to(repo_root)))
    return True

dotnet_version_test_candidates = [
    repo_root / "bindings/dotnet/tests/Zlink.Tests/VersionTests.cs",
    repo_root / "bindings/dotnet/tests/Zlink.Tests/test_system.cs",
]
java_version_test_candidates = [
    repo_root / "bindings/java/src/test/java/dev/kairoscode/zlink/VersionTest.java",
    repo_root / "bindings/java/src/test/java/dev/kairoscode/zlink/CoreVersionPortedTest.java",
]
node_version_test = repo_root / "bindings/node/tests/version.test.js"
python_version_test = repo_root / "bindings/python/tests/test_version.py"
native_manifest = repo_root / "bindings/native-artifacts.txt"

def pick_existing(paths):
    for p in paths:
        if p.exists():
            return p
    return None

dotnet_version_test = pick_existing(dotnet_version_test_candidates)
java_version_test = pick_existing(java_version_test_candidates)

dotnet_minor_updated = False
dotnet_patch_updated = False
dotnet_major_updated = False
if dotnet_version_test is not None:
    dotnet_major_updated = replace_regex_optional(
        dotnet_version_test,
        r"Assert\.Equal\(\d+,\s*major\);",
        f"Assert.Equal({major}, major);",
    )
    dotnet_minor_updated = replace_regex_optional(
        dotnet_version_test,
        r"Assert\.Equal\(\d+,\s*minor\);",
        f"Assert.Equal({minor}, minor);",
    )
    dotnet_patch_updated = replace_regex_optional(
        dotnet_version_test,
        r"Assert\.Equal\(\d+,\s*patch\);",
        f"Assert.Equal({patch}, patch);",
    )
if len({dotnet_major_updated, dotnet_minor_updated, dotnet_patch_updated}) > 1:
    errors.append(
        f"{dotnet_version_test}: partial dotnet version marker update "
        "(major/minor/patch pattern mismatch)"
    )

java_major_updated = False
java_minor_updated = False
java_patch_updated = False
if java_version_test is not None:
    java_major_updated = replace_regex_optional(
        java_version_test,
        r"assertEquals\(\d+,\s*v\[0\]\);",
        f"assertEquals({major}, v[0]);",
    )
    java_minor_updated = replace_regex_optional(
        java_version_test,
        r"assertEquals\(\d+,\s*v\[1\]\);",
        f"assertEquals({minor}, v[1]);",
    )
    java_patch_updated = replace_regex_optional(
        java_version_test,
        r"assertEquals\(\d+,\s*v\[2\]\);",
        f"assertEquals({patch}, v[2]);",
    )
if len({java_major_updated, java_minor_updated, java_patch_updated}) > 1:
    errors.append(
        f"{java_version_test}: partial java version marker update "
        "(major/minor/patch pattern mismatch)"
    )
elif not java_major_updated and not java_minor_updated and not java_patch_updated:
    # Newer java tests may read expected version directly from core/include/zlink.h.
    pass

replace_regex_optional(node_version_test, r"assert\.equal\(v\[0\],\s*\d+\);",
              f"assert.equal(v[0], {major});")
replace_regex_optional(node_version_test, r"assert\.equal\(v\[1\],\s*\d+\);",
              f"assert.equal(v[1], {minor});")
replace_regex_optional(node_version_test, r"assert\.equal\(v\[2\],\s*\d+\);",
              f"assert.equal(v[2], {patch});")

python_major_updated = replace_regex_optional(
    python_version_test,
    r"self\.assertEqual\(major,\s*\d+\)",
    f"self.assertEqual(major, {major})",
)
python_minor_updated = replace_regex_optional(
    python_version_test,
    r"self\.assertEqual\(minor,\s*\d+\)",
    f"self.assertEqual(minor, {minor})",
)
python_patch_updated = replace_regex_optional(
    python_version_test,
    r"self\.assertEqual\(patch,\s*\d+\)",
    f"self.assertEqual(patch, {patch})",
)
if len({python_major_updated, python_minor_updated, python_patch_updated}) > 1:
    errors.append(
        f"{python_version_test}: partial python version marker update "
        "(major/minor/patch pattern mismatch)"
    )
elif not python_major_updated and not python_minor_updated and not python_patch_updated:
    # Newer python tests may read expected version directly from core/include/zlink.h.
    pass

def update_version_header(path: Path) -> None:
    replace_regex(path, r"#define ZLINK_VERSION_MAJOR [0-9]+",
                  f"#define ZLINK_VERSION_MAJOR {major}")
    replace_regex(path, r"#define ZLINK_VERSION_MINOR [0-9]+",
                  f"#define ZLINK_VERSION_MINOR {minor}")
    replace_regex(path, r"#define ZLINK_VERSION_PATCH [0-9]+",
                  f"#define ZLINK_VERSION_PATCH {patch}")

c_top_header = repo_root / "bindings/c/include/zlink.h"
c_common = repo_root / "bindings/c/include/zlink/common.h"
c_common_version_test = repo_root / "bindings/c/tests/test_c_common_header_version.c"
c_contract_surface_test = repo_root / "bindings/c/tests/test_c_contract_surface.c"
go_top_header = repo_root / "bindings/go/include/zlink.h"
go_common = repo_root / "bindings/go/include/zlink/common.h"
go_contract_test = repo_root / "bindings/go/contract_test.go"
rust_top_header = repo_root / "bindings/rust/include/zlink.h"
rust_common = repo_root / "bindings/rust/include/zlink/common.h"

update_version_header(c_top_header)
update_version_header(c_common)
for path in (c_common_version_test, c_contract_surface_test):
    replace_regex(path, r"ZLINK_VERSION_MAJOR == [0-9]+",
                  f"ZLINK_VERSION_MAJOR == {major}")
    replace_regex(path, r"ZLINK_VERSION_MINOR == [0-9]+",
                  f"ZLINK_VERSION_MINOR == {minor}")
    replace_regex(path, r"ZLINK_VERSION_PATCH == [0-9]+",
                  f"ZLINK_VERSION_PATCH == {patch}")
    replace_regex(path, r"ZLINK_MAKE_VERSION \([0-9]+, [0-9]+, [0-9]+\)",
                  f"ZLINK_MAKE_VERSION ({major}, {minor}, {patch})")

update_version_header(go_top_header)
update_version_header(go_common)
replace_regex(go_contract_test,
              r'got != "[0-9]+"',
              f'got != "{patch}"')
replace_regex(go_contract_test,
              r'want [0-9]+"',
              f'want {patch}"')

update_version_header(rust_top_header)
update_version_header(rust_common)
go_native = repo_root / "bindings/go/native"
if go_native.exists():
    native_artifacts = sorted(
        str(path.relative_to(go_native))
        for path in go_native.rglob("*")
        if path.is_file() or path.is_symlink()
    )
    manifest_text = "\n".join(native_artifacts) + "\n"
    if native_manifest.read_text(encoding="utf-8") != manifest_text:
        native_manifest.write_text(manifest_text, encoding="utf-8")
        updated.append(str(native_manifest.relative_to(repo_root)))

if errors:
    print("Error: failed to update some version markers:", file=sys.stderr)
    for err in errors:
        print(f"  - {err}", file=sys.stderr)
    sys.exit(1)

if updated:
    print("Updated:")
    for path in sorted(set(updated)):
        print(f"  - {path}")
else:
    print("Updated: none (already up to date)")
PY

# cpp binding version markers (common.h fallback defines + contract test assertion)
IFS='.' read -r EV_MAJOR EV_MINOR EV_PATCH <<<"${expect_version}"
CPP_COMMON="${REPO_ROOT}/bindings/cpp/include/zlink/common.h"
CPP_TOP_HEADER="${REPO_ROOT}/bindings/cpp/include/zlink.h"
CPP_VER_TEST="${REPO_ROOT}/bindings/cpp/tests/contract/test_cpp_contract_common_header_version.cpp"
if [[ -f "${CPP_TOP_HEADER}" ]]; then
  sed -i -E "s/#define ZLINK_VERSION_MAJOR [0-9]+/#define ZLINK_VERSION_MAJOR ${EV_MAJOR}/; s/#define ZLINK_VERSION_MINOR [0-9]+/#define ZLINK_VERSION_MINOR ${EV_MINOR}/; s/#define ZLINK_VERSION_PATCH [0-9]+/#define ZLINK_VERSION_PATCH ${EV_PATCH}/" "${CPP_TOP_HEADER}"
fi
if [[ -f "${CPP_COMMON}" ]]; then
  sed -i -E "s/#define ZLINK_VERSION_MAJOR [0-9]+/#define ZLINK_VERSION_MAJOR ${EV_MAJOR}/; s/#define ZLINK_VERSION_MINOR [0-9]+/#define ZLINK_VERSION_MINOR ${EV_MINOR}/; s/#define ZLINK_VERSION_PATCH [0-9]+/#define ZLINK_VERSION_PATCH ${EV_PATCH}/" "${CPP_COMMON}"
fi
if [[ -f "${CPP_VER_TEST}" ]]; then
  sed -i -E "s/ZLINK_MAKE_VERSION\([0-9]+, [0-9]+, [0-9]+\)/ZLINK_MAKE_VERSION(${EV_MAJOR}, ${EV_MINOR}, ${EV_PATCH})/; s/ZLINK_VERSION_MAJOR == [0-9]+/ZLINK_VERSION_MAJOR == ${EV_MAJOR}/; s/ZLINK_VERSION_MINOR == [0-9]+/ZLINK_VERSION_MINOR == ${EV_MINOR}/; s/ZLINK_VERSION_PATCH == [0-9]+/ZLINK_VERSION_PATCH == ${EV_PATCH}/" "${CPP_VER_TEST}"
fi

echo "[3/3] Verifying linux-x64 zlink_version exactly matches ${expect_version}"

REPO_ROOT="${REPO_ROOT}" EXPECT_VERSION="${expect_version}" "${py_bin}" - <<'PY'
import ctypes
import os
import sys

repo_root = os.environ["REPO_ROOT"]
expect = os.environ["EXPECT_VERSION"]
expect_parts = expect.split(".")
if len(expect_parts) != 3 or not all(p.isdigit() for p in expect_parts):
    print(f"Error: invalid EXPECT_VERSION '{expect}', expected X.Y.Z.")
    sys.exit(1)
libs = {
    "python": os.path.join(repo_root, "bindings/python/src/zlink/native/linux-x86_64/libzlink.so"),
    "node": os.path.join(repo_root, "bindings/node/prebuilds/linux-x64/libzlink.so"),
    "dotnet": os.path.join(repo_root, "bindings/dotnet/native/linux-x64/libzlink.so"),
    "java": os.path.join(repo_root, "bindings/java/src/main/resources/native/linux-x86_64/libzlink.so"),
    "cpp": os.path.join(repo_root, "bindings/cpp/native/linux-x86_64/libzlink.so"),
    "c_binding": os.path.join(repo_root, "bindings/cpp/native/linux-x86_64/libzlink_c.so"),
    "go": os.path.join(repo_root, "bindings/go/native/linux-x86_64/libzlink.so"),
    "rust": os.path.join(repo_root, "bindings/rust/native/linux-x86_64/libzlink.so"),
}

failed = False
for name, path in libs.items():
    if not os.path.exists(path):
        if name == "c_binding":
            print(f"{name}: [WARN] MISSING ({path})")
            continue
        print(f"{name}: MISSING ({path})")
        failed = True
        continue
    if name == "c_binding":
        print(f"{name}: PRESENT ({path})")
        continue
    lib = ctypes.CDLL(os.path.abspath(path))
    fn = lib.zlink_version
    fn.argtypes = [
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.c_int),
    ]
    fn.restype = None
    major, minor, patch = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
    fn(ctypes.byref(major), ctypes.byref(minor), ctypes.byref(patch))
    actual = f"{major.value}.{minor.value}.{patch.value}"
    if actual == expect:
        print(f"{name}: {actual}")
    else:
        print(f"{name}: VERSION_MISMATCH (actual={actual}, expected={expect})")
        failed = True

if failed:
    sys.exit(1)
PY

echo "Done: binding native libraries and Core-version markers updated (core=${expect_version})."
