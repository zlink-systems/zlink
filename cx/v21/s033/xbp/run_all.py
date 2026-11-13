import pathlib,subprocess,time,itertools,json,os
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/xbp')
verdict=pathlib.Path('/root/jobs/fx/cx/v21/s033/core124/verdict.md')
while not (J/'raw/builds-done.json').exists() or not (J/'counter.so').exists() or not verdict.exists():
    time.sleep(10)
(J/'raw/gate-verdict-copy.md').write_bytes(verdict.read_bytes())
for run in range(1,4):
    for payload in (1024,4096):
        for lang in ('cpp','dotnet','java'):
            for kind in ('framework','binding'):
                for mode in (('plain','count') if run%2 else ('count','plain')):
                    if not verdict.exists():raise RuntimeError('release verdict absent')
                    cell=f'{lang}-{kind}-{payload}-{mode}-r{run}'
                    print(time.strftime('%H:%M:%S'),cell,flush=True)
                    args=['flock','-o','/root/jobs/fx/cx/measure.lock','taskset','-c','16-23','python3',str(J/'run_cell.py'),lang,kind,str(payload),mode,str(run)]
                    with open(J/'raw'/f'{cell}-driver.log','w') as log:
                        log.write(json.dumps(args)+'\n');log.flush()
                        p=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT)
                    if p.returncode:raise SystemExit(f'{cell} failed {p.returncode}')
(J/'raw/measurements-done.json').write_text(json.dumps({'end':time.time()}))
print('all measurements done',flush=True)
