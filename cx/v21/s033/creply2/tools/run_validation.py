from pathlib import Path
import subprocess,os,shutil,json,re,hashlib
P=Path('/root/jobs/fx/cx/v21/s033/creply2');B=Path('/root/worktree/zlink-3867-creply2/framework/bench/grpc/cpp/build')
expected=json.loads((P/'inputs.json').read_text())
for source in ['/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so','/root/.cache/zlink/zlink-cpp/1.23.0/lib/libzlink_cpp.a']:
 assert hashlib.sha256(Path(source).read_bytes()).hexdigest()==expected[source],source
results={}
results['suite']=subprocess.run(['python3',str(P/'tools/run_suite.py')]).returncode
results['stress']=subprocess.run(['python3',str(P/'tools/stress_race.py')]).returncode
# Preserve the Release control before repeating with the final Debug test source.
for source,target in [('logs/deadline-prototype-destructor-control.log','logs/deadline-prototype-destructor-control-release.log'),('negative-control.json','negative-control-release.json')]:
 if not (P/target).exists():shutil.copy2(P/source,P/target)
env=os.environ.copy();env['LD_PRELOAD']=str(B/'framework/libtest_cpp_framework_reply_close_clock.so');env['LD_LIBRARY_PATH']='/root/.cache/zlink/core/1.23.0/linux-x64/lib:'+env.get('LD_LIBRARY_PATH','')
with (P/'logs/deadline-prototype-destructor-control.log').open('w') as log:
 result=subprocess.run([str(B/'creply_prototype_close_control'),'destructor-control'],env=env,stdout=log,stderr=subprocess.STDOUT)
s=(P/'logs/deadline-prototype-destructor-control.log').read_text()
rows=re.findall(r'iteration=(\d+) close_attempts=(\d+) busy=(\d+) native_after_close=(\d+)',s)
assert result.returncode==1 and len(rows)==100 and all(int(r[3])==0 and int(r[2])==2 for r in rows),(result.returncode,len(rows))
assert 'message ownership: all 100 iterations consumed or released' in s
(P/'negative-control.json').write_text(json.dumps({'exit':result.returncode,'iterations':len(rows),'native_handles_left_open':100,'native_busy_observed_per_iteration':2,'cleanup':'real public close probe closed each leaked handle before context destruction','configuration':'Debug, same final test source as positive race'},indent=2))
results['negative_control_expected_failure']=True
(P/'validation-status.json').write_text(json.dumps(results,indent=2));print(results,flush=True)
raise SystemExit(0 if results['suite']==0 and results['stress']==0 else 1)
