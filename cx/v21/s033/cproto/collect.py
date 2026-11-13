import json,pathlib,statistics
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/cproto'); result={}
for side in ['main','step2','step4','final','final-shared-writer']:
    result[side]={}
    for pattern in ['request-backpressure','request-serial']:
        runs=[]
        for p in sorted((j/'log'/side).glob(f'run*/zlink-framework-cpp-{pattern}-1024/results.json')):
            c=json.loads(p.read_text())['cells'][0]
            if c.get('contaminated') or c.get('drain_bound_hit') or c.get('errors') or c.get('abandoned'):
                raise SystemExit(f'invalid measurement: {p}')
            assert c['submitted']==c['completed']==c['target_stats']['received'],p
            assert c['server_rejected_count']==0 and c['target_stats']['errors']==0,p
            rate=c['throughput_per_second']
            runs.append({'path':str(p),'caller_us':c['submit_thread_cores']*1e6/rate,'req_s':rate,
                         'submit_cores':c['submit_thread_cores'],'source_us':c['client_cores']*1e6/rate,
                         'non_submit_us':c['non_submit_cores']*1e6/rate,
                         'latency_mean_us':c['latency_mean_ms']*1000,'latency_p95_us':c['latency_p95_ms']*1000,
                         'latency_p99_us':c['latency_p99_ms']*1000,'completed':c['completed']})
        if not runs:continue
        metrics={k:statistics.median(r[k] for r in runs) for k in runs[0] if k not in ['path','completed']}
        result[side][pattern]={'n':len(runs),'median':metrics,'runs':runs}
(j/'summary.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
main=result['main'].get('request-backpressure',{}).get('median',{}).get('caller_us',0)
for side,patterns in result.items():
 for pattern,values in patterns.items():
    m=values['median'];print(side,pattern,'n=',values['n'],'caller_us=',round(m['caller_us'],6),'req/s=',round(m['req_s'],3),'delta=',round(m['caller_us']-main,6),'roundtrip_mean_us=',round(m['latency_mean_us'],3))
