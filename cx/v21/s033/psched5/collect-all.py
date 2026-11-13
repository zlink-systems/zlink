import json,os,pathlib,subprocess,sys
j=pathlib.Path(__file__).parent;argv=sys.argv[1:];out=pathlib.Path(argv[argv.index('--out')+1]);out.mkdir(parents=True,exist_ok=True);pid=argv[argv.index('--target')+1];pm=os.environ.get('PM')=='1';seconds='25' if pm else '5'
procs=[]
cmd=['perf','record','-o',str(out/'stages.data'),'-a','-C','8-15','-e','psched5:publish','--filter','owner != "node"','-e','psched5:start','--filter','owner != "node"','-e','psched5:dispatch','--filter','owner != "node"','-e','psched5:http','-e','psched5:spot','-e','psched5:local','--','sleep',seconds]
procs.append(subprocess.Popen(cmd,stdout=(out/'stages.stdout').open('w'),stderr=(out/'stages.stderr').open('w')))
cmd=['perf','record','-o',str(out/'sched.data'),'-a','-C','8-15','-e','sched:sched_switch','-e','sched:sched_wakeup','--','sleep',seconds]
procs.append(subprocess.Popen(cmd,stdout=(out/'sched.stdout').open('w'),stderr=(out/'sched.stderr').open('w')))
r=subprocess.run(['python3',str(j/('collect.py' if pm else 'collect-22k.py')),*argv])
for p in procs:
 p.wait()
 if p.returncode: raise SystemExit(f'perf exited {p.returncode}; see {out}')
sys.exit(r.returncode)
