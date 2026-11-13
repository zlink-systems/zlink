import json,time
from pathlib import Path
base=Path('/root/jobs/fx/cx/v21/s033/jl57bp/raw/baseline-L5_7/request-backpressure-1024/counted/run1/zlink-framework-java-request-backpressure-1024')
for _ in range(180):
 try:
  raw=(base/'results.json').read_bytes();c=json.loads(raw)['cells'][0]
  if c.get('active_elapsed_ms')==10000:
   dest=base/'warmup-results.json'
   if not dest.exists(): dest.write_bytes(raw)
   print('preserved',dest);break
  if c.get('active_elapsed_ms')==5000: print('active already started');break
 except (OSError,ValueError,KeyError): pass
 time.sleep(.25)
