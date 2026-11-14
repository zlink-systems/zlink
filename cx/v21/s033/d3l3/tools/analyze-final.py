import pathlib,subprocess,json
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/d3l3');dll=j/'tools/analyzer/bin/Release/net8.0/analyzer.dll'
for name in ['sched-L2-send1K','sched-L3-send1K-v2','sched-L2-bp4K','sched-L3-bp4K','exp-no-clone-send1K','exp-no-clone-bp4K']:
 d=j/'raw'/name
 for role in ['source','target']:
  with open(d/(role+'-summary.json'),'w') as f,open(d/(role+'-analyzer.log'),'w') as log:
   r=subprocess.run(['taskset','-c','6-7,14-15','dotnet',str(dll),str(d/(role+'.nettrace')),str(d/'window.json')],stdout=f,stderr=log)
  print(name,role,r.returncode,flush=True)
