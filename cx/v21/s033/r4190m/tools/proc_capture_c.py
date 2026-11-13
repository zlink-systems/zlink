#!/usr/bin/env python3
import json,os,pathlib,subprocess,sys,time,signal
root=pathlib.Path(sys.argv[1]); runner=int(sys.argv[2]); tracked={}; clk=os.sysconf('SC_CLK_TCK')
def cond():
    lines=[subprocess.check_output(['date','-u','+%FT%TZ'],text=True).strip(),subprocess.check_output(['uptime'],text=True).strip(),subprocess.check_output(['free','-b'],text=True).splitlines()[1]]
    for c in range(16):
        p=pathlib.Path(f'/sys/devices/system/cpu/cpu{c}/cpufreq/scaling_cur_freq')
        try:v=p.read_text().strip()
        except OSError:v='unavailable'
        lines.append(f'cpu{c}.scaling_cur_freq_khz={v}')
    return '\n'.join(lines)+'\n'
def snap(pid):
    out={}
    try: tasks=list((pathlib.Path('/proc')/str(pid)/'task').iterdir())
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
        if not cell.is_dir():continue
        item=tracked.setdefault(str(cell),{'roles':{},'written':False,'started':False})
        for role,pidfile in [('target','target.pid'),('source','source-wrapper.pid')]:
            try: pid=int((cell/pidfile).read_text().strip())
            except (OSError,ValueError):continue
            if role=='source' and not item['started']:
                try: os.kill(pid,signal.SIGSTOP)
                except ProcessLookupError: continue
                deadline=time.monotonic()+600
                while float(pathlib.Path('/proc/loadavg').read_text().split()[0])>=2.0 and time.monotonic()<deadline: time.sleep(1)
                if float(pathlib.Path('/proc/loadavg').read_text().split()[0])>=2.0: raise SystemExit('1-minute load average stayed >=2.0 for 600 seconds')
                (cell/'conditions-before.txt').write_text(cond())
                item['started']=True
                before=snap(pid)
                try: os.kill(pid,signal.SIGCONT)
                except ProcessLookupError: pass
            else: before=snap(pid)
            rec=item['roles'].setdefault(role,{'pid':pid,'before':before,'after':{}})
            current=snap(pid)
            for tid,data in current.items():
                old=rec['after'].get(tid)
                if old is None or data['ticks']>old['ticks']: rec['after'][tid]=data
        if (cell/'results.json').exists() and item['roles'] and all(not alive(r['pid']) for r in item['roles'].values()) and not item['written']:
            (cell/'proc-before.json').write_text(json.dumps({k:{'pid':v['pid'],'threads':v['before']} for k,v in item['roles'].items()},indent=2)+'\n')
            (cell/'proc-after.json').write_text(json.dumps({k:{'pid':v['pid'],'threads':v['after']} for k,v in item['roles'].items()},indent=2)+'\n')
            (cell/'conditions-after.txt').write_text(cond());item['written']=True
    time.sleep(.005)
for key,item in tracked.items():
    if item['written']:continue
    cell=pathlib.Path(key)
    if item['roles']:
        (cell/'proc-before.json').write_text(json.dumps({k:{'pid':v['pid'],'threads':v['before']} for k,v in item['roles'].items()},indent=2)+'\n')
        (cell/'proc-after.json').write_text(json.dumps({k:{'pid':v['pid'],'threads':v['after']} for k,v in item['roles'].items()},indent=2)+'\n')
        (cell/'conditions-after.txt').write_text(cond())
