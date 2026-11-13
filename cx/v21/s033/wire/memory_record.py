import json,pathlib,time
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/wire');minimum=json.loads((job/'memory-min.json').read_text())['minimum_kb'];samples=0
while not (job/'memory-record-stop').exists():
 available=int(next(x.split()[1] for x in pathlib.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
 minimum=min(minimum,available);samples+=1
 (job/'memory-total-min.json').write_text(json.dumps({'minimum_kb':minimum,'last_kb':available,'samples_after_overlap':samples}))
 time.sleep(1)
