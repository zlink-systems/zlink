from pathlib import Path
import shutil,subprocess,os
P=Path('/root/jobs/fx/cx/v21/s033/jenv');W=Path('/root/worktree/zlink-jenv')
# Preserve the original baseline runtime and apply only the already-built pacing driver.
src=P/'main-build/client/build/install/bench-client';dst=P/'main-counter-build/client/build/install/bench-client'
shutil.copytree(src,dst,dirs_exist_ok=True)
shutil.copy2(W/'framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar',dst/'lib/client.jar')
s=(P/'runner/run.py').read_text()
s=s[:s.index('# The enclosing flock owns the whole sequential matrix')]
s=s.replace("raw=a.case=='binding'", "raw=a.case.startswith('binding')")
s=s.replace("a.case in ('main','binding')", "a.case in ('main','binding','main-counters','binding-counters')")
s=s.replace("str(build/'client/build/install/bench-client/bin/bench-client')", "str((P/'main-counter-build' if a.case=='main-counters' else build)/'client/build/install/bench-client/bin/bench-client')")
s=s.replace("str(out/'capture')", "str(out/'capture-bpf')")
s=s.replace("if not a.smoke:\n  subprocess.run([env['JAVA_HOME']+'/bin/jcmd'", "if False:\n  subprocess.run([env['JAVA_HOME']+'/bin/jcmd'")
a=s.index(" if not a.smoke:\n  collect('perf')");b=s.index(' while not active.done():',a)
s=s[:a]+" collect('bpf')\n"+s[b:]
(P/'runner/run-counter.py').write_text(s)
collector=(P/'tools/collect.py').read_text()
old="  cond=' || '.join(f'pid == {pid}' for pid in pids.values());cp=os.getpid()"
new="""  initial=snapshot()
  io_tids=[int(t) for role in pids for t,v in initial['processes'][role]['threads'].items() if 'IO/' in v['comm']]
  cond=' || '.join(f'tid == {tid}' for tid in io_tids);cp=os.getpid()"""
assert old in collector
collector=collector.replace(old,new)
start=collector.index("  program=f'''BEGIN")
stop=collector.index("  program += f'''",start)
template=(P/'tools/tcp-counter.bt.tmpl').read_text()
collector=collector[:start]+"  program="+repr(template)+".format(cp=cp,source=pids['source'],target=pids['target'],cond=cond)\n"+collector[stop:]
(P/'tools/collect-counter.py').write_text(collector)
s=(P/'runner/run-counter.py').read_text().replace("str(P/'tools/collect.py')", "str(P/'tools/collect-counter.py')")
(P/'runner/run-counter.py').write_text(s)
for c in ('main-counters','prototype-counters','binding-counters'):
 print('COUNTER',c,flush=True)
 subprocess.run(['python3',str(P/'runner/run-counter.py'),c,'--rate','61132'],check=True)
subprocess.run(['python3',str(P/'tools/finish_counters.py')],check=True)
