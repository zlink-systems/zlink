import pathlib, subprocess, os, json, hashlib, datetime, sys
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/c4dec'); wt=pathlib.Path('/root/worktree/zlink-v21-s033/c4dec')

def command(args): return subprocess.check_output(args,cwd=wt,text=True).strip()
def record(binary,label,sources):
    source_times={str(p):p.stat().st_mtime_ns for p in sources}
    if not binary.is_file() or binary.stat().st_mtime_ns < max(source_times.values()): raise RuntimeError('stale binary '+str(binary))
    metadata={'time':datetime.datetime.now(datetime.timezone.utc).isoformat(),'head':command(['git','rev-parse','HEAD']),'dirty':bool(command(['git','status','--porcelain'])),'base':command(['git','merge-base','HEAD','origin/main']),'binary':str(binary),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'source_mtimes':source_times,'binary_mtime':binary.stat().st_mtime_ns,'cpus':sorted(os.sched_getaffinity(0)),'scheduling':os.sched_getscheduler(0),'scope':'decode only, no throughput verdict, no measure.lock'}
    (job/'raw'/f'{label}.metadata.json').write_text(json.dumps(metadata,indent=2))
    with (job/'progress.md').open('a') as f:f.write(f"{metadata['time']} {label}: HEAD={metadata['head']} dirty={metadata['dirty']}, base={metadata['base']}, binary={binary} SHA256={metadata['sha256']}; call-shape.md의 body-only 대조 적용; freshness 통과; CPUs={metadata['cpus']}; policy={metadata['scheduling']}\n")

def run(candidate,kind):
    stem=f'{candidate}-{kind}';binary=wt/'build-c4dec'/f'c4dec-{kind}-{candidate}'
    profile=job/'raw'/candidate/'include/zlink/json_profile.hpp'
    sources=[profile,job/'tools'/f'{"bulk" if kind=="bulk" else "registry"}-probe.cpp']
    if candidate=='candidate-a':sources+=[job/'raw/candidate-a/include/nlohmann/detail/input/lexer.hpp']
    record(binary,stem,sources)
    env=dict(os.environ)
    if kind=='registry':env['LD_PRELOAD']=str(wt/'build-c4dec/libc4dec-malloc-counter.so')
    with (job/'raw'/f'{stem}-allocations.txt').open('w') as out:
        subprocess.run([str(binary),'counts'],cwd=wt,env=env,stdout=out,stderr=subprocess.STDOUT,check=True,timeout=20)
    if kind!='bulk':return
    for size in [1024,4096]:
        label=f'{candidate}-decode-{size}';record(binary,label,sources)
        with (job/'raw'/f'{label}.log').open('w') as out:
            subprocess.run(['perf','record','-q','-e','cpu-clock:u','-F','499','--call-graph','fp','-o',str(job/'raw'/f'{label}.perf.data'),'--',str(binary),'decode',str(size)],stdout=out,stderr=subprocess.STDOUT,env=env,check=True,timeout=20)
        for mode,args in [('self',['--no-children']),('callgraph',['--children'])]:
            with (job/'raw'/f'{label}.{mode}.txt').open('w') as out:subprocess.run(['perf','report','--stdio','-i',str(job/'raw'/f'{label}.perf.data'),'--percent-limit','0.5','--sort','symbol']+args,stdout=out,check=True)
for candidate in ['baseline','candidate-a','candidate-b']:run(candidate,sys.argv[1] if len(sys.argv)>1 else 'bulk')
