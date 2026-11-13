from pathlib import Path
from collections import Counter
import re, json, sys
job=Path(__file__).parent
mode=sys.argv[1] if len(sys.argv)>1 else 'repeat-final'
rows=[]
text=(job/(mode+'-mailbox-01.log')).read_text()
blocks=re.split(r'\[ RUN      \] ',text)[1:]
for block in blocks:
    name=block.splitlines()[0]
    passed=bool(re.search(r'\[       OK \] '+re.escape(name)+r' ',block))
    neg=[len(re.findall('negative control fault='+str(i)+' detected=1',block)) for i in (1,2,3)]
    rows.append((name,passed,neg))
lines=[]
for name in sorted({r[0] for r in rows}):
    runs=[r for r in rows if r[0]==name]
    totals=[sum(r[2][i] for r in runs) for i in range(3)]
    lines.append(f'{name}: pass={sum(r[1] for r in runs)}/{len(runs)}; lost/duplicate/wrapper detected={totals}')
for case in ('channel','actor'):
    files=sorted(job.glob(mode+'-'+case+'-*.log'))
    totals=[sum(p.read_text().count('negative control fault='+str(i)+' detected=1') for p in files) for i in (1,2,3)]
    exits=[r for r in json.loads((job/(mode+'-summary.json')).read_text()) if r['case']==case]
    lines.append(f'public-{case}: pass={sum(r["exit"]==0 for r in exits)}/{len(exits)}; lost/duplicate/wrapper detected={totals}')
    observations=Counter(line for p in files for line in p.read_text().splitlines() if 'before release:' in line or 'explicit Channel registration' in line)
    lines.extend(f'  {count}x {line}' for line,count in observations.items())
(job/('repeat-counts.log' if mode=='repeat-final' else mode+'-counts.log')).write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
