import pathlib as P,subprocess as S,shlex,sys,json
j=P.Path('/root/jobs/fx/cx/v21/s033/csites');r=P.Path('/root/worktree/zlink-3867h/framework/languages/cpp');b=r/'build/linux-ninja-release';old=j/'before-src';dest=j/'baseline-test';dest.mkdir(exist_ok=True)
target='test_cpp_framework_request_call_allocations';cmds=S.check_output(['ninja','-C',str(b),'-t','commands',target],text=True).splitlines();objects={}
def arg(x):
 if x.startswith('-I'+str(r)+'/../../'):return '-I'+str(P.Path(x[2:]).resolve())
 return x.replace(str(r),str(old))
for line in cmds:
 if ' -c ' not in line or 'CMakeFiles/'+target+'.dir/' not in line:continue
 cmd=shlex.split(line);obj=cmd[cmd.index('-o')+1];src=cmd[cmd.index('-c')+1];output=dest/(str(len(objects))+'-'+P.Path(src).stem+'.o');objects[obj]=str(output)
 filtered=[];i=0
 while i<len(cmd):
  if cmd[i]=='-MD':i+=1;continue
  if cmd[i] in ['-MT','-MF']:i+=2;continue
  filtered.append(arg(cmd[i]));i+=1
 filtered[filtered.index('-o')+1]=str(output)
 if not output.exists():S.run(filtered,cwd=b,check=True)
 print('baseline compiled',P.Path(src).name,flush=True)
link=shlex.split(cmds[-1]);link=link[2:link.index('&&',2)]
for i,x in enumerate(link):
 if x in objects:link[i]=objects[x]
 elif x.startswith('libzlink_framework'):link[i]=str(j/'before-pristine'/x)
 elif x==target:link[i]=str(dest/target)
 else:link[i]=arg(x)
S.run(link,cwd=b,check=True)
code=S.run([str(dest/target),'--gtest_filter=RequestCallAllocations.NodeReplyConstructsOnlyFinalPublicAndPhysicalResultStates:RequestCallAllocations.SpotFinalReceiverUsesFewerAllocationsThanTheConversionChain'],cwd=r).returncode
print('baseline tests exit',code,flush=True);raise SystemExit(code)
