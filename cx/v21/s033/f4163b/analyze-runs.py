from pathlib import Path
import re,csv,statistics as st
j=Path('/root/jobs/fx/cx/v21/s033/f4163b')
rows=[]
for kind in ['C','R','L']:
 for round in [1,2]:
  p=j/'runs'/f'{kind}-r{round}-single-ROUTER_ROUTER-inproc.log'
  if not p.exists():continue
  run=None
  for line in p.read_text().splitlines():
   m=re.search(r'run (\d+)/5:',line)
   if m:run=int(m.group(1))+(round-1)*5
   if 'median:' in line:run=None
   if run and re.match(r'\s*\| (256|1024)B\s*\|',line):
    c=[x.strip() for x in line.split('|')[1:-1]]
    rows.append(dict(kind=kind,run=run,size=int(c[0][:-1]),rate_m=float(c[1].split()[0])/1000,mean_ms=float(c[3].split()[0]),p95_ms=float(c[4].split()[0]),p99_ms=float(c[5].split()[0])))
if rows:
 with (j/'runs-10.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
for size in [256,1024]:
 for kind in ['C','R','L']:
  a=[r for r in rows if r['kind']==kind and r['size']==size]
  if not a:continue
  means=[r['mean_ms'] for r in a];p95=[r['p95_ms'] for r in a];rates=[r['rate_m'] for r in a]
  print(size,kind,'n',len(a),'median',st.median(means),'range',min(means),max(means),'p95',st.median(p95),'rate',st.median(rates),'high',sum(x>=.9 for x in means),'means',means)
