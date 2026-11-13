from pathlib import Path
import subprocess, os, time, json, shutil, signal
out=Path(__file__).parent
env=os.environ.copy()
env.update(EnableSourceControlManagerQueries='false',EnableSourceLink='false')
def run(label,cmd,monitor=False):
    available=int(next(s.split()[1] for s in Path('/proc/meminfo').read_text().splitlines() if s.startswith('MemAvailable:')))
    assert available>=12*1024*1024
    path=out/'raw'/f'{label}.log'
    with path.open('w') as log:
        process=subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,env=env)
        last_size=0; last_progress=time.monotonic(); captured=False
        while process.poll() is None:
            size=path.stat().st_size
            if size!=last_size:
                last_progress=time.monotonic(); last_size=size
            if monitor and not captured and time.monotonic()-last_progress>60:
                # Restrict discovery to this test invocation's descendants.
                descendants={process.pid}
                changed=True
                while changed:
                    changed=False
                    for p in Path('/proc').glob('[0-9]*'):
                        try:
                            ppid=int(next(s.split()[1] for s in (p/'status').read_text().splitlines() if s.startswith('PPid:')))
                            if ppid in descendants and int(p.name) not in descendants:
                                descendants.add(int(p.name)); changed=True
                        except (FileNotFoundError,ProcessLookupError,PermissionError,StopIteration): pass
                for pid in descendants:
                    try: args=(Path('/proc')/str(pid)/'cmdline').read_bytes()
                    except (FileNotFoundError,ProcessLookupError): continue
                    if b'testhost.dll' not in args: continue
                    (out/'raw'/f'{label}-testhost.pid').write_text(str(pid))
                    with (out/'raw'/f'{label}-native-stacks.log').open('w') as stack_log:
                        gdb=subprocess.Popen(['gdb','--batch','-p',str(pid),'-ex','set pagination off','-ex','thread apply all bt','-ex','detach'],stdout=stack_log,stderr=subprocess.STDOUT)
                        try: gdb.wait(timeout=20)
                        except subprocess.TimeoutExpired:
                            gdb.send_signal(signal.SIGTERM); gdb.wait()
                captured=True
                print(f'{label}: captured native stacks after stalled progress',flush=True)
            time.sleep(1)
        code=process.wait()
    (out/'raw'/f'{label}.status.json').write_text(json.dumps({'exit_code':code,'command':cmd,'mem_available_kb_start':available}))
    print(f'{label}: exit={code}',flush=True)
    return code

import sys
raise SystemExit(run(sys.argv[1],sys.argv[2:],monitor=True))
