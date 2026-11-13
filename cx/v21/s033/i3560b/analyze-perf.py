import pathlib,re,json,collections
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560b')
header=re.compile(r'^(.+?)\s+(\d+)/(\d+)\s+([\d.]+):\s+(\d+)\s+(.+):\s*$')
frame=re.compile(r'^\s+[0-9a-f]+\s+(.+?)\s+\(([^()]*)\)\s*$')
results=[]
for p in sorted(J.glob('raw-*/zlink-*-run-*/perf-stacks.txt')):
 if p.stat().st_size==0:continue
 cell=p.parent;before=json.loads((cell/'threads-before.json').read_text());after=json.loads((cell/'threads-after.json').read_text())
 tids={int(t):(role,v['comm']) for role,a in before.items() for t,v in a.items()}
 tids.update({int(t):(role,v['comm']) for role,a in after.items() for t,v in a.items()})
 sums=collections.defaultdict(lambda:{'samples':0,'period':0,'leaf':collections.Counter(),'core':collections.Counter(),'paths':collections.Counter(),'example':{}})
 state=None;fs=[]
 def add():
  if state is None:return
  pid,tid,period=state;a=sums[tid];a['samples']+=1;a['period']+=period
  if fs:a['leaf'][fs[0]]+=1
  for f in set(x for x in fs if x.startswith('zlink::')):a['core'][f]+=1
  path='other'
  if any('socket_base_t::process_commands(' in x for x in fs):
   if any('zlink_completion_recv' in x for x in fs):path='completion_recv -> process_submit_commands -> batch notification'
   elif any('socket_base_t::get_events(' in x for x in fs):path='public poller get_events recheck -> batch notification'
   elif any('zlink_request' in x for x in fs):path='request submit -> batch notification'
   elif any('zlink_reply' in x for x in fs):path='reply submit -> batch notification'
   elif any('zlink_router_recv' in x for x in fs):path='router receive -> batch notification'
   else:path='other batch notification'
  elif any('socket_base_t::notify_request_completion' in x for x in fs):path='notify_request_completion -> mailbox.send'
  elif any('mailbox_t::send(' in x for x in fs):path='pipe command -> mailbox.send'
  elif any('timer_signal' in x for x in fs):path='timer'
  a['paths'][path]+=1
  if path not in a['example']:a['example'][path]=fs[:]
 with p.open() as f:
  for line in f:
   m=header.match(line.strip())
   if m:
    add();state=(int(m[2]),int(m[3]),int(m[5]));fs=[]
   elif line.strip():
    n=frame.match(line)
    if n:fs.append(n[1])
  add()
 out={'raw':str(cell),'threads':{}}
 for tid,a in sums.items():
  role,comm=tids.get(tid,('unknown','unknown'))
  out['threads'][str(tid)]={'role':role,'comm':comm,'samples':a['samples'],'period':a['period'],'paths':dict(a['paths']),'top_leaf':a['leaf'].most_common(12),'top_core':a['core'].most_common(16),'examples':a['example']}
 results.append(out)
(J/'perf-summary.json').write_text(json.dumps(results,indent=2)+'\n')
for r in results:
 print(pathlib.Path(r['raw']).parent.name,pathlib.Path(r['raw']).name)
 for tid,a in r['threads'].items():
  if '/IO/' in a['comm'] or 'wakeperf' in r['raw']:
   if a['samples']<20:continue
   print(tid,a['role'],a['comm'],'samples',a['samples'])
   if 'wakeperf' in r['raw']:print(a['paths'])
   else:
    for f,n in a['top_core'][:5]:print(round(n/a['samples']*100,1),f[:135])
