#!/bin/bash
cd /root/worktree/zlink-v21-s033/r4203
T=framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj
F="FullyQualifiedName~OwnerDeadline"
export ZLINK_LIBRARY_PATH=/root/.cache/zlink/core-release/1.24.0/linux-x64/lib/libzlink.so
echo "== after"; dotnet test $T -c Release -f net8.0 --filter "$F" 2>&1 | grep -E "Passed!|Failed!|error|Failed " | head -20
git checkout -q origin/main -- framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/
echo "== before"; dotnet test $T -c Release -f net8.0 --filter "$F" 2>&1 | grep -E "Passed!|Failed!|error|Failed " | head -20
git checkout -q HEAD -- framework/languages/dotnet/src/Zlink.Framework/Runtime/Locations/
git status --short | head
