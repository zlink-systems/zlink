import json,sys
from pathlib import Path
from collections import Counter,defaultdict
import analyze
r=json.load(open(analyze.ROOT/'analysis.json'))
for cell in ('zlink-framework-cpp-request-backpressure-1024',):
 print('\nCELL',cell)
 for role in ('source','target'):
  rr=r[cell]['roles'][role];tid=rr['critical_tid'];t=rr['threads'][tid]
  print('ROLE',role,'critical',tid,'US',t['cpu_us'],'F TOP20')
  fs=[f for f in t['functions'] if 'zlink::framework::' in analyze.owner_name(f['symbol']) and '::bench::' not in analyze.owner_name(f['symbol'])]
  for f in fs[:25]:print(f"{f['self_us']:.4f}\t{f['inclusive_us']:.4f}\t{f['symbol'][:420]}")
  print('THREAD ROLES')
  for tid,t in sorted(rr['threads'].items(),key=lambda it:-it[1]['cpu_us']):
   if t['cores']<.003:continue
   # Find representative non-wrapper functions by inclusive weight.
   funcs=[f for f in t['functions'] if any(x in f['symbol'] for x in ['worker_loop','node_receive_role_t','drain_application_owner','request_loop','command_loop','io_context::run','run_tasks','run_loop','dispatch_ready','completion_dispatcher','coroutine_executor','poller_t::wait'])]
   funcs.sort(key=lambda f:-f['inclusive_us'])
   print(tid,round(t['cpu_us'],3),round(t['cores'],3),[f['symbol'][:160] for f in funcs[:3]])
  if role!='source':continue
  tid=rr['critical_tid'];t=rr['threads'][tid];ss=json.load(open(analyze.ROOT/'out-profile/run1'/cell/'capture/source.samples.json'));ss=[x for x in ss if x['tid']==tid];factor=t['cpu_us']/sum(x['period'] for x in ss)
  group=defaultdict(Counter)
  for sm in ss:
   cat=analyze.classify(sm['frames']);fr=sm['frames'];chosen=None
   if cat=='allocation':
    for s,d in fr:
     if analyze.ALLOC.search(s):continue
     if 'zlink::framework::' in s or '/libzlink.so' in d or 'framework_driver_t::' in s or 'raw_driver_t::' in s or 'binding_driver_t::' in s:chosen=s;break
   elif cat=='handoff/wake':
    for s,d in fr:
     if ('/libc.so' in d or d=='inlined' or 'kernel.kallsyms' in d or '/libstdc++' in d):continue
     chosen=s;break
   else:chosen=fr[0][0] if fr else '<empty>'
   group[cat][chosen or '<unresolved>']+=sm['period']*factor
  for cat,c in group.items():
   print('STAGE OWNERS',cat)
   for s,v in c.most_common(25): print(f'{v:.4f}\t{s[:470]}')
