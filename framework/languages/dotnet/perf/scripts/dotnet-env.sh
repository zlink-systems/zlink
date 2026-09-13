#!/usr/bin/env bash
# Public package provenance is verified by the common runner before application startup.
export ZLINK_CORE_PACKAGE_PREFIX="${ZLINK_CORE_PACKAGE_PREFIX:-${HOME}/.cache/zlink/core/0.18.0/linux-x64}"
export ZLINK_LIBRARY_PATH="${ZLINK_CORE_PACKAGE_PREFIX}/lib"
export LD_LIBRARY_PATH="${ZLINK_LIBRARY_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export UseSharedCompilation=false MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_TELEMETRY_OPTOUT=1
