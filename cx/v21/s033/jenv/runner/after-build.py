from pathlib import Path
import subprocess,time,os
P=Path('/root/jobs/fx/cx/v21/s033/jenv')
while True:
 log=(P/'prototype-build.log').read_text()
 if 'BUILD FAILED' in log:raise SystemExit('prototype build failed')
 if 'BUILD SUCCESSFUL' in log:break
 time.sleep(1)
cmd=['/root/jobs/fx/cx/measure-shared.sh','taskset','-c','0-7','/usr/lib/jvm/java-25-openjdk-amd64/bin/java','--enable-native-access=ALL-UNNAMED','--class-path','/root/worktree/zlink-jenv/framework/bench/grpc/java/client/build/install/bench-client/lib/*',str(P/'tools/DescriptorCheck.java')]
with (P/'descriptor-check.log').open('w') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
with (P/'remaining-run.log').open('w') as f:subprocess.run(['flock','-o','/root/jobs/fx/cx/measure.lock','python3',str(P/'runner/remaining.py')],stdout=f,stderr=subprocess.STDOUT,check=True)
print('remaining matrix finished',flush=True)
