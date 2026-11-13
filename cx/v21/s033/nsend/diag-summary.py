import pathlib,json,sys
J=pathlib.Path(__file__).parent;name=sys.argv[1] if len(sys.argv)>1 else 'diag';out={}
for impl in ['zlink-framework-node','zlink-node']:
 out[impl]={}
 for role in ['source','target']:
  d=json.loads((J/name/'run1'/f'{impl}-send-saturation-4096/capture/{role}.diag.json').read_text());c=d['after']['counts'];n=c.get('bench.payload.messages',c.get('setImmediate.scheduled')) if role=='source' else c['Router.recv.messages'];v={k:x/n for k,x in c.items()};out[impl][role]={'messages':n,'per_message':v};print(impl,role,'messages',n);print({k:round(x,4) for k,x in v.items()})
(J/(name+'-normalized.json')).write_text(json.dumps(out,indent=2)+'\n')
