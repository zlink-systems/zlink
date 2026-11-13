from pathlib import Path
import re,collections,json,sys
j=Path(__file__).parent
for v in ('main','branch'):
 p=j/'perf/send-saturation'/v;d=p/'run1/zlink-framework-cpp-send-saturation-1024'
 roles=set(re.findall(r'ROLE app-worker tid=(\d+)',(d/'target.log').read_text()))
 counts=collections.Counter();chains=collections.defaultdict(collections.Counter);n=0
 for block in (p/'perf.script').read_text().split('\n\n'):
  lines=block.splitlines()
  if not lines:continue
  m=re.search(r'(\d+)/(\d+)\s+([\d.]+):\s+(\d+) cpu-clock:',lines[0])
  if not m or m[2] not in roles:continue
  frames=[re.sub(r'^\s*[0-9a-f]+ ','',s).rsplit(' (',1)[0] for s in lines[1:] if s.strip()]
  if not frames:continue
  n+=1;counts[frames[0]]+=1;chains[frames[0]][' -> '.join(frames[1:5])]+=1
 print('\n',v,'samples',n)
 for f,c in counts.most_common(30):
  print(f'{c/n*100:.1f}% ({c}): {f[:230]}')
  for s,x in chains[f].most_common(2):print('   ',x,s[:520])
