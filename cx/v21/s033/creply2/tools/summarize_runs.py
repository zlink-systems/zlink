from pathlib import Path
import copy,json,statistics as st
P=Path('/root/jobs/fx/cx/v21/s033/creply2')
def check(r):
 c=r['cells'][0];t=c['target_stats']
 assert c['submitted']==c['completed']==t['received']
 assert c['errors']==c['abandoned']==c['header_validation_failures']==t['errors']==0
 assert not c.get('contaminated') and not c.get('drain_bound_hit')
 assert t.get('rejected',0) in (None,0)
 return c
rows={};valid=[];minimum=[]
for f in sorted((P/'runs').glob('*/results.json')):
 r=json.loads(f.read_text());c=check(r);valid.append(f.parent.name)
 meta=json.loads((f.parent/'meta.json').read_text())['arguments']
 minimum.append(json.loads((f.parent/'runner-meta.json').read_text())['min_memavailable_kb'])
 if 'unpaced' not in f.parent.name:continue
 key=f"{meta['variant']}/{meta['pattern']}/{meta['payload']}"
 rows.setdefault(key,[]).append({'name':f.parent.name,'rate':c['throughput_per_second'],'us':1e6/c['throughput_per_second'],'caller_us':c['submit_thread_cores']*1e6/c['throughput_per_second']})
summary={}
for key,runs in rows.items():
 assert len(runs)==5,(key,len(runs))
 summary[key]={'runs':runs,'rate':st.median(r['rate'] for r in runs),'min':min(r['rate'] for r in runs),'max':max(r['rate'] for r in runs),'caller_us':st.median(r['caller_us'] for r in runs)}
for key,s in summary.items():
 variant,pattern,size=key.split('/')
 if variant=='binding':continue
 b=summary[f'binding/{pattern}/{size}'];s.update(ratio=s['rate']/b['rate'],framework_us=1e6/s['rate'],binding_us=1e6/b['rate'],budget_us=1e6/b['rate']/.6,over_budget_us=1e6/s['rate']-1e6/b['rate']/.6)
negative=[];sample=json.loads(next((P/'runs').glob('*/results.json')).read_text())
for fault in ('count','error','abandoned','headers','contaminated','drain'):
 r=copy.deepcopy(sample);c=r['cells'][0]
 if fault=='count':c['completed']-=1
 elif fault=='error':c['errors']=1
 elif fault=='abandoned':c['abandoned']=1
 elif fault=='headers':c['header_validation_failures']=1
 elif fault=='contaminated':c['contaminated']=True
 else:c['drain_bound_hit']=True
 try:check(r)
 except AssertionError:negative.append(fault)
 else:raise RuntimeError(('accepted corrupt data',fault))
assert min(minimum)>=12*1048576
out={'throughput':summary,'valid_runs':valid,'negative_inputs_rejected':negative,'min_memavailable_kb':min(minimum),'mutex':{}}
for variant in ('main','direct'):
 f=P/f'runs/{variant}-mutex/mutex-window.json'
 if not f.exists():continue
 m=json.loads(f.read_text());delta={k:m['after'][k]-m['before'][k] for k in ('calls','contended','wait_ns','errors')}
 n=json.loads((f.parent/'results.json').read_text())['cells'][0]['completed']
 assert delta['calls']==n and delta['errors']==0
 out['mutex'][variant]={'requests':n,**delta,'wait_us_per_request':delta['wait_ns']/n/1000}
(P/'summary.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
