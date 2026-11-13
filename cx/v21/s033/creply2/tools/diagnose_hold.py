import pathlib,subprocess,os,signal,time,json
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/creply2');B=pathlib.Path('/root/worktree/zlink-3867-creply2/framework/bench/grpc/cpp/build')
env=os.environ.copy();env['LD_PRELOAD']=str(B/'framework/libtest_cpp_framework_reply_close_clock.so')
with (P/'logs/diagnose-hold-test.log').open('w') as t,(P/'logs/diagnose-hold-stack.log').open('w') as s:
 child=subprocess.Popen([str(B/'framework/test_cpp_framework_reply_close'),'deterministic'],env=env,stdout=t,stderr=t,start_new_session=True)
 bpf=subprocess.Popen(['bpftrace','-e',f'tracepoint:syscalls:sys_enter_clock_nanosleep /pid == {child.pid} && tid == {child.pid}/ {{ print(ustack(32)); exit(); }}'],stdout=s,stderr=s,start_new_session=True)
 try:
  until=time.monotonic()+12
  while time.monotonic()<until and bpf.poll() is None and child.poll() is None:time.sleep(.1)
 finally:
  if bpf.poll() is None:os.killpg(bpf.pid,signal.SIGINT)
  bpf.wait()
  if child.poll() is None:os.killpg(child.pid,signal.SIGTERM)
  child.wait()
 print('diagnostic child',child.returncode,'bpf',bpf.returncode)
