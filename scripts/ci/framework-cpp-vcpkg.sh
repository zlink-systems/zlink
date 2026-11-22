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
    triplet=x64-linux-zlink-release
    overlay_triplets="${framework_dir}/cmake/vcpkg-triplets"
    cache_dir="${GITHUB_WORKSPACE}/.cache/framework-cpp"
    cache_prefix="framework-cpp-vcpkg-${RUNNER_OS}-${RUNNER_ARCH}-${revision}-"
    cache_hash="$(sha256sum "${framework_dir}/vcpkg.json" \
      "${overlay_triplets}/${triplet}.cmake" scripts/ci/framework-cpp-vcpkg.sh \
      | sha256sum | cut -d' ' -f1)"

    {
      echo "VCPKG_ROOT=${vcpkg_root}"
      echo "VCPKG_INSTALLED_DIR=${cache_dir}/vcpkg-installed"
      echo "VCPKG_ARCHIVE_DIR=${cache_dir}/vcpkg-archives"
      echo "ZLINK_VCPKG_TRIPLET=${triplet}"
      echo "VCPKG_OVERLAY_TRIPLETS=${GITHUB_WORKSPACE}/${overlay_triplets}"
    } >> "${GITHUB_ENV}"
    {
      echo "cache-key=${cache_prefix}${cache_hash}-v4"
      echo "restore-key=${cache_prefix}"
    } >> "${GITHUB_OUTPUT}"
    ;;
  install)
    # The system toolchain disables manifest installation. The gRPC port
    # supplies protobuf for both jobs, including the sanitizer build.
    mkdir -p "${VCPKG_ARCHIVE_DIR}"
    VCPKG_BINARY_SOURCES="clear;files,${VCPKG_ARCHIVE_DIR},readwrite" \
      bash scripts/ci/vcpkg-install.sh \
      "opentelemetry-cpp:${ZLINK_VCPKG_TRIPLET}" \
      "redis-plus-plus[async-std]:${ZLINK_VCPKG_TRIPLET}" \
      "grpc:${ZLINK_VCPKG_TRIPLET}" \
      --overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}" \
      --host-triplet="${ZLINK_VCPKG_TRIPLET}" \
      --x-install-root="${VCPKG_INSTALLED_DIR}" \
      --clean-after-build
    ;;
  *)
    echo "Usage: $0 {resolve|install}" >&2
    exit 2
    ;;
esac
