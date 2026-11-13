import pathlib,json,sys
J=pathlib.Path(__file__).parent
names=sys.argv[1:] or ['baseline','baseline-r3-clean','cpu','perf','heap','diag','same-rate','matched-rate-baseline','prototype','prototype-cpu','prototype-diag']
summary={}
for name in names:
 files=sorted((J/name).glob('run*/*/results.json'));assert files,name
 mem=[]
 for p in files:
  d=json.loads(p.read_text())['cells'][0];t=d['target_stats'];cmd=json.loads((p.parent/'commands.json').read_text())
  assert d['errors']==d['abandoned']==d['server_errors']==t['errors']==0,(name,p)
  if 'framework' in d['implementation'] and (name=='matched-rate-baseline' or name.startswith('prototype')):
   manifest=json.loads((J/'variant-manifest.json').read_text());expected=manifest['baseline_js_sha256' if name=='matched-rate-baseline' else 'candidate_js_sha256'];assert cmd['raw_port_sha256']==expected,(name,p,'wrong runtime variant')
  assert d['submitted']==d['completed']==t['received']+(t.get('rejected') or 0),(name,p)
  assert (t.get('rejected') or 0)==0,(name,p)
  assert cmd['affinity']=={'source':list(range(8)),'target':list(range(8,16))},(name,p,cmd['affinity'])
  assert all(any(x.endswith('/zlink.node') for x in cmd['loaded_native'][role]) and any('/libzlink.so' in x for x in cmd['loaded_native'][role]) for role in ['source','target']),(name,p)
  for q in [p.parent/'before.json',p.parent/'after.json',p.parent/'capture/meta.json']:
   if q.exists():
    v=json.loads(q.read_text());m=v.get('memavailable_kb');
    if m is not None:assert m>=12582912,(name,p,m);mem.append(m)
 guard=J/name/'guard-summary.json';g=json.loads(guard.read_text()) if guard.exists() else None
 if g:assert g['detected'] is None and g['minimum_memavailable_kb']>=12582912,(name,g)
 summary[name]={'cases':len(files),'zero_errors_abandoned_rejected':True,'source_target_counts_match':True,'affinity_valid':True,'minimum_observed_mem_kib':min(mem) if mem else None,'guard':g}
print(json.dumps(summary,indent=2));(J/'validation-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
