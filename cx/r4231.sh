#!/bin/bash
cd /root/worktree/zlink-v21-s033/r4231
T=framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj
F="FullyQualifiedName~PausedSerialExecution|FullyQualifiedName~LocationRuntimeTests"
export ZLINK_LIBRARY_PATH=/root/.cache/zlink/core-release/1.24.0/linux-x64/lib/libzlink.so
S=framework/languages/dotnet/src/Zlink.Framework/Runtime
echo "== after"; dotnet test $T -c Release -f net8.0 --filter "$F" 2>&1 | grep -E "Passed!|Failed!|error CS|^\s+Failed " | head -20
git checkout -q 8ee092fdd1 -- $S/Execution/
echo "== before (Execution at 8ee092fdd1)"; dotnet test $T -c Release -f net8.0 --filter "$F" 2>&1 | grep -E "Passed!|Failed!|error CS|^\s+Failed " | head -30
git checkout -q HEAD -- $S/Execution/
git status --short | head
