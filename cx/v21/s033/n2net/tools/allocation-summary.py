import json,sys
from pathlib import Path
base=Path(sys.argv[1]); result={}
for f in base.glob('*.heapsnapshot'):
 s=json.loads(f.read_text()); fields=s['snapshot']['meta']['trace_function_info_fields']; infos=s['trace_function_infos']; strings=s['strings']; rows=[]
 def walk(nodes,active=False):
  for i in range(0,len(nodes),5):
   start=nodes[i+1]*len(fields)
   name=strings[infos[start+fields.index('name')]]; url=strings[infos[start+fields.index('script_name')]]
   own=active or name=='runMeasuredHeader'
   if own and nodes[i+2]: rows.append(dict(name=name,url=url,line=infos[start+fields.index('line')],objects=nodes[i+2],bytes=nodes[i+3]))
   walk(nodes[i+4],own)
 walk(s['trace_tree'])
 result[f.stem]=dict(calls=2000,objectsPerMessage=sum(r['objects'] for r in rows)/2000,bytesPerMessage=sum(r['bytes'] for r in rows)/2000,frames=rows)
(base/'allocation-summary.json').write_text(json.dumps(result,indent=2))
for k,v in result.items():
 print(k,round(v['objectsPerMessage'],3),round(v['bytesPerMessage'],2))
 print(*[(r['name'],r['objects']/2000,r['bytes']/2000) for r in v['frames']],sep='\n')
