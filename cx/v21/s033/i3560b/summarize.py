import json,re,pathlib,collections
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560b')
rows=[]
for raw in sorted(J.glob('raw-*')):
 for cell in sorted(raw.glob('zlink-*-run-*')):
  if not (cell/'stats-after.json').exists():continue
  ids=dict(x.split('=') for x in (cell/'pids.txt').read_text().splitlines())
  before=json.loads((cell/'threads-before.json').read_text()); after=json.loads((cell/'threads-after.json').read_text())
  st0=json.loads((cell/'stats-before.json').read_text());st1=json.loads((cell/'stats-after.json').read_text())
  delta=st1.get('completed',0)-st0.get('completed',0)
  maps=collections.defaultdict(list)
  for line in (cell/'wake-counts.txt').read_text().splitlines() if (cell/'wake-counts.txt').exists() else []:
   m=re.match(r'^@(\w+)\[(.*)\]: (\d+)$',line)
   if m:maps[m[1]].append((m[2].split(', '),int(m[3])))
  req=sum(n for _,n in maps['request_ok'])
  row={'raw':str(cell),'completed_stats_delta':delta,'request_ok_bpf':req,'roles':{}}
  for role,pid in ids.items():
   ts={t:after[role].get(t,before[role].get(t)) for t in set(before[role])|set(after[role])}
   io=collections.defaultdict(float)
   tids=[]
   for tid,v in ts.items():
    if '/IO/' in v['comm']:
     us=max(0,after[role].get(tid,{}).get('cpu_ns',0)-before[role].get(tid,{}).get('cpu_ns',0))/1000
     io[v['comm']]+=us/delta if delta else 0
     tids.append({'tid':tid,'comm':v['comm'],'cpu_us':us,'present_before':tid in before[role],'present_after':tid in after[role]})
   r={'pid':int(pid),'io_us_per_request':dict(io),'io_tids_union':tids,'maps':{}}
   for name,items in maps.items():
    selected=[]
    for key,n in items:
     if key[0]==pid:
      tid=key[1] if len(key)>1 else ''
      selected.append({'key':key,'thread':ts.get(tid,{}).get('comm','unknown'),'count':n,'per_request':n/req if req else None})
    if selected:r['maps'][name]=selected
   row['roles'][role]=r
  rows.append(row)
(J/'summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for r in rows:
 print(pathlib.Path(r['raw']).parent.name,pathlib.Path(r['raw']).name,'requests_bpf',r['request_ok_bpf'],'stats_delta',r['completed_stats_delta'])
 for role,a in r['roles'].items():
  print(role,'io_us/request',a['io_us_per_request'])
  for name in ['send','reads','writes','notify_socket','requests','wait_enter','comp_result','router_result','reply']:
   items=a['maps'].get(name,[])
   if items: print(name,'total',sum(x['count'] for x in items),'per_request',sum(x['count'] for x in items)/r['request_ok_bpf'] if r['request_ok_bpf'] else None)
