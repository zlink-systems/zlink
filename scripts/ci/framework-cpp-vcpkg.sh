#!/usr/bin/env bash
set -euo pipefail

cd "${GITHUB_WORKSPACE}"

case "${1:-}" in
  resolve)
    vcpkg_root="${VCPKG_INSTALLATION_ROOT:-/usr/local/share/vcpkg}"
    test -x "${vcpkg_root}/vcpkg"
    test -f "${vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
    revision="$(git -C "${vcpkg_root}" rev-parse HEAD 2>/dev/null \
      || "${vcpkg_root}/vcpkg" version | sed -n '1p' | sha256sum | cut -d' ' -f1)"

    framework_dir=framework/languages/cpp
    overlay_triplets="${framework_dir}/cmake/vcpkg-triplets"
    # Build tools such as protoc come from the release host triplet. protobuf
    # turns on debug-only code paths in generated code whenever a sanitizer is
    # active, so the sanitizer Framework links protobuf built with the same
    # sanitizer flags (#5177).
    host_triplet=x64-linux-zlink-release
    case "${2:-release}" in
      release)
        triplet=x64-linux-zlink-release
        cache_name=framework-cpp
        ;;
      sanitizer)
        triplet=x64-linux-zlink-sanitizer
        cache_name=framework-cpp-sanitizer
        ;;
      *)
        echo "Unknown vcpkg variant: ${2}" >&2
        exit 2
        ;;
    esac
    cache_dir="${GITHUB_WORKSPACE}/.cache/${cache_name}"
    cache_prefix="${cache_name}-vcpkg-${RUNNER_OS}-${RUNNER_ARCH}-${revision}-"
    cache_hash="$(sha256sum "${framework_dir}/vcpkg.json" \
      "${overlay_triplets}/${host_triplet}.cmake" \
      "${overlay_triplets}/${triplet}.cmake" scripts/ci/framework-cpp-vcpkg.sh \
      | sha256sum | cut -d' ' -f1)"

    {
      echo "VCPKG_ROOT=${vcpkg_root}"
      echo "VCPKG_INSTALLED_DIR=${cache_dir}/vcpkg-installed"
      echo "VCPKG_ARCHIVE_DIR=${cache_dir}/vcpkg-archives"
      echo "ZLINK_VCPKG_TRIPLET=${triplet}"
      echo "ZLINK_VCPKG_HOST_TRIPLET=${host_triplet}"
      echo "VCPKG_OVERLAY_TRIPLETS=${GITHUB_WORKSPACE}/${overlay_triplets}"
    } >> "${GITHUB_ENV}"
    {
      echo "cache-key=${cache_prefix}${cache_hash}-v5"
      echo "restore-key=${cache_prefix}"
    } >> "${GITHUB_OUTPUT}"
    ;;
  install)
    # The system toolchain disables manifest installation, and the other
    # Framework dependencies come from apt. A manifest made from vcpkg.json
    # with only these ports keeps its builtin-baseline and overrides (protobuf,
    # abseil, gRPC), so Linux builds the same versions as Windows (#5337).
    # Classic-mode `vcpkg install <port>` ignored them and took the runner
    # port versions.
    framework_manifest=framework/languages/cpp/vcpkg.json
    manifest_root="${RUNNER_TEMP}/framework-cpp-vcpkg-manifest"
    mkdir -p "${VCPKG_ARCHIVE_DIR}" "${manifest_root}"
    baseline="$(python3 - "${framework_manifest}" "${manifest_root}/vcpkg.json" <<'PY'
import json
import sys

source, target = sys.argv[1:]
with open(source, encoding="utf-8") as handle:
    manifest = json.load(handle)
ci_ports = ["opentelemetry-cpp", "redis-plus-plus", "grpc", "protobuf"]
dependencies = [
    dep for dep in manifest["dependencies"]
    if (dep if isinstance(dep, str) else dep["name"]) in ci_ports
]
dependencies += manifest["features"]["etcd"]["dependencies"]
names = sorted(dep if isinstance(dep, str) else dep["name"] for dep in dependencies)
if names != sorted(ci_ports):
    sys.exit(f"{source} no longer lists the CI vcpkg ports {ci_ports}: {names}")
ci_manifest = {
    "name": "zlink-framework-cpp-ci",
    "version-string": manifest["version-string"],
    "builtin-baseline": manifest["builtin-baseline"],
    "dependencies": dependencies,
    "overrides": manifest["overrides"],
}
with open(target, "w", encoding="utf-8") as handle:
    json.dump(ci_manifest, handle, indent=2)
print(manifest["builtin-baseline"])
PY
)"
    if ! git -C "${VCPKG_ROOT}" cat-file -e "${baseline}^{commit}" 2>/dev/null; then
      echo "::error::${VCPKG_ROOT} has no commit ${baseline} (builtin-baseline of ${framework_manifest})"
      exit 1
    fi
    VCPKG_BINARY_SOURCES="clear;files,${VCPKG_ARCHIVE_DIR},readwrite" \
      bash scripts/ci/vcpkg-install.sh \
      --x-manifest-root="${manifest_root}" \
      --triplet="${ZLINK_VCPKG_TRIPLET}" \
      --overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}" \
      --host-triplet="${ZLINK_VCPKG_HOST_TRIPLET}" \
      --x-install-root="${VCPKG_INSTALLED_DIR}" \
      --clean-after-build
    ;;
  verify)
    # A cache restored from an older key, or a changed install path, must not
    # leave Linux on other versions than vcpkg.json pins.
    python3 - framework/languages/cpp/vcpkg.json \
      "${VCPKG_INSTALLED_DIR}/vcpkg/status" "${ZLINK_VCPKG_TRIPLET}" <<'PY'
import json
import sys

manifest_path, status_path, triplet = sys.argv[1:]
with open(manifest_path, encoding="utf-8") as handle:
    overrides = {
        item["name"]: (item["version"], item.get("port-version", 0))
        for item in json.load(handle)["overrides"]
    }
installed = {}
with open(status_path, encoding="utf-8") as handle:
    for block in handle.read().split("\n\n"):
        fields = dict(
            line.split(": ", 1) for line in block.splitlines() if ": " in line
        )
        if fields.get("Architecture") == triplet and "Feature" not in fields \
                and fields.get("Status", "").endswith(" installed"):
            installed[fields["Package"]] = (
                fields["Version"], int(fields.get("Port-Version", "0"))
            )
failed = False
for name in ("protobuf", "abseil", "grpc"):
    want, have = overrides[name], installed.get(name)
    print(f"{name}:{triplet} installed={have} vcpkg.json={want}")
    failed |= have != want
if failed:
    sys.exit(f"{status_path} does not match the overrides in {manifest_path}")
PY
    ;;
  *)
    echo "Usage: $0 {resolve [release|sanitizer]|install|verify}" >&2
    exit 2
    ;;
esac
