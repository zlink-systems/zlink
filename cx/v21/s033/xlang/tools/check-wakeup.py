import subprocess,sys,time,json,pathlib,signal
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');code='''import threading,os,sys,time,json
a=threading.Event();b=threading.Event();stop=threading.Event()
def worker():
 print(json.dumps({'tid':threading.get_native_id()}),flush=True)
 for i in range(1000):
  a.wait();a.clear();b.set()
t=threading.Thread(target=worker);t.start();input()
for i in range(1000):
 a.set();b.wait();b.clear();time.sleep(.0005)
t.join()
'''
f=subprocess.Popen([sys.executable,'-u','-c',code],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True);tid=json.loads(f.stdout.readline())['tid'];pid=f.pid
program=f'''BEGIN {{@own[{pid}]={pid};@own[{tid}]={pid};printf("READY\\n");}}
tracepoint:sched:sched_waking /pid=={pid}/ {{@all[args->pid]=count();if(@own[args->pid]){{@plain[args->pid]=count();}}if(@own[(uint64)args->pid]){{@cast[args->pid]=count();}}}}
END {{clear(@own);}}
'''
p=P/'wakeup-fixture.bt';p.write_text(program);b=subprocess.Popen(['bpftrace','-q','-f','json','-B','line',str(p)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
lines=[]
while True:
 l=b.stdout.readline();lines.append(l)
 if 'READY' in l:break
 if not l:raise RuntimeError(lines)
f.stdin.write('\n');f.stdin.flush();f.wait(timeout=10);b.send_signal(signal.SIGINT);tail=b.communicate(timeout=10)[0];text=''.join(lines)+tail;(P/'wakeup-fixture.log').write_text(text);print(text)
