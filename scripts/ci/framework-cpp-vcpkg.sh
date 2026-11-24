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
    mkdir -p "${VCPKG_ARCHIVE_DIR}"
    VCPKG_BINARY_SOURCES="clear;files,${VCPKG_ARCHIVE_DIR},readwrite" \
      bash scripts/ci/vcpkg-install.sh \
      --x-manifest-root=framework/languages/cpp \
      --x-feature=etcd \
      --triplet="${ZLINK_VCPKG_TRIPLET}" \
      --overlay-triplets="${VCPKG_OVERLAY_TRIPLETS}" \
      --host-triplet="${ZLINK_VCPKG_HOST_TRIPLET}" \
      --x-install-root="${VCPKG_INSTALLED_DIR}" \
      --clean-after-build
    ;;
  verify)
    python3 scripts/ci/framework-cpp-verify-dependencies.py \
      "${VCPKG_INSTALLED_DIR}/vcpkg/status" "${ZLINK_VCPKG_TRIPLET}" --with-etcd
    ;;
  *)
    echo "Usage: $0 {resolve [release|sanitizer]|install|verify}" >&2
    exit 2
    ;;
esac
