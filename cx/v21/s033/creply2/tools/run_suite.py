from pathlib import Path
import subprocess,os,shutil,json,time
P=Path('/root/jobs/fx/cx/v21/s033/creply2');B=Path('/root/worktree/zlink-3867-creply2/framework/bench/grpc/cpp/build')
env=os.environ.copy();env['LD_LIBRARY_PATH']='/root/.cache/zlink/core/1.23.0/linux-x64/lib:'+env.get('LD_LIBRARY_PATH','')
command=['ctest','--test-dir',str(B),'-L','^(unit|contract)$','--output-on-failure','--output-junit',str(P/'logs/ctest-unit-contract.xml'),'-j2']
start=time.time()
with (P/'logs/ctest-unit-contract.log').open('w') as log:
 log.write('COMMAND '+repr(command)+'\n');log.flush()
 result=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT)
shutil.copy2(B/'Testing/Temporary/LastTest.log',P/'logs/ctest-unit-contract-LastTest.log')
shutil.copy2(B/'CMakeCache.txt',P/'CMakeCache-tests.txt')
(P/'suite-status.json').write_text(json.dumps({'command':command,'exit':result.returncode,'started':start,'ended':time.time()},indent=2))
print('Full unit/contract suite exit:',result.returncode,flush=True)
raise SystemExit(result.returncode)
