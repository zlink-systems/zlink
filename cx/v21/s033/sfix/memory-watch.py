import pathlib,time,datetime
p=pathlib.Path('/root/jobs/fx/cx/v21/s033/sfix')
with (p/'memory.tsv').open('a',buffering=1) as f:
 while not (p/'memory-stop').exists():
  kb=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
  f.write(f'{datetime.datetime.now().isoformat()}\t{kb}\n')
  time.sleep(1)
