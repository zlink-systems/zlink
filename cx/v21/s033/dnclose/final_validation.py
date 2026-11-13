from pathlib import Path
import os, subprocess, shutil, json, time
out=Path(__file__).parent
env=os.environ.copy(); env.update(EnableSourceControlManagerQueries='false', EnableSourceLink='false')
def run(label,cmd):
    assert int(next(s.split()[1] for s in Path('/proc/meminfo').read_text().splitlines() if s.startswith('MemAvailable:')))>=12*1024*1024
    with (out/'raw'/f'{label}.log').open('w') as log:
        r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,env=env)
    (out/'raw'/f'{label}.status.json').write_text(json.dumps({'exit_code':r.returncode,'command':cmd}))
    print(f'{label}: exit={r.returncode}',flush=True)
    return r.returncode
files=subprocess.check_output(['git','diff','--name-only','--','bindings/dotnet'],text=True).splitlines()
files+=['bindings/dotnet/tests/Zlink.Tests/'+f for f in ['test_closed_submit_ownership.cs','test_socket_handle_reservation.cs','test_submit_close_reservation.cs']]
if run('format-apply-final',['dotnet','format','whitespace','bindings/dotnet/Zlink.sln','--no-restore','--include',*files]): raise SystemExit(1)
if run('format-verify-final',['dotnet','format','whitespace','bindings/dotnet/Zlink.sln','--no-restore','--verify-no-changes','--include',*files]): raise SystemExit(1)
if run('build-tests-final',['dotnet','build','bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj','-m:1']): raise SystemExit(1)
shutil.copytree('bindings/dotnet/tests/Zlink.Tests/bin/Debug/net8.0',out/'main-fixed-fixture-tests',dirs_exist_ok=True)
for f in ['Systems.Zlink.dll','Systems.Zlink.pdb']:
    shutil.copy2(out/'perf-main'/f,out/'main-fixed-fixture-tests'/f)
run('main-fixed-fixture-ordered',['dotnet',str(out/'ordered-isolation/bin/Debug/net8.0/Ordered.dll'),str(out/'main-fixed-fixture-tests')])
run('after-fixed-fixture-ordered',['dotnet',str(out/'ordered-isolation/bin/Debug/net8.0/Ordered.dll'),str(Path('bindings/dotnet/tests/Zlink.Tests/bin/Debug/net8.0').resolve())])
run('suite-except-baseline-gc',['bash','bindings/dotnet/tests/run_tests.sh','--filter','FullyQualifiedName!~payload_round_trips_during_concurrent_compacting_gc','--logger','console;verbosity=normal','--logger','trx;LogFileName=suite-except-baseline-gc.trx','--results-directory',str(out/'raw'/'suite-except-baseline-gc-results'),'--diag',str(out/'raw'/'suite-except-baseline-gc-diag.log')])
run('diff-check-final',['git','diff','--check'])
