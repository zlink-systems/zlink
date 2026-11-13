import pathlib as P,os,sys,shutil,hashlib,json
j=P.Path('/root/jobs/fx/cx/v21/s033/csites'); stage=sys.argv[1]; mode=sys.argv[2]; impl=sys.argv[3] if len(sys.argv)>3 else 'framework'; pattern=sys.argv[4] if len(sys.argv)>4 else 'request-backpressure';round=sys.argv[5] if len(sys.argv)>5 else '1'
d=j/(stage+'-'+mode+'-'+impl+'-'+pattern+'-'+round);d.mkdir(exist_ok=True)
b=j/(stage if mode=='counts' else stage+'-perf')
original=P.Path('/root/jobs/fx/cx/v21/s033/csite2')/('run.py' if impl=='framework' else 'run_binding.py')
s=original.read_text().replace("j=P.Path('/root/jobs/fx/cx/v21/s033/csite2');base=P.Path('/root/jobs/fx/cx/v21/s033/crecv3/build')",f'j=P.Path({str(d)!r});base=P.Path({str(b)!r})')
s=s.replace("j/'bench_cpp_client'","base/'bench_cpp_client'").replace('request-backpressure',pattern)
if mode=='counts':
 s=s.replace("LD_PRELOAD=str(j/'libchand.so')",f"LD_PRELOAD={str(j/'redirect.so')!r}+':'+{str(b/'libchand.so')!r}")
 os.environ['CSITES_OUTPUT']=str(d)
else:
 s=s.replace("mem(); env=dict(os.environ,LD_PRELOAD=str(j/'libchand.so'),TMPDIR=str(j));", "mem(); env=dict(os.environ,TMPDIR=str(j));env.pop('LD_PRELOAD',None);")
 # Counts are irrelevant to the throughput run; preserve launch/timing/workload logic.
 a=s.index('def snapshot(ch):');z=s.index('\ntry:',a);s=s[:a]+"def snapshot(ch):return {}\n"+s[z:]
(d/'harness-sha256.json').write_text(json.dumps({'original':str(original),'sha256':hashlib.sha256(original.read_bytes()).hexdigest(),'path_substitutions_only':mode=='counts','mode':mode}))
exec(compile(s,str(original),'exec'))
if stage=='after-final' and mode=='counts':
 import subprocess as S
 post_count_job=P.Path('/root/jobs/fx/cx/v21/s033/csites')
 with (post_count_job/'after-final-analysis.log').open('w') as f:S.run([sys.executable,str(post_count_job/'analyze.py'),stage],stdout=f,stderr=f,check=True)
 analysis=json.loads((post_count_job/'after-final-analysis.json').read_text())
 assert analysis['sites']['target']['1']['locks']==0,'actual public target still uses serializer cache'
 assert all(x['heap']==0 for x in analysis['arena_upstream'].values())
 print('PASS actual target serializer cache lock=0; PMR upstream=0 before throughput',flush=True)
