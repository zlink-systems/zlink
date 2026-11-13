from pathlib import Path
import subprocess,json,sys
J=Path(__file__).parent;c=json.loads((J/'build-config.json').read_text());src=J/'src';b=J/'build'
base=['c++','-O3','-DNDEBUG','-std=c++20','-g1','-pthread']+[x.replace(chr(92),'') for x in c['defines']]+c['includes']+['-I/root/worktree/zlink-3867s/framework/runtime/protocol/generated/cpp']
targets=[('raw-client','raw-client',0),('raw-server','raw-server',0),('codec-client','raw-client',1),('codec-server','codec-server',1),('layers-client','layers-client',1),('layers-server','layers-server',1),('full-window-client','full-window-client',1)]
if len(sys.argv)>1:targets=[x for x in targets if x[0] in sys.argv[1:]]
for name,file,codec in targets:
 obj=b/(name+'.o');cmd=base+['-DLADDER_CODEC='+str(codec),'-c',str(src/(file+'.cpp')),'-o',str(obj)]
 print('compile',name,flush=True);subprocess.run(cmd,check=True)
 if name.startswith('raw-'):
  deps=['/root/.cache/zlink/zlink-cpp/1.23.0/lib/libzlink_cpp.a','/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so.1.23.0','-Wl,--start-group']
  deps += [x for x in c['libs'] if ('protobuf' in x or 'absl' in x or 'utf8' in x)]
  deps+=['-Wl,--end-group','-ldl','-lz','-lm']
 else:deps=c['libs']
 subprocess.run(['c++','-pthread',str(obj),str(b/'bench.pb.cc.o')]+([str(b/'bench.grpc.pb.cc.o')] if name=='full-window-client' else [])+['-Wl,-rpath,/root/.cache/zlink/core/1.23.0/linux-x64/lib']+deps+['-o',str(b/name)],check=True)
