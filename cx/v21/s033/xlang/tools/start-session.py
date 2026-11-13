import json,os,pathlib,sys,time
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');W=pathlib.Path('/root/worktree/zlink-xlang');G=W/'framework/bench/grpc'
if not (P/'BUILD_EXTRAS_DONE').exists():raise SystemExit('Build batch did not complete; no measurement has started.')
files=[G/'cpp/build/bench_cpp_client',G/'cpp/build/bench_cpp_framework_server',G/'cpp/build/bench_cpp_zlink_server',G/'java/client/build/install/bench-client/bin/bench-client',G/'java/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server',G/'java/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server',G/'dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll',G/'dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll',G/'dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll',W/'framework/languages/node/packages/framework/dist/index.js',W/'framework/languages/node/packages/nestjs/dist/index.js',P/'tools/alloc.so',P/'tools/jvm-alloc.so',P/'tools/node-alloc.node',P/'tools/eventpipe/bin/Release/net8.0/counter.dll']
missing=[str(f) for f in files if not f.is_file()];assert not missing,missing
assert 'count=10000 bytes=10400000' in (P/'jvm-jfr-counter-check.log').read_text()
assert json.loads((P/'node-counter-check.json').read_text())['count']>80000
# Remove the temporary continuation after its owning build process has ended.
(G/'cpp/build.sh').write_text((P/'tools/cpp-build.original.sh').read_text())
(P/'BUILD_DONE').touch()
(P/'preflight.json').write_text(json.dumps({'time':time.time(),'files':[{'path':str(f),'size':f.stat().st_size,'mtime':f.stat().st_mtime} for f in files]},indent=2))
os.execv(sys.executable,[sys.executable,str(P/'tools/session.py')])
