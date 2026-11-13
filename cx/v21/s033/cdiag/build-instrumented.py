from pathlib import Path
import subprocess,shlex,json,concurrent.futures,sys
j=Path(__file__).parent
files=['stateful/public_host_runtime.cpp','mesh/service_mailbox.cpp','mesh/mesh_node_host_service.cpp','dispatch/offload_executor.cpp','execution/serial_execution_queue.cpp']
for v,old in [('main',j.with_name('cdrain')/'main-build'),('branch',j.with_name('cdrain2')/'build')]:
 b=j/(v+'-build');commands=json.loads((j/(v+'-commands.json')).read_text());selected=[];objects=[]
 for f in files+['bench_pm_server.cpp']:
  matches=[c for c in commands if ('/'+f+' ' in c+' ' or ' '+f+' ' in c+' ') and ' -c ' in c]
  assert len(matches)==1,(v,f,len(matches))
  args=shlex.split(matches[0]);src=args[args.index('-c')+1];out=args[args.index('-o')+1]
  dest=b/out;dest.parent.mkdir(parents=True,exist_ok=True)
  args[args.index('-c')+1]=str(j/v/('bench_pm_server.cpp' if f=='bench_pm_server.cpp' else 'framework/languages/cpp/framework/src/runtime/'+f))
  args[args.index('-o')+1]=str(dest)
  if '-MF' in args:args[args.index('-MF')+1]=str(dest)+'.d'
  selected.append((f,args));
  if f!='bench_pm_server.cpp':objects.append(str(dest))
 def compile(item):
  f,args=item
  with (j/(v+'-'+Path(f).name+'.compile.log')).open('w') as log:
   result=subprocess.run(args,cwd=b,stdout=log,stderr=log)
  print(v,f,result.returncode,flush=True)
  if result.returncode:raise RuntimeError(f)
 with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:list(pool.map(compile,selected))
 subprocess.run(['/usr/bin/c++','-O3','-DNDEBUG','-std=c++20','-c',str(j/'trace.cpp'),'-o',str(b/'trace.o')],check=True,cwd=b)
 subprocess.run(['/usr/bin/ar','r',str(b/'libzlink_framework.a'),*objects],check=True,cwd=b)
 link=commands[-1].removeprefix(': && ').removesuffix(' && :')
 args=shlex.split(link);args.insert(args.index('-o'),str(b/'trace.o'))
 subprocess.run(args,check=True,cwd=b,stdout=(j/(v+'-link.log')).open('w'),stderr=subprocess.STDOUT)
 print(v,'linked',flush=True)
