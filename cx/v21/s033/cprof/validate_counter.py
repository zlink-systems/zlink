import pathlib,subprocess,json,os,time,struct
J=pathlib.Path(__file__).parent; d=J/'counter-check';d.mkdir(exist_ok=True)
def read(p):
 b=p.read_bytes();m,v,n=struct.unpack_from('QQQ',b);assert m==0x435050414c4c4f43
 total=[0]*4
 for i in range(n):
  tid,*vals=struct.unpack_from('QQQQQ',b,4096+i*64)
  for k,v in enumerate(vals):total[k]+=v
 return total
proc=subprocess.Popen([str(J/'helper-build/counter_check')],stdout=subprocess.PIPE,text=True,env={**os.environ,'LD_PRELOAD':str(J/'alloc_counter.so'),'PROF_ALLOC_DIR':str(d)})
pid=int(proc.stdout.readline().split('=')[1]);p=d/f'alloc-{pid}.bin'
before=read(p);time.sleep(1.5);after=read(p);assert proc.wait()==0
observed=[b-a for a,b in zip(before,after)];expected=[1000,1000,1000,2000]
assert observed==expected,(observed,expected)
wrong=expected.copy();wrong[0]+=1
assert observed!=wrong
(d/'validation.json').write_text(json.dumps({'fields':['malloc','calloc','realloc','free'],'observed':observed,'expected':expected,'positive_pass':True,'deliberately_wrong_expectation_rejected':observed!=wrong},indent=2))
print('counter positive and negative validation passed',observed)
