import json,pathlib,collections,re
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/i3560b');rows=json.load(open(J/'summary.json'));out={}
for c in rows:
 mode=pathlib.Path(c['raw']).parent.name;impl=pathlib.Path(c['raw']).name
 key=mode+'/'+impl; a={'raw':c['raw'],'request_ok_bpf':c['request_ok_bpf'],'completed_stats_delta':c['completed_stats_delta'],'roles':{}}
 for role,r in c['roles'].items():
  b={'pid':r['pid'],'io_us_per_request':r['io_us_per_request'],'io_tid_count':len(r['io_tids_union']),'io_union_missing_endpoint':[v for v in r['io_tids_union'] if not v['present_before'] or not v['present_after']]}
  for mapname in ('send','reads','writes','comp_result','comp_record','event_mask','router_enter','router_result'):
   ms=r['maps'].get(mapname,[])
   if mapname in ('send','reads','writes'):
    if ms:b[mapname]={'count':sum(x['count'] for x in ms),'per_request':sum(x['count'] for x in ms)/c['request_ok_bpf']}
   elif ms:
    agg=collections.Counter()
    for x in ms:agg[x['key'][-1]]+=x['count']
    b[mapname]=dict(agg)
  if 'comp_result' in b:b['requests_per_completion_drain']=b.get('comp_record',{}).get('2',0)/b['comp_result'].get('201',1)
  a['roles'][role]=b
 out[key]=a
validation=[]
for p in sorted(J.glob('raw-*/run*/**/results.json')):
 c=json.load(open(p))['cells'][0]
 validation.append({'raw':str(p),'rps':c['throughput_per_second'],**{k:c.get(k) for k in ('errors','abandoned','header_validation_failures','server_rejected_count','contaminated','drain_bound_hit')}})
(J/'evidence-summary.json').write_text(json.dumps({'measurements':out,'validation':validation},indent=2)+'\n')
for k,a in out.items():
 if any(x in k for x in ('raw-light/','raw-light-matched','raw-stacks/')):
  print(k,{r:b.get('send') for r,b in a['roles'].items()})
print('union missing',sum(len(b['io_union_missing_endpoint']) for a in out.values() for b in a['roles'].values()))
print('cells',len(validation),'failed',[v for v in validation if any(v[k] for k in ('errors','abandoned','header_validation_failures','server_rejected_count','contaminated','drain_bound_hit'))])
