#!/usr/bin/env bash
set -e
# No test is excluded. sample_parity and layout_contract were once
# believed to fail on main; they pass, and both read only the source
# tree, so BUILD_SAMPLES=OFF above does not change their result.
parallel_args=()
if [[ "${ZLINK_SINGLE_WORKER}" == "true" ]]; then
  parallel_args=(--parallel 1)
fi
ctest --preset linux-ninja-release "${parallel_args[@]}" \
  -L '^(unit|contract)$'

# Tests labelled integration or extension that finish in seconds gate PRs too (#5865).
# The Redis ones use the Redis this job starts.
ctest --preset linux-ninja-release "${parallel_args[@]}" \
  -R '^(test_cpp_framework_parity_contract|test_cpp_framework_http_integration|test_cpp_framework_entry_spot_admission_burst|test_cpp_framework_stream_framework|test_cpp_framework_locations_redis|test_cpp_framework_location_store_counters_redis|connector_perf_smoke|test_cpp_http_client|test_unreal_stream_connector|test_unreal_adapter_push)$'

# Nightly only (#5865): the Godot and Axmol engine e2e tests start the .NET
# sample server with a Docker Redis, and the connector package tests install
# into a clean consumer; each takes 17-36 s, too long for every PR.
if [[ "${ZLINK_CPP_NIGHTLY}" == "true" ]]; then
  ctest --preset linux-ninja-release "${parallel_args[@]}" \
    -R '^(test_godot_stream_connector|test_axmol_stream_connector|test_cpp_stream_connector_install_consumer|test_unreal_stream_connector_package_contract)$'
fi
