import copy,hashlib,json,pathlib,statistics
j=pathlib.Path(__file__).parent

def counts(d):
 s,t=d['stats_after']['source'],d['stats_after']['target']
 assert s['completed']>0 and s['submitted']==s['completed']
 assert s['errors']==t['errors']==s['inFlight']==s['currentInFlight']==t['inFlight']==t['currentInFlight']==0
 assert not s['failure']
 assert t['allPhaseMessages']-d['stats_before']['target']['allPhaseMessages']==s['completed']
 assert t['received']==s['completed']
 assert d['memavailable_kb']>=12*1024*1024
 def role_total(role,key):
  return sum(v.get(key,0) for v in d['after']['source']['threads'].values() if v['role']==role)-sum(v.get(key,0) for v in d['before']['source']['threads'].values() if v['role']==role)
 assert role_total(2,'rotation_start')==role_total(2,'rotation_end')>0
 assert role_total(2,'completion_post')==s['completed']
 assert role_total(2,'completion_run')==0 and role_total(3,'completion_run')>=s['completed']
 assert role_total(2,'aligned')>=s['completed']
 for side in ('source','target'):
  a,b=d['after'][side],d['before'][side]
  assert a['sample_count']>b['sample_count']
  for tid,v in a['threads'].items():
   for k in ('mutex_lock','trylock_ok','malloc','calloc','realloc','aligned'):
    assert v[k]>=b['threads'].get(tid,{}).get(k,0)
  posts=sum(v.get('completion_post',0) for v in a['threads'].values())-sum(v.get('completion_post',0) for v in b['threads'].values())
  runs=sum(v.get('completion_run',0) for v in a['threads'].values())-sum(v.get('completion_run',0) for v in b['threads'].values())
  assert posts==runs,(side,posts,runs)
 return s['completed']

out={}
for stage in ('before','after-final'):
 path=j/f'{stage}-counts-framework-request-backpressure-1/counts-raw.json'
 if not path.exists():continue
 d=json.loads(path.read_text());n=counts(d)
 bad=copy.deepcopy(d);bad['stats_after']['source']['completed']+=1
 try:counts(bad)
 except AssertionError:negative='rejected completed+1'
 else:raise AssertionError('corrupt evidence accepted')
 badpost=copy.deepcopy(d)
 tid=next(t for t,v in badpost['after']['source']['threads'].items() if v['role']==2 and v.get('completion_post',0)>0)
 badpost['after']['source']['threads'][tid]['completion_post']-=1
 try:counts(badpost)
 except AssertionError:negative+='; rejected completion_post-1'
 else:raise AssertionError('missing completion post accepted')
 label='after' if stage=='after-final' else stage
 out[label]={'count_requests':n,'negative_control':negative,'patterns':{}}
 for pattern in ('request-backpressure','send-saturation'):
  series={}
  for impl in ('framework','binding'):
   values=[]
   for r in range(1,4):
    p=j/f'{stage}-throughput-{impl}-{pattern}-{r}'
    cell=json.loads((p/('results.json' if impl=='framework' else 'binding-results.json')).read_text())['cells'][0]
    assert cell['errors']==cell['abandoned']==cell['header_validation_failures']==0
    assert not cell['contaminated'] and not cell['drain_bound_hit']
    assert cell['submitted']==cell['completed'] and cell['payload_size']==1024 and cell['pattern']==pattern
    h=json.loads((p/'harness-sha256.json').read_text())
    assert hashlib.sha256(pathlib.Path(h['original']).read_bytes()).hexdigest()==h['sha256']
    values.append(cell['throughput_per_second'])
   series[impl]={'rounds':values,'median':statistics.median(values)}
  series['F/B']=series['framework']['median']/series['binding']['median'];out[label]['patterns'][pattern]=series
(j/'validation.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))

a=json.loads((j/'after-final-analysis.json').read_text());b=json.loads((j/'before-analysis.json').read_text())
for side in ('source','target'):
 assert sum(x['heap'] for x in a['roles'][side].values())<sum(x['heap'] for x in b['roles'][side].values())
 assert a['sites_exclusive'][side]['R-a']['locks']==0
 assert a['sites_exclusive'][side]['3']['heap']==0
 assert a['arena_upstream'][side]['heap']==0
assert a['sites']['target']['1']['locks']==0,'public registered handler still uses lazy serializer cache'
assert a['sites_exclusive']['target']['2']['heap']==0
assert a['sites_exclusive']['source']['6']['heap']==0
assert a['sites_exclusive']['target']['10']['heap']==0
assert a['sites_exclusive']['source']['12']['heap']==0
print('PASS changed-site removal checks including actual public target codec cache')
