from pathlib import Path
import json,re,collections,sys
J=Path('/root/jobs/fx/cx/v21/s033/cbatch')
NAMES={0:'read',1:'write',19:'readv',20:'writev',44:'sendto',45:'recvfrom',46:'sendmsg',47:'recvmsg',232:'epoll_wait',233:'epoll_ctl',281:'epoll_pwait'}
def address(s):
 ip,port=s.split(':');raw=bytes.fromhex(ip)
 return '.'.join(map(str,raw[::-1]))+':'+str(int(port,16)) if len(raw)==4 else ip+':'+str(int(port,16))
def summarize(cap):
 before=json.load(open(cap/'before.json'));after=json.load(open(cap/'after.json'));maps={}
 for l in (cap/'bpf.stderr').read_text().splitlines():
  try:d=json.loads(l)
  except ValueError:continue
  if d.get('type')=='map':maps.update(d['data'])
 p='send-saturation' in str(cap);n=after['stats']['target']['received']-before['stats']['target']['received'] if p else after['stats']['source']['completed']-before['stats']['source']['completed']
 elapsed=after['monotonic']-before['monotonic'];tcp={}
 for table in ('tcp','tcp6'):
  for l in before[table].splitlines()[1:]:
   fields=l.split();tcp[fields[9]]={'local':address(fields[1]),'remote':address(fields[2]),'state':fields[3]}
 result={'mode':json.load(open(cap/'meta.json'))['mode'],'capture':str(cap),'messages':n,'seconds':elapsed,'rate':n/elapsed,'stats_before':before['stats'],'stats_after':after['stats'],'roles':{},'map_keys':list(maps),'paired_epoll': '@epollret' in maps}
 for role in ('source','target'):
  pid=before['processes'][role]['pid'];b=before['processes'][role];a=after['processes'][role]
  threads={};fds={};rows=collections.defaultdict(lambda:collections.Counter())
  for tid,t in b['threads'].items():
   if tid not in a['threads']:continue
   ns=a['threads'][tid]['runtime_ns']-t['runtime_ns']
   threads[tid]={'comm':t['comm'],'cpu_us_per_message':ns/1e3/n,'cores':ns/1e9/elapsed,'io':'IO/' in t['comm']}
  for fd,v in b['fds'].items():
   fds[fd]=v.copy();m=re.match(r'socket:\[(\d+)\]',v['link'])
   if m and m[1] in tcp:fds[fd]['tcp']=tcp[m[1]]
  for mapname,field in [('@call','calls'),('@positive','positive'),('@bytes','positive_return_sum'),('@requested','requested')]:
   for key,value in maps.get(mapname,{}).items():
    pp,t,f,nr=map(int,key.split(','))
    if pp==pid:rows[t,f,nr][field]=value
  for key,value in maps.get('@returns',{}).items():
   pp,t,f,nr,ret=map(int,key.split(','))
   if ret>=2**63:ret-=2**64
   if pp==pid:rows[t,f,nr]['ret_'+str(ret)]=value
  for key,value in maps.get('@epolltimeout',{}).items():
   pp,t,f,timeout=map(int,key.split(','))
   if timeout>=2**63:timeout-=2**64
   if pp==pid:rows[t,f,232]['timeout_'+str(timeout)]=value
  for key,value in maps.get('@epollret',{}).items():
   pp,t,f,timeout,ret=map(int,key.split(','))
   if pp==pid:
    label=('zero_timeout_' if timeout==0 else 'blocking_')+('nonempty' if ret==1 else 'empty' if ret==0 else 'error')
    rows[t,f,232][label]+=value
  syscall_rows=[];sums={'io':collections.defaultdict(collections.Counter),'other':collections.defaultdict(collections.Counter)}
  for (t,f,nr),counts in sorted(rows.items()):
   thread=threads.get(str(t),{'comm':'unknown','io':False});fd=fds.get(str(f),{})
   syscall_rows.append({'tid':t,'comm':thread['comm'],'io':thread['io'],'fd':f,'fdlink':fd.get('link'),'tcp':fd.get('tcp'),'syscall':NAMES.get(nr,str(nr)),'counts':dict(counts),'per_message':{k:v/n for k,v in counts.items()}})
   sums['io' if thread['io'] else 'other'][NAMES.get(nr,str(nr))].update(counts)
  result['roles'][role]={'pid':pid,'threads':threads,'fds':fds,'rows':syscall_rows,'totals':{g:{s:{'counts':dict(c),'per_message':{k:v/n for k,v in c.items()}} for s,c in sy.items()} for g,sy in sums.items()},'io_cpu_us_per_message':sum(v['cpu_us_per_message'] for v in threads.values() if v['io'])}
  result['roles'][role]['commands']={m:{k:v for k,v in maps.get(m,{}).items() if k.startswith(str(pid)+',')} for m in ('@cmdsend','@cmdrecv','@recvempty','@recvcalls','@apiparts')}
 (cap/'summary.json').write_text(json.dumps(result,indent=2));return result
if __name__=='__main__':
 all=[]
 for p in J.glob('out/*/run1/*/capture*'):
  if p.parents[2].name.startswith('paced-'):continue
  if (p/'bpf.stderr').exists() and (p/'after.json').exists():
   r=summarize(p);all.append(r);print(p.parents[2].name,r['messages'],round(r['rate']),[(role,round(r['roles'][role]['io_cpu_us_per_message'],2)) for role in ('source','target')])
 (J/'bpf-summary.json').write_text(json.dumps(all,indent=2))
