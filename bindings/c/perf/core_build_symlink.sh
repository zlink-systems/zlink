#!/usr/bin/env bash

perf_uses_external_core_build() {
  local root_dir="${1:?repository root required}"
  local core_build_dir="${2:?Core build directory required}"
  [[ -L "${root_dir}/core/build" ]] || return 1
  [[ "$(normalize_cmake_path "${core_build_dir}")" == "$(normalize_cmake_path "${root_dir}/core/build")" ]]
}
