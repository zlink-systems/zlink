import json,pathlib,statistics
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');patterns=['send-saturation','request-backpressure','request-serial'];langs=['cpp','java','dotnet','node'];rates={}
for pattern in patterns:
 values=[]
 for lang in langs:
  for fw in [True,False]:
   cells=[]
   for p in (P/'runs/unpaced').glob(f"{lang}-{'F' if fw else 'B'}-{pattern}-*/meta.json"):
    m=json.loads(p.read_text())
    if m.get('ok'):cells.append(m['throughput'])
   assert len(cells)==3,(lang,fw,pattern,len(cells));values+=cells
 step=100 if pattern=='request-serial' else 1000
 rates[pattern]=max(step,int(min(values)*.4/step)*step)
(P/'rates.json').write_text(json.dumps(rates,indent=2))
for order,mode,duration in [(150,'alloc',20)]:
 jobs=[]
 for pattern in patterns:
  for lang in langs:
   for fw in [True,False]:jobs.append({'lang':lang,'fw':fw,'pattern':pattern,'mode':mode,'rate':rates[pattern],'warmup':10,'duration':duration,'run':1})
 (P/f'queue/{order}-{mode}.json').write_text(json.dumps(jobs,indent=2))
print('segment rates',rates,flush=True)
