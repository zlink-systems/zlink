import datetime,json,pathlib,subprocess
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/lfx-cpp'); T=J/'tools'
def hold(p): subprocess.run([str(T/'run-group.sh'),str(p)],check=True)
def summarize(name,*args):
    with (J/'raw'/f'aggregate-{name}.json').open('x') as f:
        subprocess.run(['python3',str(T/'summarize.py'),*map(str,args)],stdout=f,check=True)
def report(): subprocess.run(['python3',str(T/'write-report-s1.py')],check=True)
try:
    hold(T/'plan-binding.json'); summarize('binding','--binding')
    for pair in ['L0-L1','L2-L3','L4-L7']:
        hold(T/f'plan-{pair}.json')
        for turn in range(1,20):
            path=T/f'repeat-{pair}-round{turn}.json'
            summarize(f'{pair}-round{turn}','--repeat-plan',path)
            pending=json.loads(path.read_text())
            if not pending: break
            order=['0','1','2','3','4','7']; levels=[l for l in order if any(c[0]==l for c in pending)]
            for chunk in range(0,len(levels),2):
                selected=levels[chunk:chunk+2]; part=T/f'repeat-{pair}-round{turn}-hold{chunk//2+1}.json'
                part.write_text(json.dumps([c for c in pending if c[0] in selected],indent=2)+'\n')
                hold(part)
        else: raise RuntimeError('repeat planning did not converge')
        report()
    (J/'raw/measurement-success.json').write_text(json.dumps({'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'complete':True})+'\n')
except BaseException:
    subprocess.run(['python3',str(T/'summarize.py')],stdout=(J/'raw/aggregate-interrupted.json').open('w'))
    if (J/'raw/runs.jsonl').exists(): report()
    raise
