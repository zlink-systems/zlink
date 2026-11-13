import argparse, json, pathlib, signal, subprocess, time
ap=argparse.ArgumentParser(); ap.add_argument('--before',action='store_true'); a=ap.parse_args()
job=pathlib.Path(__file__).parent
build=pathlib.Path('/root/worktree/zlink-v21-s033/psched5/.psched5-tests')
if a.before:
 tests=[{'name':'test_cpp_framework_scheduler_drain','command':[str(build/'test_cpp_framework_scheduler_drain')]}]; attempts=1; prefix='before-'
else:
 tests=json.loads(subprocess.check_output(['ctest','--test-dir',str(build),'-R','^test_cpp_framework_(scheduler_drain|mailbox_owner_dispatch)','--show-only=json-v1']))['tests']; attempts=50; prefix=''
 (job/'focused-registrations.json').write_text(json.dumps(tests,indent=2)+'\n')
results=[]
for test in tests:
 for attempt in range(1,attempts+1):
  name=test['name']; log=job/f'{prefix}{name}-{attempt}.log'
  with log.open('w') as out:
   started=time.monotonic(); p=subprocess.Popen(test['command'],cwd=build,stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
   try:
    rc=p.wait(timeout=8); result='pass' if rc==0 else 'fail'
   except subprocess.TimeoutExpired:
    # Capture every process in this test's process group before SIGTERM.
    targets=[]
    for proc in pathlib.Path('/proc').iterdir():
     if not proc.name.isdigit(): continue
     try:
      import os
      if os.getpgid(int(proc.name))==p.pid: targets.append(int(proc.name))
     except ProcessLookupError: pass
    for pid in targets:
     with (job/f'{prefix}{name}-{attempt}-{pid}-stacks.txt').open('w') as stack:
      debugger=subprocess.Popen(['gdb','-q','-batch','-p',str(pid),'-ex','set pagination off','-ex','thread apply all bt','-ex','detach'],stdout=stack,stderr=subprocess.STDOUT)
      try: debugger.wait(timeout=10)
      except subprocess.TimeoutExpired: debugger.terminate(); debugger.wait()
    os.killpg(p.pid,signal.SIGTERM); rc=p.wait(); result='timeout; stopped with SIGTERM'
   results.append({'test':name,'attempt':attempt,'result':result,'exit':rc,'elapsed_seconds':time.monotonic()-started,'raw':str(log)})
   (job/f'{prefix}focused-results.json').write_text(json.dumps(results,indent=2)+'\n')
   if attempt==attempts or result!='pass': print(json.dumps(results[-1]),flush=True)
   if result!='pass': raise SystemExit(1)
