from pathlib import Path
import csv,collections,json,math,sys
j=Path(__file__).parent;prefix=sys.argv[1] if len(sys.argv)>1 else 'pm'
def quantile(v,q):s=sorted(v);return s[math.ceil(len(s)*q)-1]
report={}
for variant in ('main','branch'):
 path=j/(prefix+'-'+variant+'.csv')
 if not path.exists():continue
 groups=collections.defaultdict(list)
 for a,b,c,d,e in csv.reader(path.open()):groups[int(a)].append({'ns':int(b),'tid':int(c),'stage':d,'key':e})
 records=[]
 for id,events in groups.items():
  if id==1 or events[-1]['stage']!='request_complete':continue
  stages={e['stage']:e for e in events}
  if 'handler_enter' not in stages:continue
  start=stages['request_start']['ns'];end=stages['request_complete']['ns']
  handler=stages['handler_enter'];gate=next(e for e in reversed(events) if e['stage']=='gate_acquired' and e['ns']<handler['ns'])
  published=next(e for e in reversed(events) if e['stage']=='gate_executor_enqueue' and e['ns']<gate['ns'] and e['key']==gate['key'])
  row={'id':id,'total_ms':(end-start)/1e6,'events':events,'stages_ms':{'before_gate_enqueue':(published['ns']-start)/1e6,'gate_executor_queue':(gate['ns']-published['ns'])/1e6,'gate_to_handler':(handler['ns']-gate['ns'])/1e6,'handler':(stages['handler_exit']['ns']-handler['ns'])/1e6,'completion':(end-stages['handler_exit']['ns'])/1e6}}
  records.append(row)
 if not records:continue
 chosen=sorted(records,key=lambda r:r['total_ms'])[math.ceil(.99*len(records))-1]
 result={'n':len(records),'p50_ms':quantile([r['total_ms'] for r in records],.5),'p99_ms':quantile([r['total_ms'] for r in records],.99),'chosen':chosen,'stage_p50_ms':{s:quantile([r['stages_ms'][s] for r in records],.5) for s in records[0]['stages_ms']},'stage_p99_ms':{s:quantile([r['stages_ms'][s] for r in records],.99) for s in records[0]['stages_ms']},'records':records}
 report[variant]=result
 print(variant,'n',len(records),'p50',result['p50_ms'],'p99',result['p99_ms'],'id',chosen['id'],chosen['stages_ms'])
(j/(prefix+'-analysis.json')).write_text(json.dumps(report,indent=2))
