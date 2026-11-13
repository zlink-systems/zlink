import json,pathlib,collections,re
p=pathlib.Path('/root/jobs/fx/cx/v21/s033/jsend')
def maps(f):
 out={}
 for l in f.read_text().splitlines():
  try:d=json.loads(l)
  except ValueError:continue
  if d.get('type')=='map':out.update(d['data'])
 return out
out={}
for run in (p/'runs').iterdir():
 cap=run/'capture'
 if not (cap/'handoff/window.json').exists():continue
 ini=json.load(open(cap/'initial.json'));names={t:{'role':r,'comm':d['comm']} for r,x in ini['processes'].items() for t,d in x['threads'].items()};rr={}
 for mode in ('handoff','completion'):
  if not (cap/mode/'window.json').exists():continue
  w=json.load(open(cap/mode/'window.json'));b=w['before'];a=w['after'];dt=(a['start']+a['end']-b['start']-b['end'])/2;rate=(a['stats']['source']['completed']-b['stats']['source']['completed'])/dt;txt=(cap/(mode+'.log')).read_text();sec=(int(re.search(r'STOP (\d+)',txt)[1])-int(re.search(r'START (\d+)',txt)[1]))/1e9;n=rate*sec;m=maps(cap/(mode+'.log'))
  r={'rate':rate,'seconds':sec,'messages_estimate':n,'maps':m}
  if mode=='handoff':
   r['native_reply_count']=sum(m.get('@reply',{}).values());r['native_reply_per_request']=r['native_reply_count']/n;r['io']={}
   for k in ('@wakeup','@waking'):
    c=collections.Counter();cp=collections.defaultdict(collections.Counter)
    for key,v in m.get(k,{}).items():
     t,pid,tid,cpu,dest=key.split(',');c[t]+=v;cp[t]['local' if cpu==dest else 'remote']+=v
    for t,v in c.items():r['io'].setdefault(t,{'name':names.get(t)}).update({k.lstrip('@')+'_per_request':v/n,k.lstrip('@')+'_cpu':dict(cp[t])})
  rr[mode]=r
 out[run.name]=rr
(p/'handoff-summary.json').write_text(json.dumps(out,indent=2))
for name,r in out.items():
 print(name,'reply/request',r['handoff']['native_reply_per_request'],'producer',r['handoff']['maps'].get('@reply'))
 print('io',r['handoff']['io'])
 print('completion',r.get('completion',{}).get('maps'))
