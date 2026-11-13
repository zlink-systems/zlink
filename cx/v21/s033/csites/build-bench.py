import pathlib as P,subprocess as S,shlex,shutil,sys,json
j=P.Path('/root/jobs/fx/cx/v21/s033/csites');stage=sys.argv[1]; dest=j/stage;dest.mkdir(exist_ok=True)
old=P.Path('/root/jobs/fx/cx/v21/s033/crecv3/build');root=P.Path('/root/worktree/zlink-3867h');cpp=root/'framework/languages/cpp';build=cpp/'build/linux-ninja-release';src=j/'before-src' if stage.startswith('before') else cpp
archive_source=j/'before-pristine' if stage.startswith('before') else build
counted=not stage.endswith('-perf')
# csite2 interception and workload sources remain unchanged.
shutil.copy2('/root/jobs/fx/cx/v21/s033/csite2/libchand.so',dest/'libchand.so')
for name in ['libzlink_framework.a','libzlink_framework_provider_abstractions.a']:
 if not (dest/name).exists():shutil.copy2(archive_source/name,dest/name)
cmds=S.check_output(['ninja','-C',str(old),'-t','commands','bench_cpp_client','bench_cpp_framework_server'],text=True).splitlines()
def rewrite(x):
 x=x.replace('/root/worktree/zlink-3867f/framework/languages/cpp/../../../',str(root)+'/').replace('/root/worktree/zlink-3867f',str(root))
 if x.startswith('-I'+str(cpp)+'/../../'):x='-I'+str(P.Path(x[2:]).resolve())
 elif x.startswith('-I'+str(cpp)):x=x.replace(str(cpp),str(src))
 return x
header='extern "C" void chand_role(int);\nextern "C" void chand_event(int);\n'
for name,archive in ([('mesh_node_host_service.cpp','libzlink_framework.a'),('provider_task_context.cpp','libzlink_framework_provider_abstractions.a')] if counted else []):
 rel=next((src/'framework/src').rglob(name));s=header+rel.read_text()
 if name.startswith('mesh'):
  needle='std::optional<zlink::poll_event_flag_t> readiness;\n        while (!host._stop';assert needle in s;s=s.replace(needle,'chand_role(2);\n        '+needle,1).replace('readiness = rotate (readiness);','{ chand_event(0); readiness = rotate (readiness); chand_event(1); }',1)
 else:
  s=s.replace('[this] { _context.run (); }','[this] { chand_role(3); _context.run (); }').replace('boost::asio::post (_context,','chand_event(2);\n        boost::asio::post (_context,').replace('const completion_turn_scope_t scope{owner','chand_event(3);\n        const completion_turn_scope_t scope{owner')
 p=dest/name;p.write_text(s)
 orig=next(shlex.split(x) for x in cmds if ' -c ' in x and ('/'+name) in x)
 cmd=[];i=0
 while i<len(orig):
  if orig[i]=='-MD':i+=1;continue
  if orig[i] in ['-MT','-MF']:i+=2;continue
  cmd.append(rewrite(orig[i]));i+=1
 cmd[cmd.index('-c')+1]=str(p);obj=dest/(name+'.o');cmd[cmd.index('-o')+1]=str(obj)
 cmd+=['-I'+str(rel.parent),'-DZLINK_FRAMEWORK_TEST_CREATION_PROBE','-g','-fno-omit-frame-pointer']
 
 if not obj.exists():S.run(cmd,check=True)
 S.run(['ar','r',str(dest/archive),str(obj)],check=True)
for target in ['bench_cpp_client','bench_cpp_framework_server']:
 name='bench_client.cpp' if target.endswith('client') else 'bench_framework_server.cpp'
 source=P.Path('/root/jobs/fx/cx/v21/s033/csite2/bench_client.cpp') if name=='bench_client.cpp' and counted else old/name
 orig=next(shlex.split(x) for x in cmds if ' -c ' in x and ('/'+name) in x)
 cmd=[];i=0
 while i<len(orig):
  if orig[i]=='-MD':i+=1;continue
  if orig[i] in ['-MT','-MF']:i+=2;continue
  cmd.append(rewrite(orig[i]));i+=1
 cmd[cmd.index('-c')+1]=str(source);obj=dest/(name+'.o');cmd[cmd.index('-o')+1]=str(obj);cmd+=['-DZLINK_FRAMEWORK_TEST_CREATION_PROBE','-g','-fno-omit-frame-pointer'];S.run(cmd,check=True)
 link=next(shlex.split(x) for x in reversed(cmds) if ' -o '+target+' ' in x)
 link=link[2:link.index('&&',2)]
 for i,x in enumerate(link):
  if x==target:link[i]=str(dest/target)
  elif x=='CMakeFiles/'+target+'.dir/'+name+'.o':link[i]=str(obj)
  elif x in ['libzlink_framework.a','libzlink_framework_provider_abstractions.a']:link[i]=str(dest/x)
  elif x.startswith('CMakeFiles/'):link[i]=str(old/x)
  else:link[i]=rewrite(x)
 
 if counted:link+=['-L'+str(dest),'-lchand','-Wl,-rpath,'+str(dest)]
 S.run(link,check=True)
shutil.copy2((old if stage.startswith('before') else j/'before-perf')/'bench_cpp_zlink_server',dest/'bench_cpp_zlink_server')
print('built',stage,flush=True)
