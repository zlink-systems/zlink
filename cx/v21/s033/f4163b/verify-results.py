import pathlib,json,hashlib,subprocess,csv,statistics as st
j=pathlib.Path(__file__).parent
baseline=json.loads((j/'logs/baseline.json').read_text())
checks={}
checks['old_branch_unchanged']=subprocess.check_output(['git','rev-parse',baseline['branch']],text=True).strip()==baseline['revision']
checks['old_recipes_unchanged']=all(hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()==sha for p,sha in baseline['old_recipes'].items())
checks['C_unchanged']=hashlib.sha256(pathlib.Path(baseline['C_runtime']).read_bytes()).hexdigest()==baseline['C_sha256']
checks['C_probe_unchanged']=hashlib.sha256((pathlib.Path(baseline['C_probe_prefix'])/'lib/libzlink.so.1.23.0').read_bytes()).hexdigest()==baseline['C_probe_sha256']
rows=list(csv.DictReader((j/'runs-10.csv').open())); probes=list(csv.DictReader((j/'probe-summary.csv').open()))
checks['runs_60']=len(rows)==60 and all(len([x for x in rows if x['kind']==k and x['size']==s])==10 for k in ['C','R','L'] for s in ['256','1024'])
checks['probes_15']=len(probes)==15 and all(len([x for x in probes if x['series']=='probe-'+k])==5 for k in ['C','R','L'])
checks['probe_values_present']=all(float(x['mean_ms'])>0 and float(x['qmean'])>=0 and int(x['records'])>0 for x in probes)
checks['unique_runs']=len({(x['kind'],x['run'],x['size']) for x in rows})==60
checks['six_stages_complete']=sum('END ' in x for x in (j/'logs/measure-stage.log').read_text().splitlines())==6
preflights=[]
for filename in ['build-preflight.jsonl','measure-preflight.jsonl']:
 preflights += [json.loads(l) for l in (j/filename).read_text().splitlines()]
checks['resource_preflights']=all(x['allowed'] and x['MemAvailable_bytes']>=12*1024**3 and x['job_bytes']<25*1024**3 for x in preflights)
r={'checks':checks,'resource':{'count':len(preflights),'min_MemAvailable_GiB':min(x['MemAvailable_bytes'] for x in preflights)/1024**3,'max_job_GiB':max(x['job_bytes'] for x in preflights)/1024**3}}
(j/'logs/final-integrity.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
assert all(checks.values())
