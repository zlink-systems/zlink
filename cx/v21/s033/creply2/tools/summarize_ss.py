import re,json,pathlib
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/creply2')
def parse(s):
 rows={};lines=s.splitlines()
 for i,l in enumerate(lines):
  if not l.startswith('ESTAB'):continue
  c=l.split();local=int(c[3].rsplit(':',1)[1]);peer=int(c[4].rsplit(':',1)[1]);details=lines[i+1] if len(lines)>i+1 else ''
  d={k:float(v) for k,v in re.findall(r'\b(\w+):([\d.]+)',details)}
  m=re.search(r'skmem:\(r(\d+),rb(\d+),t(\d+),tb(\d+)',details)
  if m:d.update({'rmem':int(m[1]),'rcvbuf':int(m[2]),'tmem':int(m[3]),'sndbuf':int(m[4])})
  m=re.search(r'retrans:(\d+)/(\d+)',details);d['total_retrans']=int(m[2]) if m else 0
  d.update({'local':local,'peer':peer,'recvq':int(c[1]),'sendq':int(c[2])});rows[(local,peer)]=d
 return rows
out={}
for run in sorted((P/'runs').iterdir()):
 f=run/'capture/ss.json'
 if not f.exists():continue
 ss=json.load(open(f));rows=[(r,parse(r['ss'])) for r in ss]
 if len(rows)<2:continue
 b,br=rows[0];a,ar=rows[-1];n=a['stats']['stats']['source']['completed']-b['stats']['stats']['source']['completed'];secs=(a['start']+a['end']-b['start']-b['end'])/2
 rr=[]
 for key,last in ar.items():
  if key not in br:continue
  first=br[key];delta={k:last.get(k,0)-first.get(k,0) for k in ('bytes_sent','bytes_received','bytes_acked','data_segs_in','data_segs_out','segs_in','segs_out','total_retrans')}
  delta['pure_acks_out']=delta['segs_out']-delta['data_segs_out'];delta['pure_acks_in']=delta['segs_in']-delta['data_segs_in']
  vals={k:[d[key].get(k,0) for r,d in rows if key in d] for k in ('rcvbuf','rcv_space','rcv_ssthresh','rtt','mss','rcvmss','snd_wnd','cwnd')}
  rr.append({'local':key[0],'peer':key[1],'delta':delta,'per_request':{k:v/n for k,v in delta.items()},'avg_data_seg_bytes':delta['bytes_sent']/delta['data_segs_out'] if delta['data_segs_out'] else None,'values':{k:{'first':v[0],'last':v[-1],'min':min(v),'max':max(v)} for k,v in vals.items()}})
 out[run.name]={'messages':n,'seconds':secs,'rate':n/secs,'connections':rr}
(P/'ss-summary.json').write_text(json.dumps(out,indent=2))
for name,r in out.items():
 print(name,round(r['rate']),[(str(d['local'])+'->'+str(d['peer']),round(d['per_request']['data_segs_out'],3),round(d['per_request']['pure_acks_out'],3),round(d['avg_data_seg_bytes'] or 0),d['values']['rcvbuf']) for d in r['connections']])
