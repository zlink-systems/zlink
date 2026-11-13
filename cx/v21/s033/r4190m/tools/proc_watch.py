#!/usr/bin/env python3
import json, os, pathlib, sys, time, subprocess, signal
root=pathlib.Path(sys.argv[1]); runner=int(sys.argv[2]); tracked={}

def usage():
    lines=[subprocess.check_output(['date','-u','+%FT%TZ'],text=True).strip(),subprocess.check_output(['uptime'],text=True).strip(),subprocess.check_output(['free','-b'],text=True).splitlines()[1]]
    for cpu in range(16):
        try:v=pathlib.Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq').read_text().strip()
        except OSError:v='unavailable'
        lines.append(f'cpu{cpu}.scaling_cur_freq_khz={v}')
    return '\n'.join(lines)+'\n'

def find(log):
    for proc in pathlib.Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        pid=int(proc.name)
        try:
            for fd in (proc/'fd').iterdir():
                if os.path.realpath(fd)==str(log):
                    exe=os.path.basename(os.readlink(proc/'exe'))
                    if exe.startswith('bench_'):return pid
        except (OSError,PermissionError):pass
    return None

def snap(pid):
    out={}
    try: tasks=(pathlib.Path('/proc')/str(pid)/'task').iterdir()
    except OSError:return out
    for task in tasks:
        try:
            stat=(task/'stat').read_text(); q=stat[stat.rfind(')')+2:].split()
            out[task.name]={'comm':(task/'comm').read_text().strip(),'ticks':int(q[11])+int(q[12])}
        except (OSError,IndexError,ValueError):pass
    return out

def alive(pid):return pathlib.Path('/proc',str(pid)).exists()

while pathlib.Path('/proc',str(runner)).exists():
    for cell in root.glob('run*/*'):
        key=str(cell)
        if not cell.is_dir():continue
        item=tracked.setdefault(key,{'roles':{},'started':False})
        for role in ('target','source'):
            pid=find(cell/(role+'.log'))
            if pid is not None:
                if role=='source' and role not in item['roles']:
                    try: os.kill(pid,signal.SIGSTOP)
                    except ProcessLookupError: continue
                    deadline=time.monotonic()+600
                    while float(pathlib.Path('/proc/loadavg').read_text().split()[0])>=2.0 and time.monotonic()<deadline:time.sleep(2)
                    load=float(pathlib.Path('/proc/loadavg').read_text().split()[0])
                    if load>=2.0: raise SystemExit(f'1-minute load average stayed >=2.0 for 600 seconds (last {load})')
                    if not item['started']:
                        (cell/'conditions-before.txt').write_text(usage());item['started']=True
                    try: os.kill(pid,signal.SIGCONT)
                    except ProcessLookupError: pass
                elif not item['started']:
                    # The C runner starts the idle target before its source client.
                    pass
                roledata=item['roles'].setdefault(role,{'pid':pid,'before':snap(pid),'after':{}})
                roledata['after']=snap(pid)
        done=(cell/'results.json').exists() and item['started'] and all(not alive(v['pid']) for v in item['roles'].values())
        if done and not item.get('written'):
            (cell/'proc-before.json').write_text(json.dumps({r:{'pid':v['pid'],'threads':v['before']} for r,v in item['roles'].items()},indent=2)+'\n')
            (cell/'proc-after.json').write_text(json.dumps({r:{'pid':v['pid'],'threads':v['after']} for r,v in item['roles'].items()},indent=2)+'\n')
            (cell/'conditions-after.txt').write_text(usage());item['written']=True
    time.sleep(.02)
# Flush cells ending at runner shutdown.
for key,item in tracked.items():
    if item.get('written'):continue
    cell=pathlib.Path(key)
    if item['roles']:
        (cell/'proc-before.json').write_text(json.dumps({r:{'pid':v['pid'],'threads':v['before']} for r,v in item['roles'].items()},indent=2)+'\n')
        (cell/'proc-after.json').write_text(json.dumps({r:{'pid':v['pid'],'threads':v['after']} for r,v in item['roles'].items()},indent=2)+'\n')
        (cell/'conditions-after.txt').write_text(usage())
