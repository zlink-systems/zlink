import json,pathlib
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');(P/'queue').mkdir(exist_ok=True)
langs=['cpp','java','dotnet','node'];patterns=['send-saturation','request-backpressure','request-serial'];jobs=[]
for n in range(1,4):
 for pattern in patterns:
  for lang in (langs if n!=2 else list(reversed(langs))):
   for fw in ([True,False] if n!=2 else [False,True]):jobs.append({'lang':lang,'fw':fw,'pattern':pattern,'mode':'unpaced','run':n,'warmup':10,'duration':10})
(P/'queue/100-unpaced.json').write_text(json.dumps(jobs,indent=2));print(len(jobs),'unpaced jobs queued')
