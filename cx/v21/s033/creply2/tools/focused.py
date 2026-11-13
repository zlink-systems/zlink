import subprocess,pathlib,os,shutil,json,re,sys
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/creply2')
B=pathlib.Path('/root/worktree/zlink-3867-creply2/framework/bench/grpc/cpp/build')
pattern='reply_close' if '--reply-only' in sys.argv else 'reply_close|close_failure'
subprocess.run(['ctest','--test-dir',str(B),'-R',pattern,'--output-on-failure','-j1'],check=True)
shutil.copy2(B/'Testing/Temporary/LastTest.log',P/'logs/deadline-focused-LastTest.log')
env=os.environ.copy();env['LD_PRELOAD']=str(B/'framework/libtest_cpp_framework_reply_close_clock.so')
with (P/'logs/deadline-prototype-destructor-control.log').open('w') as f:
 result=subprocess.run([str(B/'creply_prototype_close_control'),'destructor-control'],env=env,stdout=f,stderr=subprocess.STDOUT)
s=(P/'logs/deadline-prototype-destructor-control.log').read_text()
assert result.returncode==1,(result.returncode,s[-2000:])
rows=re.findall(r'iteration=(\d+) close_attempts=(\d+) busy=(\d+) native_after_close=(\d+)',s)
assert len(rows)==100 and all(int(r[3])==0 and int(r[2])==2 for r in rows),rows
(P/'negative-control.json').write_text(json.dumps({'exit':result.returncode,'iterations':len(rows),'native_handles_left_open':100,'native_busy_observed_per_iteration':2,'cleanup':'real public close probe closed each leaked handle before context destruction'},indent=2))
print('Expected negative control failed: native handles left open 100/100 through binding destructors',flush=True)
