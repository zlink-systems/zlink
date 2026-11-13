import subprocess, sys, time, json, os, shutil
from pathlib import Path
out = Path(__file__).parent
status = out/'raw'/'after-final-focused.status.json'
while not status.exists():
    time.sleep(1)
if json.loads(status.read_text())['exit_code']:
    print('focused test failed; dependent validation stopped', flush=True)
    sys.exit(1)
env = os.environ.copy()
env.update(EnableSourceControlManagerQueries='false', EnableSourceLink='false')
files = subprocess.check_output(['git','diff','--name-only','--','bindings/dotnet'], text=True).splitlines()
files += ['bindings/dotnet/tests/Zlink.Tests/'+s for s in ['test_closed_submit_ownership.cs','test_socket_handle_reservation.cs','test_submit_close_reservation.cs']]
def run(label, command):
    assert int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:'))) >= 12*1024*1024
    with (out/'raw'/f'{label}.log').open('w') as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, env=env)
    (out/'raw'/f'{label}.status.json').write_text(json.dumps({'exit_code':result.returncode,'command':command}))
    print(f'{label}: exit={result.returncode}', flush=True)
    if result.returncode:
        sys.exit(result.returncode)
run('format-verify-complete', ['dotnet','format','whitespace','bindings/dotnet/Zlink.sln','--no-restore','--verify-no-changes','--include',*files])
run('build-after-perf-final', ['dotnet','build','bindings/dotnet/perf/single/Zlink.BindingBench/Zlink.BindingBench.csproj','-c','Release','-m:1','-p:EnableSourceControlManagerQueries=false','-p:EnableSourceLink=false'])
shutil.copytree('bindings/dotnet/perf/single/Zlink.BindingBench/bin/Release/net8.0', out/'perf-after-final',dirs_exist_ok=True)
run('full-suite', ['bash','bindings/dotnet/tests/run_tests.sh','--logger','console;verbosity=normal','--logger','trx;LogFileName=full-suite.trx','--results-directory',str(out/'raw'/'full-suite-results')])
run('diff-check', ['git','diff','--check'])
