import json,os,pathlib,subprocess,sys
j=pathlib.Path(__file__).parent;argv=sys.argv[1:];out=pathlib.Path(argv[argv.index('--out')+1]);out.mkdir(parents=True,exist_ok=True);pid=argv[argv.index('--target')+1];pm=os.environ.get('PM')=='1';seconds='25' if pm else '5'
cmd=['perf','record','-o',str(out/'cpu.data'),'-e','cpu-clock','-F','99','--call-graph','dwarf,8192','-p',pid,'--','sleep',seconds]
procs=[]
procs.append(subprocess.Popen(cmd,stdout=(out/'cpu.stdout').open('w'),stderr=(out/'cpu.stderr').open('w')))
if pm:
 cmd=['perf','record','-o',str(out/'stages.data'),'-e','psched5:publish','-e','psched5:start','-e','psched5:dispatch','-p',pid,'--','sleep',seconds]
 procs.append(subprocess.Popen(cmd,stdout=(out/'stages.stdout').open('w'),stderr=(out/'stages.stderr').open('w')))
# Trace target thread switches and wakeups, with worker IDs saved by the regular collector.
procs.append(subprocess.Popen(['perf','record','-o',str(out/'sched.data'),'-e','sched:sched_switch','-e','sched:sched_wakeup','-p',pid,'--','sleep',seconds],stdout=(out/'sched.stdout').open('w'),stderr=(out/'sched.stderr').open('w')))
r=subprocess.run(['python3',str(j/('collect.py' if pm else 'collect-22k.py')),*argv])
for p in procs:
 p.wait()
 if p.returncode: raise SystemExit(f'perf exited {p.returncode}; see {out}')
sys.exit(r.returncode)
