"""Run the original aggregator over strictly validated raw cells; never alter raw."""
import datetime,json,pathlib,subprocess,sys
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868');R=J/'raw'
REPO=pathlib.Path('/root/worktree/zlink-v21-s033/l3868')
AGG=pathlib.Path('/root/worktree/zlink-v21-s033/agg-main/framework/bench/grpc/tools/bench_aggregate.py')
CHECK=REPO/'framework/bench/grpc/cpp/ladder/check_result.py'
history=R/('derived-history-'+datetime.datetime.now(datetime.timezone.utc).strftime('%H%M%S'))
if (R/'partial-summary.json').exists():
    import shutil
    history.mkdir()
    shutil.copy2(R/'partial-summary.json',history/'partial-summary.json')
    for old in (R/'accepted-owner').glob('*'):
        if old.is_file():shutil.copy2(old,history/old.name)
success=set()
for log in [R/'l0-corrected-driver.log',R/'l0-owner-driver.log']:
    if log.exists():
        for line in log.read_text().splitlines():
            try:
                value=json.loads(line)
                if isinstance(value,dict) and 'raw' in value:success.add(str(pathlib.Path(value['raw']).resolve()))
            except (ValueError,TypeError):pass
manifest=json.loads((R/'build-provenance.json').read_text())
output={'commit':manifest['commit'],'matrix':{},'cross':{},'excluded':[]}
work=R/'accepted-owner';work.mkdir(exist_ok=True)
main=list((R/'L0-owner').glob('*/run*/*/results.json'))
groups={'main':main,
 'raw-raw':[p for p in main if p.parent.name=='zlink-cpp-send-saturation-1024'],
 'framework-framework':[p for p in main if p.parent.name=='zlink-framework-cpp-send-saturation-1024'],
 'framework-raw':list((R/'cross/L0-owner/framework-raw').glob('run*/*/results.json'))}
for group,paths in groups.items():
    accepted=[]
    for p in paths:
        try:
            assert str(p.resolve()) in success,'runner validation not completed'
            x=json.loads(p.read_text());c=x['cells'][0]
            meta=x['metadata'];prov=json.loads((p.parent/'binary-provenance.json').read_text())
            assert prov['commit']==manifest['commit'] and prov['binaries']==manifest['binaries'],'binary provenance mismatch'
            assert meta['sourceCpus']=='0-7' and meta['targetCpus']=='8-15','CPU mismatch'
            check=subprocess.run([sys.executable,str(CHECK),str(p)],capture_output=True,text=True)
            assert check.returncode==0,check.stderr
            run=p.parent.parent.name;dest=work/group/run/p.parent.name
            dest.parent.mkdir(parents=True,exist_ok=True)
            if dest.exists():assert dest.resolve()==p.parent.resolve()
            else:dest.symlink_to(p.parent.resolve(),target_is_directory=True)
            accepted.append((p,c))
        except Exception as error:
            output['excluded'].append({'raw':str(p),'reason':str(error)})
    if not accepted:continue
    out=work/(group+'.json')
    with (work/(group+'.md')).open('w') as log:
        subprocess.run([sys.executable,str(AGG),'--lang','cpp','--min-runs','3','--baseline','zlink-cpp','--runs-glob',str(work/group/'run*'),'--json-out',str(out)],stdout=log,check=True)
    rows=json.loads(out.read_text())['rows']
    for row in rows:
        key=f"{row['pattern']}-{row['payload_size']}";impl=row['implementation']
        entries=[(p,c) for p,c in accepted if (c['pattern'],c['payload_size'],c['implementation'])==(row['pattern'],row['payload_size'],impl)]
        stat={'runs':len(entries),'raw':[str(p) for p,c in entries],'g5':row['g5_status']}
        if len(entries)==3:
            value=row['median']['throughput_per_second']
            stat.update(median_msg_s=value,median_raw=str(next(p for p,c in entries if c['throughput_per_second']==value)))
        if group=='main':output['matrix'].setdefault(key,{})[impl]=stat
        else:output['cross'][group]=stat
(R/'partial-summary.json').write_text(json.dumps(output,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({'accepted_matrix_cells':len(output['matrix']),'cross_groups':list(output['cross']),'excluded':output['excluded']},ensure_ascii=False))
