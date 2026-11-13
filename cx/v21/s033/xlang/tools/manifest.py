import hashlib,json,pathlib,subprocess,time
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');W=pathlib.Path('/root/worktree/zlink-xlang');G=W/'framework/bench/grpc'
files=[]
files+=list((G/'cpp/build').glob('bench_cpp_*'))
files+=list((G/'java/client/build/install/bench-client/lib').glob('*.jar'))
files+=list((G/'java/zlink-framework-server/build/install/bench-zlink-framework-server/lib').glob('*framework*.jar'))
files+=list((G/'dotnet/Client/bin/Release/net8.0').glob('*.dll'))
files+=list((G/'dotnet/ZLinkServer/bin/Release/net8.0').glob('Zlink.Framework*.dll'))
files+=list((P/'tools').glob('*.so'))+list((P/'tools').glob('*.node'))
files+=[G/'node/client/bench-core.js',G/'cpp/client/bench_cpp_client.cpp',G/'dotnet/Client/BenchMeasurement.cs',G/'java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java']
for f in (P/'runs').glob('*/*/before.json'):
 x=json.loads(f.read_text())
 for proc in x['processes'].values():
  for l in proc['maps'].splitlines():
   if 'libzlink' in l or 'zlink.node' in l:
    p=l.split()[-1]
    if p.startswith('/'):files.append(pathlib.Path(p))
for module in ['framework','framework-codec-protobuf']:
 files+=list((W/'framework/languages/node/packages'/module/'dist').rglob('*.js'))
files+=list((P/'symbols').glob('*.map'))
rows=[]
for f in sorted(set(files)):
 if not f.is_file():continue
 with f.open('rb') as h:digest=hashlib.file_digest(h,'sha256').hexdigest()
 rows.append({'path':str(f),'resolved':str(f.resolve()),'size':f.stat().st_size,'mtime':f.stat().st_mtime,'sha256':digest})
(P/'manifest.json').write_text(json.dumps({'time':time.time(),'files':rows},indent=2))
print('manifest',len(rows),'files')
