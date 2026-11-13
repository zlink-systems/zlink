import re,json,collections,sys
from pathlib import Path
MAGIC=b'\x5a\x01'
def decode(data):
 offset=0;resync=0;frames=[];records=[];controls=[];pending=[]
 while offset+8<=len(data):
  if data[offset:offset+2]!=MAGIC:
   nxt=data.find(MAGIC,offset+1)
   if nxt<0:resync+=len(data)-offset;offset=len(data);break
   resync+=nxt-offset;offset=nxt
   if offset+8>len(data):break
  flags,kind=data[offset+2:offset+4];length=int.from_bytes(data[offset+4:offset+8],'big');hdr=8+(8 if kind else 0)
  if flags>31 or kind>3 or length>1048576:offset+=1;resync+=1;continue
  if offset+hdr+length>len(data):break
  body=data[offset+hdr:offset+hdr+length]
  item={'flags':flags,'kind':kind,'body_bytes':length,'wire_bytes':length+hdr,'prefix_hex':body[:80].hex()}
  frames.append(item)
  if flags&30:controls.append(item)
  else:
   if kind and pending:pending=[]
   pending.append(item)
   if not flags&1:records.append(pending);pending=[]
  offset+=hdr+length
 return {'frames':frames,'records':records,'controls':controls,'resync_bytes':resync,'tail_bytes':len(data)-offset,'pending_frames':len(pending)}

def parse(path):
 unfinished={};calls=[];streams=collections.defaultdict(bytearray);labels={};stacks=collections.defaultdict(collections.Counter);last=None
 for l in Path(path).open(errors='replace'):
  if l.lstrip().startswith('>'):
   if last is not None:stacks[last].update([l.strip()])
   continue
  m=re.match(r'\s*(\d+)\s+([0-9]+\.[0-9]+)\s+(.*)',l)
  if not m:continue
  tid=int(m[1]);t=float(m[2]);s=m[3]
  if '<unfinished ...>' in s:unfinished[tid]=(t,s.replace('<unfinished ...>',''));last=None;continue
  if s.startswith('<... '):
   r=re.match(r'<\.\.\. (\w+) resumed>(.*)',s)
   if not r or tid not in unfinished:continue
   t,pre=unfinished.pop(tid);s=pre+r[2]
  c=re.match(r'(\w+)\((\d+)(?:<([^>]*)>)?',s)
  ret=re.search(r'\)\s+=\s+(-?\d+)(?:\s+([A-Z]\w+))?',s)
  if not c or not ret:continue
  name,fd=c[1],int(c[2]);n=int(ret[1]);last=(tid,fd,name);labels[fd]=c[3]
  calls.append({'tid':tid,'time':t,'fd':fd,'syscall':name,'ret':n,'errno':ret[2]})
  if name in ('sendto','recvfrom','sendmsg','recvmsg','writev','readv') and n>0:
   chunks=re.findall(r'"((?:\\x[0-9a-f]{2})*)"',s)
   data=b''.join(bytes.fromhex(x.replace('\\x','')) for x in chunks)
   direction='send' if name in ('sendto','sendmsg','writev') else 'recv'
   if len(data)>=n:streams[fd,direction]+=data[:n]
 result={'path':str(path),'syscalls':dict(collections.Counter(c['syscall'] for c in calls)),'calls':calls,'streams':[],'stacks':[{ 'tid':k[0],'fd':k[1],'syscall':k[2],'frames':dict(v)} for k,v in stacks.items()]}
 for (fd,direction),data in streams.items():
  d=decode(data);rh=collections.Counter((tuple(f['body_bytes'] for f in rec),tuple(f['kind'] for f in rec),sum(f['wire_bytes'] for f in rec)) for rec in d['records'])
  result['streams'].append({'fd':fd,'direction':direction,'label':labels.get(fd),'bytes':len(data),'frames':len(d['frames']),'records':len(d['records']),'record_hist':[{'part_sizes':k[0],'kinds':k[1],'wire_bytes':k[2],'count':v} for k,v in rh.most_common()],'control_hist':[{'flags':k[0],'kind':k[1],'body_bytes':k[2],'prefix_hex':k[3],'count':v} for k,v in collections.Counter((f['flags'],f['kind'],f['body_bytes'],f['prefix_hex']) for f in d['controls']).most_common()],'first_records':d['records'][:2],'resync_bytes':d['resync_bytes'],'tail_bytes':d['tail_bytes']})
 return result
if __name__=='__main__':
 for path in sys.argv[1:]:
  r=parse(path);out=Path(path).with_suffix('.wire.json');out.write_text(json.dumps(r,indent=2));print(path,[(s['fd'],s['direction'],s['records'],s['record_hist'][:3],len(s['control_hist'])) for s in r['streams']])
