from pathlib import Path
from collections import Counter,defaultdict
import json,analyze
J=analyze.ROOT;data=json.load(open(J/'analysis.json'));out={}
for name,r in data.items():
 res={}
 for role,rr in r['roles'].items():
  samples=json.load(open(J/'out-profile/run1'/name/'capture'/f'{role}.samples.json'));bytid=defaultdict(list)
  for s in samples:bytid[s['tid']].append(s)
  totals=Counter();details=defaultdict(Counter);crit=Counter()
  for tid,t in rr['threads'].items():
   ss=bytid.get(tid,[]);wt=sum(x['period'] for x in ss);factor=t['cpu_us']/wt if wt else 0
   for x in ss:
    fr=x['frames'];cat=analyze.classify(fr);weight=x['period']*factor;syms=[s for s,d in fr];leaf=syms[0] if syms else ''
    features=[]
    if cat=='allocation' and any('zlink::msg_t::init_size' in s for s in syms):features.append('native message buffer allocation')
    if 'strftime' in leaf:features.append('strftime self')
    if any('epoll_ctl' in s for s in syms):features.append('epoll_ctl inclusive')
    if any('client_call_codec_t::create_envelope' in s for s in syms):features.append('create_envelope inclusive')
    if cat=='allocation' and any('protobuf_serializer_t' in s or 'protobuf_codec' in s for s in syms):features.append('protobuf allocation')
    if any('task_wait_registration_t::prepare_context' in s or 'capture_native_scheduler' in s or 'capture_runtime_native_continuation_scheduler' in s for s in syms):features.append('continuation context inclusive')
    if cat=='allocation' and any(analyze.SYNC.search(s) for s in syms[:12]):features.append('allocator synchronization')
    if not 'framework' in name and cat=='Framework bookkeeping':details['binding classified bookkeeping'][leaf]+=weight
    if tid==rr['critical_tid'] and cat=='allocation':
     nonalloc=next((s for s,d in fr if not analyze.ALLOC.search(s) and ('zlink::' in s or '/libzlink.so' in d or 'framework_driver_t' in s or 'binding_driver_t' in s)),leaf)
     details['critical allocation owners'][nonalloc]+=weight
    for feature in features:
     totals[feature]+=weight
     if tid==rr['critical_tid']:crit[feature]+=weight
  res[role]={'whole':dict(totals),'critical':dict(crit),'details':{k:dict(v) for k,v in details.items()}}
 out[name]=res
(J/'extras.json').write_text(json.dumps(out,indent=2))
for name,res in out.items():
 print(name)
 for role,r in res.items():
  print(role,'whole',r['whole'],'critical',r['critical'])
  for k,v in r['details'].items():
   print(k)
   for s,t in sorted(v.items(),key=lambda x:-x[1])[:10]:print(f'{t:.4f}',s[:300])
