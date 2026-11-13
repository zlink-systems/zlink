from pathlib import Path
import glob, json, statistics
BASE=Path('/root/jobs/fx/cx/v21/s033/cbatch')
WIRE={('request-backpressure','zlink-framework-cpp','request'):1427,
      ('request-backpressure','zlink-framework-cpp','reply'):1417,
      ('request-backpressure','zlink-cpp','request'):1252,
      ('request-backpressure','zlink-cpp','reply'):1252,
      ('send-saturation','zlink-framework-cpp','request'):1389,
      ('send-saturation','zlink-cpp','request'):1244}

def data_row(cap, role, syscall):
    rows=[x for x in cap['roles'][role]['rows'] if x['syscall']==syscall and x.get('tcp')]
    byfd={}
    for x in rows:
        d=byfd.setdefault(x['fd'], {'bytes':0,'positive':0,'calls':0,'tcp':x['tcp']})
        d['bytes']+=x['counts'].get('positive_return_sum',0)
        d['positive']+=x['counts'].get('positive',0)
        d['calls']+=x['counts'].get('calls',0)
    return max(byfd.values(),key=lambda x:x['bytes']) if byfd else {'bytes':0,'positive':0,'calls':0,'tcp':None}

def run_result(name,pattern,impl):
    files=glob.glob(str(BASE/f'out/{name}/run1/{impl}-{pattern}-1024/results.json'))
    if not files:return None
    c=json.load(open(files[0]))['cells'][0]
    return c

def classify(name):
    pattern='request-backpressure' if 'req' in name else 'send-saturation'
    if 'paced-' in name: impl='zlink-cpp'
    elif name.startswith('fw-'): impl='zlink-framework-cpp'
    else: impl='zlink-cpp'
    return pattern,impl

summaries=json.load(open(BASE/'bpf-summary.json'))
rows=[]
for x in summaries:
    name=Path(x['capture']).parents[2].name
    pattern,impl=classify(name)
    role=x['roles']
    if pattern=='request-backpressure':
        dirs={
          'request':('source','sendto','target','recvfrom'),
          'reply':('target','sendto','source','recvfrom')}
    else:
        dirs={'request':('source','sendto','target','recvfrom')}
    batches={}
    for direction,(sr,ss,rr,rs) in dirs.items():
        sw=WIRE[(pattern,impl,direction)]
        a=data_row(x,sr,ss); b=data_row(x,rr,rs)
        batches[direction]={
          'send':a['bytes']/sw/a['positive'] if a['positive'] else None,
          'receive':b['bytes']/sw/b['positive'] if b['positive'] else None,
          'send_successful_calls':a['positive'],'receive_successful_calls':b['positive'],
          'send_fd':a['tcp'],'receive_fd':b['tcp'],
          'send_bytes_per_message':a['bytes']/x['messages'] if x['messages'] else None,
          'receive_bytes_per_message':b['bytes']/x['messages'] if x['messages'] else None,
        }
    result=run_result(name,pattern,impl)
    wire_messages=batches['request']['send_bytes_per_message'] and (data_row(x,'source','sendto')['bytes']/WIRE[(pattern,impl,'request')])
    series='rate-limited binding' if name.startswith('spin-') else 'baseline'
    rows.append({'run':name,'pattern':pattern,'implementation':impl,'series':series,'messages':x['messages'],
      'counter_rate':x['rate'],'benchmark_rate':result.get('throughput_per_second') if result else None,
      'io_cpu_us_per_message':{r:role[r]['io_cpu_us_per_message']*x['messages']/wire_messages for r in ('source','target')},
      'hottest_io_us_per_message':{r:max((t['cpu_us_per_message'] for t in role[r]['threads'].values() if t['io']),default=0) for r in ('source','target')},
      'batches':batches,'capture':x['capture']})
(BASE/'analysis-runs.json').write_text(json.dumps(rows,indent=2))
for group in sorted(set((r['pattern'],r['implementation'],r['series']) for r in rows)):
  subset=[r for r in rows if (r['pattern'],r['implementation'],r['series'])==group]
  if len(subset)<3:continue
  print('\n',group,'n=',len(subset))
  print('run benchmark rates',[(r['run'],round(r['benchmark_rate'] or 0),round(r['counter_rate'])) for r in subset])
  for d in subset[0]['batches']:
    print(d,'send',round(statistics.median(r['batches'][d]['send'] for r in subset),3),'recv',round(statistics.median(r['batches'][d]['receive'] for r in subset),3))
  print('IO CPU', {side:round(statistics.median(r['io_cpu_us_per_message'][side] for r in subset),2) for side in ('source','target')})
  print('hottest IO CPU', {side:round(statistics.median(r['hottest_io_us_per_message'][side] for r in subset),2) for side in ('source','target')})
