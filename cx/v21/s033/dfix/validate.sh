#!/usr/bin/env bash
set -euo pipefail
export MSBUILDDISABLENODEREUSE=1
repo=/root/worktree/zlink-3869
job=/root/jobs/fx/cx/v21/s033/dfix/raw
cd "$repo"
dotnet test framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj -c Release -p:UseSharedCompilation=false --filter 'FullyQualifiedName~MeshNodeShutdownSealTests|FullyQualifiedName~ProtobufNativePayloadTests|FullyQualifiedName~EnvelopeCodec|FullyQualifiedName~CustomSerializerEnvelopeTests|FullyQualifiedName~EnvelopeHeaderCacheHotPathTests|FullyQualifiedName~ClientServerChannelRuntimeTests|FullyQualifiedName~MeshCompletionTableConvergenceTests|FullyQualifiedName~MeshReplyPayloadOwnershipTests|FullyQualifiedName~FlowCorrelationTests' > "$job/focused-final.log" 2>&1
dotnet test framework/languages/dotnet/tests/Zlink.Framework.ContractTests/Zlink.Framework.ContractTests.csproj -c Release -p:UseSharedCompilation=false > "$job/contract-final.log" 2>&1
dotnet test framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj -c Release -p:UseSharedCompilation=false > "$job/unit-final.log" 2>&1
bash framework/bench/grpc/dotnet/build.sh > "$job/bench-final-build.log" 2>&1
