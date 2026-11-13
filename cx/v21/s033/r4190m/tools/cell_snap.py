#!/usr/bin/env python3
import json, os, pathlib, re, subprocess, sys, time, urllib.request
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/r4190m')
kind=sys.argv[1]
payload=json.loads(sys.argv[2]) if len(sys.argv)>2 else {}
outroot=pathlib.Path(os.environ['OUTPUT_DIR'])

def cell_dir(p):
    run=int(re.search(r'-run(\d+)$',p['runId']).group(1))
    return outroot/f'run{run}'/p['cellId']

def conditions(dest):
    dest.mkdir(parents=True,exist_ok=True)
    lines=[subprocess.check_output(['date','-u','+%FT%TZ'],text=True).strip(),subprocess.check_output(['uptime'],text=True).strip(),subprocess.check_output(['free','-b'],text=True).splitlines()[1]]
    for cpu in range(16):
        try: v=pathlib.Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq').read_text().strip()
        except OSError: v='unavailable'
        lines.append(f'cpu{cpu}.scaling_cur_freq_khz={v}')
    (dest/('conditions-'+kind+'.txt')).write_text('\n'.join(lines)+'\n')

def find_logs(d):
    wanted={'source':str(d/'source.log'),'target':str(d/'target.log')}; candidates={role:[] for role in wanted}
    for proc in pathlib.Path('/proc').iterdir():
        if not proc.name.isdigit(): continue
        pid=int(proc.name)
        for role,needle in wanted.items():
            try:
                for fd in (proc/'fd').iterdir():
                    if os.path.realpath(fd)==needle:
                        cmd=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
                        exe=os.path.basename(os.readlink(proc/'exe'))
                        if exe.startswith('WithGrpcBench.'):
                            rank=0
                        elif exe in ('java','node') or exe.startswith('bench_'):
                            rank=1
                        elif exe=='dotnet':
                            rank=2
                        else:
                            continue
                        candidates[role].append((rank,pid,cmd,exe)); break
            except (OSError,PermissionError): pass
    return {role:(pid,cmd,exe) for role,items in candidates.items()
            if items for _,pid,cmd,exe in [min(items,key=lambda item:(item[0],item[1]))]}

def cpu_snapshot(d,label):
    found=find_logs(d); snap={}
    for role,(pid,cmd,exe) in found.items():
        tids={}
        try:
            for task in (pathlib.Path('/proc')/str(pid)/'task').iterdir():
                try:
                    raw=(task/'stat').read_text(); rest=raw[raw.rfind(')')+2:].split()
                    tids[task.name]={'comm':(task/'comm').read_text().strip(),'ticks':int(rest[11])+int(rest[12])}
                except (OSError,ValueError,IndexError): pass
        except OSError: pass
        snap[role]={'pid':pid,'cmd':cmd,'exe':exe,'threads':tids}
    (d/f'proc-{label}.json').write_text(json.dumps(snap,indent=2)+'\n')

def phase_state(url):
    try:
        with urllib.request.urlopen(url+'/bench/stats',timeout=2) as r: return json.load(r).get('phase')
    except Exception: return None

if kind=='before':
    deadline=time.monotonic()+600
    while True:
        load=float(pathlib.Path('/proc/loadavg').read_text().split()[0])
        if load<2.0: break
        if time.monotonic()>=deadline: raise SystemExit(f'1-minute load average stayed >=2.0 for 600 seconds (last {load})')
        time.sleep(2)
    d=cell_dir(payload); conditions(d); cpu_snapshot(d,'before')
elif kind=='active-before':
    cpu_snapshot(cell_dir(payload),'before')
elif kind=='after':
    d=cell_dir(payload)
    url=payload.get('sourceStatsUrl')
    if url:
        deadline=time.monotonic()+max(15,int(os.environ.get('DURATION_SECONDS','5'))+10)
        while time.monotonic()<deadline and phase_state(url)!='idle': time.sleep(.05)
    cpu_snapshot(d,'after'); conditions(d)
