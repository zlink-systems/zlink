import csv, json, os, pathlib, subprocess, time, signal
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/dfix')
variants={'A':pathlib.Path('/root/worktree/zlink-ratio040'),'B':pathlib.Path('/root/worktree/zlink-3869')}
records=[]
limit=12*1024*1024

def available():
    return int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))

for round in range(1,5):
    for pattern in ['request-backpressure','request-serial']:
        for payload in [1024,4096]:
            for variant,root in variants.items():
                tag=f'ab-r{round}-{pattern}-{payload}-{variant}'
                output=JOB/'raw'/tag
                env=os.environ.copy()
                env.update(SKIP_BUILD='1',RUNS='1',IMPLEMENTATIONS='zlink-framework-dotnet',PATTERNS=pattern,PAYLOAD_SIZES=str(payload),WARMUP_SECONDS='30',DURATION_SECONDS='5',RUN_STAMP=tag,OUTPUT_DIR=str(output))
                mem=available()
                if mem < limit: raise RuntimeError(f'MemAvailable below 12 GiB: {mem} KiB')
                started=time.time()
                cmd=['flock','-o','/root/jobs/fx/cx/measure.lock','env']+[f'{k}={env[k]}' for k in ['SKIP_BUILD','RUNS','IMPLEMENTATIONS','PATTERNS','PAYLOAD_SIZES','WARMUP_SECONDS','DURATION_SECONDS','RUN_STAMP','OUTPUT_DIR']]+['bash',str(root/'framework/bench/grpc/dotnet/run_local.sh')]
                print(f'START {tag} MemAvailable={mem/1024/1024:.2f} GiB',flush=True)
                with (JOB/'raw'/f'{tag}.log').open('w') as log:
                    process=subprocess.Popen(cmd,cwd=root,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
                    while process.poll() is None:
                        current=available();mem=min(mem,current)
                        if current < limit:
                            os.killpg(process.pid,signal.SIGTERM)
                            raise RuntimeError(f'MemAvailable below 12 GiB during {tag}')
                        time.sleep(1)
                if process.returncode: raise RuntimeError(f'{tag} failed: {process.returncode}')
                result=output/'run1'/f'zlink-framework-dotnet-{pattern}-{payload}'/'results.json'
                cell=json.loads(result.read_text())['cells'][0]
                records.append(dict(round=round,pattern=pattern,payload=payload,variant=variant,mps=cell['throughput_per_second'],completed=cell['completed'],mem_min_gib=mem/1024/1024,elapsed_s=time.time()-started,result=str(result)))
                (JOB/'raw'/'ab-results.json').write_text(json.dumps(records,indent=2)+'\n')
                print(f"DONE {tag}: {cell['throughput_per_second']:.2f} msg/s MemAvailable_min={mem/1024/1024:.2f} GiB",flush=True)
