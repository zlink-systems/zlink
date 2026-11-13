import pathlib,subprocess,json,hashlib,shutil,time,sys
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/creply2');B=pathlib.Path('/root/worktree/zlink-3867-creply2/framework/bench/grpc/cpp/build')
BUILD='/root/jobs/fx/cx/cpp-build-slot.sh';SHARED='/root/jobs/fx/cx/measure-shared.sh'
def run(name,wrapper,cmd):
 (P/'stage.json').write_text(json.dumps({'name':name,'requested_at':time.time(),'cmd':cmd}))
 print('STAGE',name,flush=True)
 subprocess.run([wrapper,'python3',str(P/'tools/guard.py'),name,*cmd],check=True)
status=P/'logs/configure-retry.status.json'
while not status.exists():time.sleep(1)
assert json.loads(status.read_text())['returncode']==0,status.read_text()
if '--after-focused' not in sys.argv:
 run('build-focused-resume1',BUILD,['cmake','--build',str(B),'--target','test_cpp_framework_reply_close','test_cpp_framework_close_failure','bench_cpp_framework_server','creply_prototype_close_control','-j1'])
 run('focused',SHARED,['python3',str(P/'tools/focused.py')])
run('build-suite-resume1',BUILD,['python3',str(P/'tools/build_suite.py')])
run('ctest-unit-contract',SHARED,['ctest','--test-dir',str(B),'--output-on-failure','-L','^(unit|contract)$','--output-junit',str(P/'logs/ctest-unit-contract.xml'),'-j1'])
shutil.copy2(B/'Testing/Temporary/LastTest.log',P/'logs/ctest-unit-contract-LastTest.log')
shutil.copy2(B/'bench_cpp_framework_server',P/'bin-direct/bench_cpp_framework_server')
(P/'binary-direct.json').write_text(json.dumps({'sha256':hashlib.sha256((P/'bin-direct/bench_cpp_framework_server').read_bytes()).hexdigest()},indent=2))
(P/'stage.json').write_text(json.dumps({'name':'measurement-wait','requested_at':time.time()}))
with (P/'logs/measurements.log').open('w') as f:
 subprocess.run(['flock','-o','/root/jobs/fx/cx/measure.lock','python3',str(P/'runner/measure_all.py')],stdout=f,stderr=subprocess.STDOUT,check=True)
subprocess.run(['python3',str(P/'tools/summarize_runs.py')],check=True)
(P/'pipeline-completed.json').write_text(json.dumps({'time':time.time()}))
