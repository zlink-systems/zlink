#!/bin/bash
cd /root/worktree/zlink-v21-s033/r4251
T=framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj
F="FullyQualifiedName~PrestartCallTests|FullyQualifiedName~EntrySpotActorDispatchTests"
export ZLINK_LIBRARY_PATH=/root/.cache/zlink/core-release/1.24.0/linux-x64/lib/libzlink.so
S=framework/languages/dotnet/src
echo "== after"; dotnet test $T -c Release -f net8.0 --filter "$F" 2>&1 | grep -E "Passed!|Failed!|error CS|^\s+Failed " | head -20
git checkout -q 939b7d671bd1d856af009d0b8e7a81b43dee1cfe -- $S
echo "== before (src at 939b7d671bd1d856af009d0b8e7a81b43dee1cfe)"; dotnet test $T -c Release -f net8.0 --filter "$F" 2>&1 | grep -E "Passed!|Failed!|error CS|^\s+Failed " | head -40
git checkout -q HEAD -- $S
git status --short | head
