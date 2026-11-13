import pathlib as P, shutil, subprocess as S, shlex, json, hashlib
j=P.Path('/root/jobs/fx/cx/v21/s033/csite2'); r=P.Path('/root/worktree/zlink-3867h'); b=P.Path('/root/jobs/fx/cx/v21/s033/crecv3/build'); c=j/'framework/languages/cpp'; c.mkdir(parents=True,exist_ok=True)
shutil.copytree(r/'framework/languages/cpp/framework/src',c/'framework/src',dirs_exist_ok=True)
# Only these copies receive counter calls. All other objects are reused after source comparison.
commands=S.check_output(['ninja','-C',str(b),'-t','commands','bench_cpp_client'],text=True).splitlines()
compile_cmds=[shlex.split(x) for x in commands if ' -c ' in x]
manifest=[]
for cmd in compile_cmds:
 src=P.Path(cmd[cmd.index('-c')+1]); candidate=P.Path(str(src).replace('/root/worktree/zlink-3867f',str(r)))
 if src.is_file() and candidate.is_file():
  a=hashlib.sha256(src.read_bytes()).hexdigest(); z=hashlib.sha256(candidate.read_bytes()).hexdigest(); manifest.append({'source':str(candidate),'sha256':z,'matches_reused_source':a==z})
(j/'source-manifest.json').write_text(json.dumps(manifest,indent=2)); assert all(x['matches_reused_source'] for x in manifest),[x for x in manifest if not x['matches_reused_source']]
header='extern "C" void chand_role(int);\nextern "C" void chand_event(int);\n'
mesh=c/'framework/src/runtime/mesh/mesh_node_host_service.cpp';s=mesh.read_text();s=header+s;s=s.replace('std::optional<zlink::poll_event_flag_t> readiness;\n        while (!host._stop','chand_role(2);\n        std::optional<zlink::poll_event_flag_t> readiness;\n        while (!host._stop',1);s=s.replace('readiness = rotate (readiness);','{ chand_event(0); readiness = rotate (readiness); chand_event(1); }',1);mesh.write_text(s)
p=c/'framework/src/runtime/dispatch/provider_task_context.cpp';s=header+p.read_text();s=s.replace('[this] { _context.run (); }','[this] { chand_role(3); _context.run (); }');s=s.replace('boost::asio::post (_context,','chand_event(2);\n        boost::asio::post (_context,');s=s.replace('const completion_turn_scope_t scope{owner','chand_event(3);\n        const completion_turn_scope_t scope{owner');p.write_text(s)
client=j/'bench_client.cpp';s=header+(b/'bench_client.cpp').read_text();s=s.replace('std::vector<std::unique_ptr<pending_t>> pending;','chand_role(1);\n        std::vector<std::unique_ptr<pending_t>> pending;')
# mark caller for whole run_tasks, regardless of local container spelling
needle='        struct pending_t\n'; assert needle in s;s=s.replace(needle,'        chand_role(1);\n'+needle,1);client.write_text(s)
# Generate an isolated CMake build with exact baseline compiler/link arguments.
def quoted(cmd):return ' '.join('"'+x.replace('\\','\\\\').replace('"','\\"')+'"' for x in cmd)
def custom(out,cmd,dep):return 'add_custom_command(OUTPUT '+str(out)+'\n COMMAND '+quoted(cmd)+'\n DEPENDS '+str(dep)+'\n VERBATIM)\n'
cm='cmake_minimum_required(VERSION 3.20)\nproject(chand NONE)\n'
outs=[]
for rel in ['framework/src/runtime/mesh/mesh_node_host_service.cpp','framework/src/runtime/dispatch/provider_task_context.cpp','bench_client.cpp']:
 name=P.Path(rel).name; orig=next(x for x in compile_cmds if x[x.index('-c')+1].endswith('/'+name));cmd=[];i=0
 while i<len(orig):
  if orig[i] in ['-MD']: i+=1;continue
  if orig[i] in ['-MT','-MF']:i+=2;continue
  cmd.append(orig[i].replace('/root/worktree/zlink-3867f',str(r)));i+=1
 cmd[cmd.index('-c')+1]=str(j/'bench_client.cpp' if name=='bench_client.cpp' else c/rel)
 out=j/(name+'.o');cmd[cmd.index('-o')+1]=str(out);cm+=custom(out,cmd,cmd[cmd.index('-c')+1]);outs.append(out)
# libs include all unchanged baseline objects; replace only instrumented objects.
for lib,idx in [('libzlink_framework.a',0),('libzlink_framework_provider_abstractions.a',1)]:
 dest=j/lib;shutil.copy2(b/lib,dest);cm+=custom(j/(lib+'.stamp'),['/usr/bin/ar','r',str(dest),str(outs[idx])],outs[idx]);cm+='add_custom_command(OUTPUT '+str(j/(lib+'.ready'))+' COMMAND /usr/bin/touch '+str(j/(lib+'.ready'))+' DEPENDS '+str(j/(lib+'.stamp'))+')\n'
 # ar custom stamp must exist
 cm=cm.replace('COMMAND /usr/bin/ar r '+str(dest)+' '+str(outs[idx]),'COMMAND /usr/bin/ar r '+str(dest)+' '+str(outs[idx])+'\n COMMAND /usr/bin/touch '+str(j/(lib+'.stamp')))
link=shlex.split(commands[-1]);link=link[2:link.index('&&',2)] if link[:2]==[':', '&&'] else link
cmd=[]
for x in link:
 if x in ['libzlink_framework.a','libzlink_framework_provider_abstractions.a']:x=str(j/x)
 elif x=='CMakeFiles/bench_cpp_client.dir/bench_client.cpp.o':x=str(outs[2])
 elif x.startswith('CMakeFiles/'):x=str(b/x)
 elif x=='bench_cpp_client':x=str(j/'bench_cpp_client')
 cmd.append(x.replace('/root/worktree/zlink-3867f',str(r)))
cmd+=['-L'+str(j),'-lchand','-Wl,-rpath,'+str(j)]
cm+=custom(j/'bench_cpp_client',cmd,' '.join(map(str,[outs[2],j/'libzlink_framework.a.ready',j/'libzlink_framework_provider_abstractions.a.ready',j/'libchand.so'])))
cm+='add_custom_command(OUTPUT '+str(j/'libchand.so')+' COMMAND /usr/bin/g++ -shared -fPIC -O2 -std=c++20 '+str(j/'counters.cpp')+' -o '+str(j/'libchand.so')+' -ldl -pthread DEPENDS '+str(j/'counters.cpp')+' VERBATIM)\n'
cm+='add_custom_target(chand ALL DEPENDS '+str(j/'bench_cpp_client')+')\n';(c/'CMakeLists.txt').write_text(cm)
(c/'CMakePresets.json').write_text(json.dumps({'version':3,'configurePresets':[{'name':'linux-ninja-release','generator':'Ninja','binaryDir':str(j/'build')}],'buildPresets':[{'name':'linux-ninja-release','configurePreset':'linux-ninja-release'}]},indent=2))
print('source comparisons',len(manifest),'all match')
