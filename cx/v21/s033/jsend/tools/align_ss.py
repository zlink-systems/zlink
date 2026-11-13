import pathlib,json
p=pathlib.Path('/root/jobs/fx/cx/v21/s033/jsend');ns={};src=(p/'tools/summarize_ss.py').read_text();exec(src[:src.index('out={}')],ns);parse=ns['parse']
out={}
for run in (p/'runs').iterdir():
 cap=run/'capture'
 if not (cap/'perf-window.json').exists() or not (cap/'ss.json').exists():continue
 w=json.load(open(cap/'perf-window.json'));ss=json.load(open(cap/'ss.json'))
 ss=[r for r in ss if r['start']>w['before']['end'] and r['end']<w['after']['start']]
 if len(ss)<2:continue
 b,a=ss[0],ss[-1];br,ar=map(lambda x:parse(x['ss']),(b,a));n=a['stats']['stats']['source']['completed']-b['stats']['stats']['source']['completed'];rr=[]
 for key,last in ar.items():
  first=br[key];delta={k:last.get(k,0)-first.get(k,0) for k in ('data_segs_out','data_segs_in','segs_out','segs_in','bytes_sent','bytes_received','total_retrans')}
  ds=delta['data_segs_out'];delta['acks_out']=delta['segs_out']-ds;delta['acks_in']=delta['segs_in']-delta['data_segs_in'];intervals=[]
  for r0,r1 in zip(ss,ss[1:]):
   d0,d1=parse(r0['ss'])[key],parse(r1['ss'])[key];nn=r1['stats']['stats']['source']['completed']-r0['stats']['stats']['source']['completed'];intervals.append((d1.get('data_segs_out',0)-d0.get('data_segs_out',0))/nn)
  rr.append({'local':key[0],'peer':key[1],'delta':delta,'per_request':{k:v/n for k,v in delta.items()},'avg_data_seg_bytes':delta['bytes_sent']/ds if ds else None,'data_segments_per_request_intervals':intervals,'rcvbuf':first['rcvbuf'],'rcvmss':first.get('rcvmss'),'mss':first.get('mss')})
 out[run.name]={'messages':n,'seconds':a['start']-b['start'],'connections':rr}
(p/'aligned-ss-summary.json').write_text(json.dumps(out,indent=2))
for name,r in out.items():
 if 'preflight' in name:continue
 print(name,[(str(t['local'])+'->'+str(t['peer']),round(t['per_request']['data_segs_out'],4),round(t['per_request']['acks_out'],4),[round(x,3) for x in t['data_segments_per_request_intervals']]) for t in r['connections']])
