import json,sys,time,urllib.request
source,target,out=sys.argv[1:]
with open(out,'a',buffering=1) as f:
 for _ in range(1100):
  row={'unix_ns':time.time_ns(),'monotonic_ns':time.monotonic_ns()}
  for role,url in [('source',source),('target',target)]:
   try:
    with urllib.request.urlopen(url+'/bench/stats',timeout=1) as r: row[role]=json.load(r)
   except Exception as e: row[role+'_error']=str(e)
  f.write(json.dumps(row)+'\n')
  if 'source_error' in row and 'target_error' in row: break
  time.sleep(.5)
