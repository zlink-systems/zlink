#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# This directory is a byte-identical copy of core/tests/certs/gen/{ca.crt,
# server.crt,server.key}, consumed by
# bindings/dotnet/perf/common/Zlink.BindingBench.Common/PerfTls.cs.
# There is no independent generation here: run core/tests/certs/gen/gen.sh
# to produce a new chain, then run this script to copy it over.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$here" rev-parse --show-toplevel)"
src="$repo_root/core/tests/certs/gen"

cp "$src/ca.crt" "$src/server.crt" "$src/server.key" "$here/"
echo "Synced ca.crt, server.crt, server.key from $src into $here"
