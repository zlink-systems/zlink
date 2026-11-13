from pathlib import Path
import json,re,statistics
P=Path('/root/jobs/fx/cx/v21/s033/creply2')
def load(n):
 f=P/n;return json.loads(f.read_text()) if f.exists() else {}
perf=load('perf-summary.json');ss=load('aligned-ss-summary.json');syscalls=load('syscall-summary.json')
rows={}
for name,v in perf.items():
 if name not in ss:continue
 conns=ss[name]['connections'];server=5247 if name.startswith('binding') else 5254
 reply=max((c for c in conns if c['local']==server),key=lambda c:c['delta']['bytes_sent'])
 t=max((t for t in v['threads'].values() if t['role']=='target'),key=lambda t:t['stage_cycles_per_request'].get('send',0))
 cyc=t['stage_cycles_per_request'].get('send',0);pack=reply['per_request']['data_segs_out']
 rows[name]={'rate':v['rate'],'reply_bytes_per_request':reply['per_request']['bytes_sent'],'packets_per_request':pack,'average_data_segment_bytes':reply['avg_data_seg_bytes'],'reply_ACKs_per_request':reply['per_request']['acks_in'],'target_thread':t['comm'],'send_cycles_per_request':cyc,'send_cycles_per_packet':cyc/pack,'cycles_samples':t['samples'],'reply_connection':reply}
throughput={}
for f in (P/'runs').glob('*/results.json'):
 name=f.parent.name;c=json.loads(f.read_text())['cells'][0]
 if 'unpaced' not in name:continue
 args=json.loads((f.parent/'meta.json').read_text())['arguments'];key=f"{args['variant']}/{args['pattern']}/{args['payload']}"
 throughput.setdefault(key,[]).append({'case':name,'rate':c['throughput_per_second'],'caller_us':c['submit_thread_cores']*1e6/c['throughput_per_second'],'client_cpu_us':c['client_cores']*1e6/c['throughput_per_second']})
for key,vals in list(throughput.items()):
 throughput[key]={'runs':vals,'rate_median':statistics.median(r['rate'] for r in vals),'rate_min':min(r['rate'] for r in vals),'rate_max':max(r['rate'] for r in vals),'caller_us_median':statistics.median(r['caller_us'] for r in vals)}
result={'same_rate':rows,'throughput':throughput}
(P/'tables.json').write_text(json.dumps(result,indent=2))
print(json.dumps({**result,'same_rate':{k:{kk:vv for kk,vv in v.items() if kk!='reply_connection'} for k,v in rows.items()}},indent=2))
