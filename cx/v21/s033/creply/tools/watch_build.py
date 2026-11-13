from pathlib import Path
import os,signal,time,json
p=1551307
while p>1:
 q=Path('/proc')/str(p);a=(q/'cmdline').read_bytes().split(b'\0')
 if b'/root/jobs/fx/cx/cpp-build-slot.sh' in a:break
 p=int((q/'stat').read_text().rsplit(')',1)[1].split()[1])
assert p>1
pg=os.getpgid(p);out=Path('/root/jobs/fx/cx/v21/s033/creply/build-memory.jsonl')
with out.open('a') as f:
 while Path(f'/proc/{p}').exists():
  kb=int(next(l.split()[1] for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))
  f.write(json.dumps({'time':time.time(),'available_kb':kb,'wrapper_pid':p})+'\n');f.flush()
  if kb<12582912:
   os.killpg(pg,signal.SIGTERM);raise RuntimeError('build stopped below 12 GiB MemAvailable')
  time.sleep(2)
