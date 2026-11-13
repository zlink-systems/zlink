"""Temporarily stop a single owned benchmark thread via ptrace, without code edits."""
import ctypes,os,time
libc=ctypes.CDLL(None,use_errno=True);libc.ptrace.argtypes=[ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p,ctypes.c_void_p];libc.ptrace.restype=ctypes.c_long

def call(request,tid):
 if libc.ptrace(request,tid,None,None)==-1:
  err=ctypes.get_errno();raise OSError(err,os.strerror(err))

def pause(tid,seconds,sample):
 # PTRACE_SEIZE + INTERRUPT stops only this thread; it sends no group SIGSTOP.
 call(0x4206,tid);stopped=False
 try:
  call(0x4207,tid);got,status=os.waitpid(tid,0x40000000)
  if got!=tid or not os.WIFSTOPPED(status):raise RuntimeError(f'Unexpected ptrace status {got} {status}')
  stopped=True;start=time.monotonic();before=sample();time.sleep(seconds);after=sample()
  return {'tid':tid,'held_seconds':time.monotonic()-start,'wait_status':status,'stopped_begin':before,'stopped_end':after}
 finally:
  if stopped:call(17,tid)
