from pathlib import Path
import re,json,subprocess
r=Path('/root/jobs/fx/cx/v21/s033/c4dec');wt=Path('/root/worktree/zlink-v21-s033/c4dec');summary=[]
for candidate in ['baseline','candidate-a','candidate-b','production']:
 text=(r/'raw'/f'{candidate}-registry-allocations.txt').read_text()
 rows=[]
 for line in text.splitlines():
  m=re.fullmatch(r'size=(\d+) (encode|decode)_(new|malloc)=(\d+) bytes=(\d+) largest=(\d+)',line)
  if m: rows.append(dict(size=int(m[1]),direction=m[2],counter=m[3],count=int(m[4]),bytes=int(m[5]),largest=int(m[6])))
 for size in [1024,4096]:
  cells=[x for x in rows if x['size']==size and x['direction']=='decode'];assert len(cells)==2
  assert len({(x['count'],x['bytes']) for x in cells})==1,(candidate,size,cells)
  if candidate in ['candidate-b','production']:assert cells[0]['count']==2 and cells[0]['bytes']==size+480
  summary.append(dict(candidate=candidate,size=size,counters=cells))
 # Resolve the recorded allocation stacks for the two requested message sizes.
 sections=[]
 pattern=r'size=(1024|4096) decode_new=\d+ bytes=\d+ largest=\d+\n(.*?)(?=\nsize=|\Z)'
 for match in re.finditer(pattern,text,re.S):
  addresses=sorted(set(re.findall(r'^  (0x[0-9a-f]+) ',match[2],re.M)))
  binary=wt/'build-c4dec'/f'c4dec-registry-{candidate}'
  resolved=subprocess.check_output(['addr2line','-f','-C','-i','-e',str(binary),'-a',*addresses],text=True)
  sections.append('size='+match[1]+'\n'+resolved)
 (r/'raw'/f'{candidate}-registry-stacks.txt').write_text('\n'.join(sections))
(r/'raw/registry-summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
