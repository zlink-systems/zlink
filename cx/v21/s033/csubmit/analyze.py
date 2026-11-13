import json,pathlib,re,statistics
job=pathlib.Path(__file__).resolve().parent
records=[]
for pattern in ('request-backpressure','send-saturation'):
 for run in (1,2,3):
  for variant in ('main','branch'):
   record={'pattern':pattern,'run':run,'variant':variant}
   for impl,key in (('zlink-cpp','binding'),('zlink-framework-cpp','framework')):
    directory=job/'measure'/str(run)/pattern/variant/'run1'/f'{impl}-{pattern}-1024'
    cell=json.loads((directory/'results.json').read_text())['cells'][0]
    assert not cell.get('contaminated',False) and not cell.get('drain_bound_hit',False),(directory,cell)
    assert cell['errors']==0 and cell['abandoned']==0,(directory,cell)
    assert cell['server_rejected_count']==0 and cell['header_validation_failures']==0,(directory,cell)
    assert cell['completed']==cell['target_stats']['received'],(directory,cell)
    assert cell['target_stats']['errors']==0 and cell['target_stats']['rejected']==0,(directory,cell)
    samples=json.loads((directory/'capture/samples.json').read_text())
    first,last=samples[0],samples[-1]
    received=last['stats']['target']['received']-first['stats']['target']['received']
    completed=last['stats']['source']['completed']-first['stats']['source']['completed']
    assert received>0 and completed>0
    threads={}
    previous=dict(first['processes']['source']['threads'])
    for sample in samples[1:]:
     for tid,now in sample['processes']['source']['threads'].items():
      before=previous.get(tid,{'runtime_ns':0,'voluntary':0,'involuntary':0})
      total=threads.setdefault(tid,{'runtime_ns':0,'switches':0})
      total['runtime_ns']+=max(0,now['runtime_ns']-before['runtime_ns'])
      total['switches']+=sum(max(0,now[k]-before[k]) for k in ('voluntary','involuntary'))
      previous[tid]=now
    data={'throughput':cell['throughput_per_second'],'cell':cell,'directory':str(directory),
          'captured_received':received,'captured_completed':completed,'seconds':last['time']-first['time'],
          'threads':threads,'source_switches_per_received':sum(t['switches'] for t in threads.values())/received}
    if key=='framework':
     marked=set(re.findall(r'ROLE source-submitter tid=(\d+)',(directory/'source.log').read_text()))
     active={tid:threads[tid] for tid in marked if tid in threads and threads[tid]['runtime_ns']>1000000}
     assert len(active)==1,(directory,marked,active)
     tid,thread=next(iter(active.items()))
     data.update(submitter_tid=tid,submitter_cpu_us_per_completed=thread['runtime_ns']/1000/completed,
                 submitter_cpu_us_per_received=thread['runtime_ns']/1000/received,
                 submitter_cores=thread['runtime_ns']/1e9/data['seconds'])
    record[key]=data
   record['ratio']=record['framework']['throughput']/record['binding']['throughput']
   records.append(record)
summary=[]
for pattern in ('request-backpressure','send-saturation'):
 for variant in ('main','branch'):
  rows=[r for r in records if r['pattern']==pattern and r['variant']==variant]
  b=statistics.median(r['binding']['throughput'] for r in rows)
  f=statistics.median(r['framework']['throughput'] for r in rows)
  summary.append({'pattern':pattern,'variant':variant,'binding_median':b,'framework_median':f,
                  'ratio_of_medians':f/b,'median_run_ratio':statistics.median(r['ratio'] for r in rows),
                  'submitter_cpu_us_per_completed_median':statistics.median(r['framework']['submitter_cpu_us_per_completed'] for r in rows),
                  'submitter_cpu_us_per_received_median':statistics.median(r['framework']['submitter_cpu_us_per_received'] for r in rows)})
result={'records':records,'summary':summary};(job/'summary.json').write_text(json.dumps(result,indent=2))
print(json.dumps(summary,indent=2))
