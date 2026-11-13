import json,glob,os
root='/root/jobs/fx/cx/v21/s033/nprof'
for pat,role in [('send','target'),('serial','source')]:
 dirname=f'cpu-framework-{pat}'
 cell='send-saturation' if pat=='send' else 'request-serial'
 d=glob.glob(f'{root}/{dirname}/run1/zlink-framework-node-{cell}-4096/capture')[0]
 cp=json.load(open(f'{d}/{role}.cpuprofile'))
 a=json.load(open(d+'/after.json')); b=json.load(open(d+'/before.json'))
 msgs=(a['stats'][role].get('completed',a['stats'][role].get('received',0))-b['stats'][role].get('completed',b['stats'][role].get('received',0)))
 if role=='target':msgs=a['stats'][role].get('received',a['stats'][role].get('completed',0))-b['stats'][role].get('received',b['stats'][role].get('completed',0))
 nodes={n['id']:n for n in cp['nodes']}; selftime={i:0 for i in nodes}
 for i,delta in zip(cp['samples'],cp.get('timeDeltas',[])):selftime[i]+=delta
 def tot(i):return selftime[i]+sum(tot(c) for c in nodes[i].get('children',[]))
 rows=[]
 for i,n in nodes.items():
  f=n['callFrame']; url=f.get('url','')
  if '/packages/framework/dist/' not in url and '/packages/framework-codec-' not in url:continue
  label=f"{url}:{f.get('functionName') or '(anonymous)'}:{f.get('lineNumber',-1)+1}"
  rows.append((selftime[i]/msgs,tot(i)/msgs,label))
 rows.sort(reverse=True)
 print('\n',pat,role,'profile_samples',len(cp.get('samples',[])),'profile_ms',sum(cp.get('timeDeltas',[]))/1000,'msgs',msgs)
 for j,(s,t,label) in enumerate(rows[:20],1):print(j,round(s,3),round(t,3),label.replace('/root/worktree/zlink-ratio040/',''))
 print('GC node samples:',[(n['callFrame'].get('functionName'),sum(1 for i in cp['samples'] if i==n['id'])) for n in cp['nodes'] if 'garbage' in n['callFrame'].get('functionName','').lower() or 'gc'==n['callFrame'].get('functionName','').lower()][:8])
