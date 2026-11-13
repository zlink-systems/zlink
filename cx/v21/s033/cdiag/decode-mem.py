from pathlib import Path
import subprocess
j=Path(__file__).parent
for d in (j/'mem/send-saturation/branch/run1/zlink-framework-cpp-send-saturation-1024/capture',j/'mem-repeat/send-saturation/main/run1/zlink-framework-cpp-send-saturation-1024/capture'):
 p=d/'mem-script.log'
 if p.exists():p.rename(d/'mem-script-field-error.log')
 with (d/'mem.script').open('w') as dest,(d/'mem-script.log').open('w') as log:
  subprocess.run(['perf','script','-i',str(d/'mem.data'),'-F','comm,pid,tid,time,event,ip,sym,symoff,dso,addr,data_src'],stdout=dest,stderr=log,check=True)
