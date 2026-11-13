import json,pathlib
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');rates=json.loads((P/'rates.json').read_text());langs=['cpp','java','dotnet','node'];bad=[]
for pattern,rate in rates.items():
 for lang in langs:
  for fw in [True,False]:
   candidates=[]
   for p in (P/'runs/alloc').glob(f"{lang}-{'F' if fw else 'B'}-{pattern}-*/meta.json"):
    m=json.loads(p.read_text())
    if m.get('ok') and m['job']['rate']==rate:
     a=json.loads((p.parent/'before.json').read_text());b=json.loads((p.parent/'after.json').read_text());actual=(b['stats']['source']['completed']-a['stats']['source']['completed'])/(b['time']-a['time']);candidates.append(actual)
   assert len(candidates)==1,(lang,fw,pattern,candidates)
   if candidates[0]<rate*.97:bad.append({'lang':lang,'fw':fw,'pattern':pattern,'rate':rate,'actual':candidates[0]})
if bad:
 (P/'rate-misses.json').write_text(json.dumps(bad,indent=2));raise SystemExit('Allocation capture did not reach common rate: see rate-misses.json; reduce this cell rate and repeat its eight allocation runs before CPU captures.')
for order,mode in [(200,'paced'),(300,'profile')]:
 jobs=[{'lang':lang,'fw':fw,'pattern':pattern,'mode':mode,'rate':rate,'warmup':10,'duration':30,'run':1} for pattern,rate in rates.items() for lang in langs for fw in [True,False]]
 (P/f'queue/{order}-{mode}.json').write_text(json.dumps(jobs,indent=2))
print('validated all allocation captures; queued paced and profile',flush=True)
