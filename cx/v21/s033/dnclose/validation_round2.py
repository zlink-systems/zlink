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
status=out/'raw'/'after-root-focused.status.json'
while not status.exists(): time.sleep(1)
files=subprocess.check_output(['git','diff','--name-only','--','bindings/dotnet'],text=True).splitlines()
files+=['bindings/dotnet/tests/Zlink.Tests/'+f for f in ['test_closed_submit_ownership.cs','test_socket_handle_reservation.cs','test_submit_close_reservation.cs']]
if run('format-apply-round2',['dotnet','format','whitespace','bindings/dotnet/Zlink.sln','--no-restore','--include',*files]): raise SystemExit(1)
if run('format-verify-round2',['dotnet','format','whitespace','bindings/dotnet/Zlink.sln','--verify-no-changes','--no-restore','--include',*files]): raise SystemExit(1)
if run('build-perf-round2',['dotnet','build','bindings/dotnet/perf/single/Zlink.BindingBench/Zlink.BindingBench.csproj','-c','Release','-m:1','-p:EnableSourceControlManagerQueries=false','-p:EnableSourceLink=false']): raise SystemExit(1)
shutil.copytree('bindings/dotnet/perf/single/Zlink.BindingBench/bin/Release/net8.0',out/'perf-after-final-02',dirs_exist_ok=True)
code=run('full-suite-02',['bash','bindings/dotnet/tests/run_tests.sh','--logger','console;verbosity=normal','--logger','trx;LogFileName=full-suite-02.trx','--results-directory',str(out/'raw'/'full-suite-02-results'),'--diag',str(out/'raw'/'full-suite-02-diag.log')],monitor=True)
if code:
    run('samples-separate-02',['bash','bindings/dotnet/samples/run_samples.sh'],monitor=True)
run('diff-check-round2',['git','diff','--check'])
