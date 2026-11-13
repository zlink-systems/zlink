import json,os,pathlib,signal,sys,time
pgid=int(sys.argv[1]); minimum=10**20
out=pathlib.Path('/root/jobs/fx/cx/v21/s033/cproto/build-memory.jsonl')
while pathlib.Path(f'/proc/{pgid}').exists():
    available=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
    if available<minimum:
        minimum=available
        with out.open('a') as h: h.write(json.dumps({'time':time.time(),'pgid':pgid,'MemAvailable_kib':minimum})+'\n')
    if available<13*1024*1024:
        os.killpg(pgid,signal.SIGTERM)
        print(f'SIGTERM build group {pgid}: MemAvailable={available/1024/1024:.3f} GiB',flush=True)
        break
    time.sleep(.5)
print(f'minimum sampled MemAvailable={minimum/1024/1024:.3f} GiB',flush=True)
