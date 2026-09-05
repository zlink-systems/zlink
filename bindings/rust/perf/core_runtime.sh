#!/usr/bin/env bash

prepare_core_runtime() {
    local native_dir="${ZLINK_RUST_NATIVE_DIR:-${CORE_LIB_DIR}}"
    local runtime="${native_dir}/libzlink.so"
    [[ -f "${runtime}" ]] || runtime="${native_dir}/libzlink.so.${ZLINK_CORE_VERSION}"
    if [[ ! -f "${runtime}" ]]; then
        echo "Rust perf runtime not found: ${native_dir}" >&2
        echo "Build core/build or set ZLINK_RUST_NATIVE_DIR." >&2
        exit 1
    fi
    local resolved_lib
    resolved_lib="$(readlink -f "${runtime}" 2>/dev/null || echo "${runtime}")"
    if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 0 ]]; then
        # A shared build belongs to its CMake source tree. Worktree checkout
        # timestamps do not describe when those same sources were changed.
        local source_dir="${REPO_DIR}/core"
        local cache="$(dirname "$(dirname "${resolved_lib}")")/CMakeCache.txt"
        local build_source=""
        if [[ -f "${cache}" ]]; then
            build_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cache}")"
        fi
        if [[ -n "${build_source}" && "${build_source}" != "${source_dir}" ]]; then
            if ! diff -qr "${source_dir}/src" "${build_source}/src" >/dev/null ||
               ! diff -qr "${source_dir}/include" "${build_source}/include" >/dev/null; then
                echo "Error: workspace Core sources differ from runtime build sources: ${build_source}" >&2
                return 1
            fi
            source_dir="${build_source}"
        fi
        local newer_source
        newer_source="$(find "${source_dir}/src" "${source_dir}/include" \
            -type f -newer "${resolved_lib}" -print -quit)" || return 1
        if [[ -n "${newer_source}" ]]; then
            echo "Error: stale core runtime detected for bindings/rust/perf." >&2
            echo "  runtime: ${resolved_lib}" >&2
            echo "  newer source: ${newer_source}" >&2
            return 1
        fi
    fi
    echo "Rust perf runtime: ${resolved_lib}"
    echo "Rust perf runtime sha256: $(sha256sum "${resolved_lib}" | awk '{print $1}')"
    export PERF_CORE_SOURCE="${ZLINK_CORE_SOURCE}"
    export PERF_CORE_VERSION="${ZLINK_CORE_VERSION}"
    export PERF_CORE_RUNTIME="${resolved_lib}"
    export ZLINK_RUST_NATIVE_DIR="${native_dir}"
    export LD_LIBRARY_PATH="${native_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
}
