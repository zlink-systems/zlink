import json,pathlib,statistics,subprocess
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/cstage');runner=j/'tools/measure.py'
subprocess.run(['python3',str(runner),'--runs','5','--warmup','30','--active','5','--tag','caller'],check=True)
rates={}
for v in ['main','staged']:
 rates[v]=statistics.median(json.loads(p.read_text())['cells'][0]['throughput_per_second'] for p in (j/'runs/caller'/v).glob('run*/results.json'))
rate=max(1000,int(min(rates.values())*.75/1000)*1000)
(j/'common-rate.json').write_text(json.dumps({'unpaced_median_rates':rates,'common_rate':rate,'policy':'75% of the smaller observed median, rounded down to 1000 req/s'},indent=2))
print('common_rate='+str(rate),flush=True)
subprocess.run(['python3',str(runner),'--runs','1','--warmup','30','--active','60','--tag','cpu','--rate',str(rate),'--cpu'],check=True)
subprocess.run(['python3',str(j/'tools/summarize.py')],check=True)
x=json.loads((j/'summary.json').read_text());assert x['same_rate']['relative_rate_difference']<.02,x['same_rate']
