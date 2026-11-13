import pathlib,json,statistics
p=pathlib.Path('/root/jobs/fx/cx/v21/s033/sfix')
patterns=('request-backpressure','request-serial','send-saturation')
rows=[]
for pattern in patterns:
 groups={'before':[],'after':[]}
 for round in range(1,5):
  vals={}
  for variant in groups:
   file=p/'ab'/str(round)/pattern/variant/'run1'/f'zlink-framework-cpp-{pattern}-1024'/'results.json'
   c=json.loads(file.read_text())['cells'][0]
   assert c['implementation']=='zlink-framework-cpp' and c['payload_size']==1024,file
   assert c['trigger']['durationMs']==5000 and c['trigger']['warmup']==30000,file
   for key in ('errors','abandoned','header_validation_failures','server_rejected_count','contaminated','drain_bound_hit'):
    assert not c[key],(file,key,c[key])
   assert c['submitted']==c['completed']==c['target_stats']['received'],file
   assert not c['target_stats']['errors'] and not c['target_stats']['rejected'],file
   v=c['throughput_per_second'];groups[variant].append(v);vals[variant]=v
  rows.append(f'| {pattern} | {round} | {vals["before"]:,.2f} | {vals["after"]:,.2f} | {(vals["after"]/vals["before"]-1)*100:+.2f}% |')
 b,a=(statistics.median(groups[x]) for x in ('before','after'))
 rows.append(f'| {pattern} | **Median** | **{b:,.2f}** | **{a:,.2f}** | **{(a/b-1)*100:+.2f}%** |')
(p/'ab-table.md').write_text('| Pattern | Round | Main msg/s | Stacked fixes msg/s | Change |\n|---|---:|---:|---:|---:|\n'+'\n'.join(rows)+'\n')
print((p/'ab-table.md').read_text())
