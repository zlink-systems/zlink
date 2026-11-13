import json,pathlib,shutil,sys,time
root=pathlib.Path(sys.argv[1])
mem=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))*1024
usage=sum(p.stat().st_size for p in root.rglob('*') if p.is_file())
r=dict(stage='measure',captured_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),MemAvailable_bytes=mem,job_bytes=usage,job_cap_bytes=25*1024**3,root_free_bytes=shutil.disk_usage('/').free,allowed=mem>=12*1024**3 and usage<25*1024**3)
with open(root/'measure-preflight.jsonl','a') as f:f.write(json.dumps(r)+'\n')
print(json.dumps(r),flush=True)
if not r['allowed']:raise SystemExit(3)
