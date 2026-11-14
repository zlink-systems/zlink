import pathlib,subprocess,json,re,time,bisect,shutil,collections
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/d3l3');epoch=time.time()-time.clock_gettime(time.CLOCK_MONOTONIC_RAW);(j/'raw/perf-clock-map.json').write_text(json.dumps(dict(epochOffsetSeconds=epoch,wall=time.time(),monotonic=time.monotonic()),indent=2))
for cell in ['perf-L2-send1K','perf-L3-send1K','exp-no-clone-perf-send1K']:
 d=j/'raw'/cell;window=json.loads((d/'window.json').read_text());ids=json.loads((d/'pids.json').read_text())
 for role in ['source','target']:
  p=pathlib.Path('/tmp')/('perf-'+str(ids[role])+'.map');dest=d/(role+'-jit.map');shutil.copyfile(p,dest)
  entries=[]
  for l in p.read_text().splitlines():
   try:a,size,name=l.split(' ',2);entries.append((int(a,16),int(size,16),name))
   except ValueError:pass
  entries.sort();starts=[e[0] for e in entries]
  def resolve(addr,sym):
   a=int(addr,16);ix=bisect.bisect_right(starts,a)-1
   if ix>=0 and a<entries[ix][0]+entries[ix][1]:return entries[ix][2]
   return sym
  path=d/(role+'-perf-script.txt')
  if not path.exists():
   with open(path,'w') as f,open(d/(role+'-perf-script.log'),'w') as err:subprocess.run(['taskset','-c','6-7,14-15','perf','script','--ns','-F','comm,pid,tid,time,event,ip,sym,dso','-i',str(d/(role+'.perf.data'))],stdout=f,stderr=err,check=True)
  samples=[]
  for block in path.read_text().strip().split('\n\n'):
   lines=block.splitlines();m=re.match(r'(.+?)\s+(\d+)/(\d+)\s+(\d+\.\d+):',lines[0])
   if not m:continue
   comm,pid,tid,ts=m.groups();ms=(float(ts)+epoch)*1000
   if not window['startUnixMs']<=ms<=window['endUnixMs']:continue
   stack=[]
   for line in lines[1:]:
    sm=re.match(r'\s*([0-9a-f]+)\s+(.*?)\s+\((.*)\)\s*$',line)
    if sm:stack.append(resolve(sm[1],sm[2]))
   samples.append(dict(tid=int(tid),comm=comm,time=float(ts),stack=stack))
  threads={}
  for tid in sorted({s['tid'] for s in samples}):
   selected=[s for s in samples if s['tid']==tid];leaf=collections.Counter(s['stack'][0] if s['stack'] else '?' for s in selected);inc=collections.Counter(n for s in selected for n in set(s['stack']));threads[tid]=dict(comm=selected[0]['comm'],samples=len(selected),leaf=leaf.most_common(25),inclusive=inc.most_common(50))
  leaf=collections.Counter(s['stack'][0] if s['stack'] else '?' for s in samples);inc=collections.Counter(n for s in samples for n in set(s['stack']))
  (d/(role+'-perf-summary.json')).write_text(json.dumps(dict(samples=len(samples),window=window,clockOffsetSeconds=epoch,threads=threads,leaf=leaf.most_common(80),inclusive=inc.most_common(200)),indent=2))
  print(cell,role,len(samples),flush=True)
