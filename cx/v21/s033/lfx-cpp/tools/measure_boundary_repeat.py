#!/usr/bin/env python3
import datetime, json, os, pathlib, subprocess, sys, time
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/lfx-cpp')
REPO=pathlib.Path('/root/worktree/zlink-v21-s033/cfx')
RAW=JOB/'raw'
CELLS=[('request-serial',1024)]
HOLD=RAW/'holds'/datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
HOLD.mkdir(parents=True,exist_ok=False)
def utc(): return datetime.datetime.now(datetime.timezone.utc).isoformat()
def log(s):
    with (HOLD/'hold.log').open('a') as f: f.write(f'{utc()} {s}\n')
    print(s,flush=True)
def cpus_setgov(value):
    for cpu in range(16): pathlib.Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor').write_text(value)
def check_guard():
    for _ in range(240):
        ready=True
        for cpu in range(16):
            for state in (2,3):
                p=pathlib.Path(f'/sys/devices/system/cpu/cpu{cpu}/cpuidle/state{state}/disable')
                if p.read_text().strip()!='1': ready=False
        if ready: return
        time.sleep(.25)
    raise RuntimeError('quiet guard did not disable C2/C3 within 60 seconds')
def validate(result,sched):
    c=json.loads(result.read_text())['cells'][0]; s=sched.read_text()
    assert not c.get('contaminated') and not c.get('drain_bound_hit') and not c.get('settle_bound_hit'), 'contaminated/drain bound'
    assert c.get('errors',0)==c.get('server_errors',0)==0 and c['target_stats']['errors']==0, 'errors'
    assert c.get('abandoned',0)==0 and c.get('header_validation_failures',0)==0, 'abandoned/header validation'
    assert c['trigger']['durationMs']==5000 and c['trigger']['warmup']==1000, 'wrong active/warmup timing'
    assert c['submitted']==c['completed']==c['target_stats']['received'], 'submitted/completed/received mismatch'
    assert all(f'cpu{cpu}.state{state}.disable_at_run_start=1' in s for cpu in (0,8) for state in (2,3)), 'C-state evidence missing'
    assert 'scheduling_invalid=' not in s, 'scheduling_invalid'
    for phase,expect in [('source_start','0-5'),('target_start','8-13'),('mid_active_source','0-5'),('mid_active_target','8-13')]:
        tids=[x for x in s.splitlines() if x.startswith(phase+'_tid=')]
        aff=[x for x in s.splitlines() if x.startswith(phase+'_thread_affinity ')]
        assert tids and len(tids)==len(aff), f'{phase} thread evidence missing'
        assert all(x.endswith('cpus='+expect) for x in aff), f'{phase} affinity mismatch'
    pol=[x for x in s.splitlines() if 'current scheduling policy:' in x]
    assert pol and all(x.endswith('SCHED_OTHER') for x in pol), 'non-SCHED_OTHER thread'
    return c

def run(level,pattern,size,rep):
    base=HOLD/f'{pattern}-{size}'/f'L{level}-repeat{rep}'
    attempt=1
    while attempt<=2:
        out=base/f'attempt{attempt}'; out.mkdir(parents=True,exist_ok=False)
        env=os.environ.copy(); env.update(IMPL='zlink-framework-cpp',ZLINK_CPP_LADDER_LEVEL=str(level))
        stamp=f'lfx-{pattern}-{size}-L{level}-repeat{rep}-{attempt}-{int(time.time())}'
        log(f'START L{level} {pattern}-{size} attempt={attempt} raw={out}')
        rc=subprocess.run(['bash',str(JOB/'tools/run-one-binding.sh'),pattern,str(size),str(out),stamp,'1'],env=env,timeout=120).returncode
        result=out/'run1'/f'zlink-framework-cpp-{pattern}-{size}'/'results.json'
        sched=result.with_name('scheduling.txt')
        try:
            assert rc==0, f'runner rc={rc}'
            assert result.is_file() and sched.is_file(), 'missing result or scheduling file'
            c=validate(result,sched)
            rec={'level':level,'repeat':rep,'pattern':pattern,'size':size,'rate':c['throughput_per_second'],'result':str(result),'scheduling':str(sched),'valid':True,'attempt':attempt,'utc':utc()}
            (out/'validation.json').write_text(json.dumps(rec,indent=2)+'\n')
            with (RAW/'measurements.jsonl').open('a') as f: f.write(json.dumps(rec)+'\n')
            log(f'VALID L{level} {pattern}-{size} msg/s={rec["rate"]} result={result} scheduling={sched}')
            return
        except Exception as e:
            (out/'validation.json').write_text(json.dumps({'level':level,'repeat':rep,'pattern':pattern,'size':size,'valid':False,'rc':rc,'reason':str(e),'utc':utc()},indent=2)+'\n')
            log(f'INVALID L{level} {pattern}-{size} attempt={attempt}: {e}; raw={out}')
            if attempt==2: raise
            attempt+=1
        finally:
            time.sleep(3)
    raise RuntimeError('retry exhausted')

rc=0; started=utc(); (HOLD/'window-start-utc.txt').write_text(started+'\n')
try:
    check_guard(); cpus_setgov('performance')
    (HOLD/'git-meta.txt').write_text(f'HEAD={subprocess.check_output(["git","-C",str(REPO),"rev-parse","HEAD"],text=True).strip()}\nstatus={subprocess.check_output(["git","-C",str(REPO),"status","--short"],text=True)}')
    for pattern,size in CELLS:
        for rep in (2,3):
            run(1,pattern,size,rep)
            run(2,pattern,size,rep)
except BaseException:
    rc=1
    raise
finally:
    cpus_setgov('powersave')
    (HOLD/'window-end-utc.txt').write_text(utc()+'\n')
    (HOLD/'runner-rc.txt').write_text(str(rc)+'\n')
    log('HOLD_END')
