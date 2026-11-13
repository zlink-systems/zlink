#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys, time

out = pathlib.Path(sys.argv[1])
command = sys.argv[2:]
proc = subprocess.Popen(command, cwd="/root/worktree/zlink-jsat/framework/bench/grpc",
                        stdout=(out / "runner.log").open("wb"), stderr=subprocess.STDOUT,
                        start_new_session=True)
root = proc.pid
snapshots = []

def descendants(rootpid):
    ppid = {}
    for entry in pathlib.Path('/proc').iterdir():
        if not entry.name.isdigit(): continue
        try:
            s = (entry / 'stat').read_text()
            r = s.rfind(')')
            ppid[int(entry.name)] = int(s[r+2:].split()[1])
        except (OSError, ValueError, IndexError): pass
    found = {rootpid}
    changed = True
    while changed:
        changed = False
        for pid, parent in ppid.items():
            if parent in found and pid not in found:
                found.add(pid); changed = True
    return found

def read_proc(pid):
    try:
        cmd = (pathlib.Path('/proc')/str(pid)/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
        if 'systems.zlink.bench.withgrpc' not in cmd: return None
        st = (pathlib.Path('/proc')/str(pid)/'status').read_text()
        allowed = next((x.split(':',1)[1].strip() for x in st.splitlines() if x.startswith('Cpus_allowed_list:')), '')
        group = 'source' if allowed.startswith('0-7') else 'target' if allowed.startswith('8-15') else allowed
        role = 'Framework/source JVM' if 'zlink-framework-java' in os.environ.get('CELL_IMPL','') else 'binding/source JVM'
        if 'rawserver' in cmd or 'frameworkserver' in cmd or 'grpcserver' in cmd: role = 'target JVM'
        threads = {}
        for task in (pathlib.Path('/proc')/str(pid)/'task').iterdir():
            tid = int(task.name)
            try:
                fields = (task/'schedstat').read_text().split()
                status = (task/'status').read_text().splitlines()
                sw = int(next(x.split()[-1] for x in status if x.startswith('voluntary_ctxt_switches:')))
                si = int(next(x.split()[-1] for x in status if x.startswith('nonvoluntary_ctxt_switches:')))
                name = (task/'comm').read_text().strip()
                threads[tid] = (name, int(fields[0]), int(fields[1]), sw, si)
            except (OSError, ValueError, StopIteration, IndexError): pass
        return pid, group, role, cmd, threads
    except OSError: return None

while proc.poll() is None:
    now = time.monotonic_ns()
    rows=[]
    for pid in descendants(root):
        row=read_proc(pid)
        if row: rows.append(row)
    snapshots.append((now, rows))
    # Attach a short JFR recording after both data processes exist. Recording includes
    # active-phase samples; instrumentation is only on the Framework run when requested.
    jvms=[r for r in rows if r[1] in ('source','target')]
    if os.environ.get('CAPTURE_JFR') == '1' and not (out/'jfr-started').exists() and len(jvms)>=2:
        for pid, group, role, cmd, threads in jvms:
            label=group
            subprocess.run(['jcmd',str(pid),'JFR.start',f'name=jsat_{label}',
                            'settings=profile','duration=50s',f'filename={out}/{label}.jfr'],
                           stdout=(out/f'jfr-start-{label}.log').open('wb'),stderr=subprocess.STDOUT)
        (out/'jfr-started').touch()
    time.sleep(0.05)
rc=proc.wait()
(out/'exit-code').write_text(str(rc)+'\n')
# Keep compact counter snapshots for active-window interpolation.
(out/'samples.json').write_text(json.dumps([
    {'t':t,'procs':[{'pid':p,'group':g,'role':r,'threads':{str(k):v for k,v in ts.items()}} for p,g,r,c,ts in rows]}
    for t,rows in snapshots],separators=(',',':')))
sys.exit(rc)
