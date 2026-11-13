import json,pathlib,re
j=pathlib.Path(__file__).resolve().parent
rows=[]
for variant in ('main','branch'):
 d=j/'census'/variant/'run1/zlink-framework-cpp-request-backpressure-1024'
 a=json.loads((d/'capture/before.json').read_text());b=json.loads((d/'capture/after.json').read_text())
 n=b['stats']['source']['completed']-a['stats']['source']['completed'];assert n>0
 def slots(s):return {str(v['tid']):v for v in s['processes']['source']['alloc']['slots'] if v['tid']}
 old,new=slots(a),slots(b)
 marked=set(re.findall(r'ROLE source-submitter tid=(\d+)',(d/'source.log').read_text()))
 active=[tid for tid in marked if tid in b['processes']['source']['threads'] and tid in a['processes']['source']['threads'] and b['processes']['source']['threads'][tid]['runtime_ns']-a['processes']['source']['threads'][tid]['runtime_ns']>1000000]
 assert len(active)==1,(variant,active)
 tid=active[0]
 def calls(tid):return sum(new[tid][k]-old.get(tid,{}).get(k,0) for k in ('malloc','calloc','realloc'))
 row={'variant':variant,'tid':tid,'completed_messages':n,'seconds':b['monotonic']-a['monotonic'],
      'submitter_heap_calls':calls(tid),'submitter_heap_calls_per_completed':calls(tid)/n,
      'process_heap_calls_per_completed':sum(calls(t) for t in new)/n}
 rows.append(row)
(j/'census-summary.json').write_text(json.dumps(rows,indent=2));print(json.dumps(rows,indent=2))
