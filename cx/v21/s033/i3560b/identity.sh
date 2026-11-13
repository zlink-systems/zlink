#!/bin/bash
set -euo pipefail
cd /root/worktree/zlink-v21-s033/i3560b
git rev-parse HEAD
git branch --show-current
git status --short
git diff --stat 0005a4d6c07160bbf661a6f7aea3d8093880514e HEAD -- core/src/runtime/core core/src/runtime/sockets/common core/src/api/socket/socket_request_reply_internal.cpp
git diff --stat 76a4d7850664c2cb32e8d35837dac45d9d2a4057 HEAD -- framework/languages/cpp/framework/src/runtime bindings/cpp/src framework/bench/grpc/cpp/client/bench_cpp_client.cpp
sha256sum /root/jobs/fx/cx/v21/s033/core124/prefix-1.24/lib/libzlink.so.1.23.0
cat /root/jobs/fx/cx/v21/s033/core124/prefix-1.24/share/zlink/core-package-provenance.json
