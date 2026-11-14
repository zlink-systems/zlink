from pathlib import Path
from collections import defaultdict,Counter
import os,json,re,subprocess,gzip,csv
J=Path('/root/jobs/fx/cx/v21/s033/c2prof'); R=J/'raw'; os.environ['DEBUGINFOD_URLS']=''
HEAD=re.compile(r'^(.+?)\s+(\d+)/(\d+)\s+([0-9.]+):\s+(\d+)\s+cpu-clock:')
FRAME=re.compile(r'^\s+([0-9a-f]+) (.*) \(([^()]*)\)$')

def parse(path):
 cache=path.with_suffix('.aggregates-v2.json')
 if cache.exists():return json.loads(cache.read_text())
 d=path.parent;b=json.loads((d/'before.json').read_text());a=json.loads((d/'after.json').read_text());groups={};cur=None;total=0;kept=0
 err=open(str(path)+'.script.log','w')
 p=subprocess.Popen(['perf','--buildid-dir',str(J/'tools/perf-buildid'),'script','--no-inline','-i',str(path),'-F','comm,pid,tid,time,event,period,ip,sym,dso'],stdout=subprocess.PIPE,stderr=err,text=True)
 def add(x):
  nonlocal total,kept
  if not x:return
  total+=1
  if not b['monotonic']<=x['time']<=a['monotonic']:return
  kept+=1;tid=x['tid'];g=groups.setdefault(tid,{'comm':x['comm'],'samples':0,'weight':0,'self':Counter(),'inclusive':Counter(),'stages':Counter(),'dsos':{},'leaf_dso':Counter()});w=x['period'];g['weight']+=w;g['samples']+=1
  fs=x['frames'];syms=[y[0] for y in fs]
  if fs:
   g['self'][fs[0][0]]+=w;g['leaf_dso'][fs[0][1]]+=w
   for s in set(syms):g['inclusive'][s]+=w
   for s,ds in fs:g['dsos'][s]=ds
  g['stages'][stage(syms)]+=w
 for line in p.stdout:
  m=HEAD.match(line)
  if m:
   add(cur);cur={'comm':m[1],'pid':m[2],'tid':m[3],'time':float(m[4]),'period':int(m[5]),'frames':[]}
  elif not line.strip():add(cur);cur=None
  elif cur:
   m=FRAME.match(line)
   if m:cur['frames'].append((m[2],m[3]))
 add(cur);assert p.wait()==0;err.close()
 out={'groups':groups,'total_samples':total,'kept_samples':kept};cache.write_text(json.dumps(out));return out

def stage(syms):
 # Disjoint attribution: narrow callee before broad caller; allocator stays with header/bench owner.
 text='\n'.join(syms)
 if re.search(r'decode_header_bytes|header_sax_t|canonical_header_reader|envelope_codec_t::decode_header',text):return 'header decode (inclusive children)'
 if re.search(r'encode_header_bytes|write_header\(|header_writer_t|header_plan_t|envelope_codec_t::encode_header',text):return 'header encode (inclusive children)'
 if re.search(r'client_call_codec_t::create_envelope|next_correlation_id|deadline_bytes',text):return 'request header creation'
 if re.search(r'client_call_codec_t::encode_payload_parts|message_parts_t::(?:take_items|message_parts_t)|encoded_payload_from_raw',text):return 'envelope adapters/parts'
 if re.search(r'channel_reply_writer_t',text):return 'reply header/parts'
 if re.search(r'futex|pthread_mutex|pthread_cond|__lll_|try_to_wake_up|finish_task_switch|__schedule|schedule\b|ttwu_',text):return 'sync/wake (outside header)'
 if re.search(r'(^|\n)(?:__libc_)?(?:malloc|calloc|realloc|free)|_int_malloc|_int_free|operator new|operator delete',text):return 'other allocation/free'
 for s in syms:
  if re.search(r'google::protobuf|BenchPayload::|encode_bench_payload',s):return 'protobuf (bench L1/L2 common)'
  if re.search(r'^zlink::(?!framework)|^zlink_|^s_send|^s_recv',s):return 'binding/Core'
  if re.search(r'ladder|driver_t|stream_t|latency_sampler|server_metrics',s):return 'bench driver/handler'
  if 'zlink::framework::' in s:return 'Framework other'
 return 'other/kernel'

def stat(path):
 result={}
 for l in path.read_text().splitlines():
  t=l.split(';')
  if len(t)<4 or l.startswith('#'):continue
  m=re.match(r'(.+)-(\d+)$',t[0])
  if not m:continue
  g=result.setdefault(m[2],{'comm':m[1],'us':0,'switches':0,'migrations':0})
  try:v=float(t[1])
  except ValueError:continue
  if t[3]=='task-clock':g['us']=v*1000
  elif t[3]=='context-switches':g['switches']=v
  elif t[3]=='cpu-migrations':g['migrations']=v
 return result

def analyze():
 runs={}
 for d in sorted(R.glob('perf-final-*')):
  if not d.is_dir() or d.name.endswith('-r0') or not (d/'validated.json').exists():continue
  meta=json.loads((d/'launch.json').read_text());counts=json.loads((d/'validated.json').read_text());msgs=counts['window_source_completed'];run={'path':str(d),'level':meta['level'],'pattern':meta['pattern'],'repeat':meta['rep'],'messages':msgs,'seconds':counts['seconds'],'roles':{}}
  for side in ['source','target']:
   msgs=counts['window_source_completed' if side=='source' else 'window_target_received']
   samples=parse(d/(side+'.perf.data'));stats=stat(d/(side+'.stat.csv'));threads={};symbol=defaultdict(lambda:Counter());stages=Counter();rawtotal=sum(g['weight'] for g in samples['groups'].values())
   for tid,t in stats.items():
    g=samples['groups'].get(tid,{});weight=g.get('weight',0);factor=t['us']/msgs/weight if weight else 0
    ss={s:{'self_share':g.get('self',{}).get(s,0)/weight*100 if weight else 0,'inclusive_share':v/weight*100 if weight else 0,'self_us':g.get('self',{}).get(s,0)*factor,'inclusive_us':v*factor} for s,v in g.get('inclusive',{}).items()}
    st={s:v*factor for s,v in g.get('stages',{}).items()}
    for s,v in ss.items():symbol[s]['self_us']+=v['self_us'];symbol[s]['inclusive_us']+=v['inclusive_us']
    stages.update(st)
    threads[tid]={**t,'cpu_us_per_message':t['us']/msgs,'context_switches_per_message':t['switches']/msgs,'migrations_per_message':t['migrations']/msgs,'samples':g.get('samples',0),'stages':st,'symbols':ss}
   for g in samples['groups'].values():
    for sym,v in g['inclusive'].items():
     symbol[sym]['inclusive_sample_share']+=v/rawtotal*100 if rawtotal else 0
     symbol[sym]['self_sample_share']+=g['self'].get(sym,0)/rawtotal*100 if rawtotal else 0
   cpu=sum(t['cpu_us_per_message'] for t in threads.values())
   run['roles'][side]={'messages':msgs,'threads':threads,'cpu_us_per_message':cpu,'stages':dict(stages),'symbols':dict(symbol),'samples':samples['kept_samples'],'unknown_leaf_samples_weight':sum(g['self'].get('[unknown]',0) for g in samples['groups'].values()),'sample_weight':rawtotal,'context_switches_per_message':sum(t['switches'] for t in threads.values())/msgs,'migrations_per_message':sum(t['migrations'] for t in threads.values())/msgs}
  runs[d.name]=run
 (R/'profiles-summary.json').write_text(json.dumps(runs,indent=2))
 for name,r in runs.items():
  print(name,'msgs',r['messages'],'seconds',round(r['seconds'],3))
  for side,rr in r['roles'].items():print(side,'CPU',round(rr['cpu_us_per_message'],3),'CS',round(rr['context_switches_per_message'],3),'stages', {k:round(v,3) for k,v in rr['stages'].items()})
 return runs
if __name__=='__main__':analyze()
