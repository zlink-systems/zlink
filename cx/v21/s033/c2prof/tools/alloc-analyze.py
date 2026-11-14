from pathlib import Path
from collections import Counter,defaultdict
import json,struct,re,subprocess,bisect
J=Path('/root/jobs/fx/cx/v21/s033/c2prof');R=J/'raw';cache={}

def symbols(path):
 if path in cache:return cache[path]
 p=Path(path)
 if not p.exists():return ([],[])
 cmd=['nm','-n','-C','--defined-only',path]
 if 'libc.so' in path or 'libstdc++' in path or 'ld-linux' in path:cmd.insert(1,'-D')
 result=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
 vals=[]
 for l in result.stdout.splitlines():
  m=re.match(r'^([0-9a-f]+)\s+[tTwW]\s+(.*)$',l)
  if m:vals.append((int(m[1],16),m[2]))
 vals.sort();cache[path]=([a for a,b in vals],[b for a,b in vals]);return cache[path]

def resolve_map(path):
 maps=[]
 for l in path.read_text().splitlines():
  t=l.split(None,5)
  if len(t)<6 or not t[5].startswith('/'):continue
  lo,hi=[int(s,16) for s in t[0].split('-')];maps.append((lo,hi,int(t[2],16),t[5]))
 def resolve(ip):
  for lo,hi,off,p in maps:
   if lo<=ip<hi:
    addr=ip-lo+off;addrs,names=symbols(p);i=bisect.bisect_right(addrs,addr)-1
    return {'symbol':names[i] if i>=0 else '[unknown]','offset':addr-addrs[i] if i>=0 else addr,'dso':p,'elf_address':hex(addr)}
  return {'symbol':'[unmapped]','elf_address':hex(ip)}
 return resolve
out={}
for d in sorted(R.glob('alloc-*')):
 if not d.is_dir() or not(d/'validated.json').exists():continue
 counts=json.loads((d/'validated.json').read_text());before=json.loads((d/'before.json').read_text());after=json.loads((d/'after.json').read_text());roles={}
 for side in ['source','target']:
  messages=counts['window_source_completed'];buf=(d/(side+'.counter')).read_bytes();enabled,n=struct.unpack_from('<II',buf);threads={};total=Counter()
  for i in range(n):
   tid,ma,ca,re_,fr,by,_,_=struct.unpack_from('<8Q',buf,64+i*64)
   name=after['processes'][side]['threads'].get(str(tid),before['processes'][side]['threads'].get(str(tid),{})).get('comm','transient HTTP')
   t=dict(tid=tid,comm=name,malloc=ma,calloc=ca,realloc=re_,free=fr,bytes=by);threads[str(tid)]=t
   total.update({k:t[k] for k in ['malloc','calloc','realloc','free','bytes']})
  rr={'messages':messages,'totals':dict(total),'per_message':{k:v/messages for k,v in total.items()},'threads':threads};roles[side]=rr
  if d.name.startswith('alloc-stacks'):
   resolve=resolve_map(d/(side+'.maps'));stackbuf=(d/(side+'.alloc-stacks.bin')).read_bytes();chains=Counter();first=Counter();bycall=defaultdict(Counter);details={}
   for record in struct.iter_unpack('<36Q',stackbuf):
    tid,kind,size,depth,*ips=record;frames=[resolve(ip-1) for ip in ips[:depth]]
    frames=[f for f in frames if 'libwindow-counter' not in f.get('dso','')]
    syms=[f['symbol'] for f in frames]
    chain=';'.join(syms);chains[chain]+=1
    own=next((f for f in frames if '/build-cfx/' in f.get('dso','') or '/libzlink.so' in f.get('dso','')),frames[0] if frames else {})
    key=own.get('symbol','unknown')+'+'+hex(own.get('offset',0));first[key]+=1;bycall[key][str(size)]+=1;details[key]=own
   rr['stack_samples']=sum(first.values());rr['callsites']=[{'callsite':s,'samples':v,'estimated_allocations_per_message':v*256/messages,'sizes':dict(bycall[s]),'location':details[s]} for s,v in first.most_common()]
   (d/(side+'.alloc-collapsed.txt')).write_text('\n'.join(f'{s} {v}' for s,v in chains.most_common()))
 out[d.name]={'path':str(d),'roles':roles,'seconds':counts['seconds']}
(R/'allocations-summary.json').write_text(json.dumps(out,indent=2))
for name,r in out.items():
 print(name)
 for side,rr in r['roles'].items():
  print(side,{k:round(v,3) for k,v in rr['per_message'].items()})
  for t in rr.get('callsites',[])[:12]:print(' ',round(t['estimated_allocations_per_message'],3),t['callsite'][:220],t['sizes'])
