#!/bin/bash
set -euo pipefail
c++ -O2 -shared -fPIC -I/usr/include/node -o /root/jobs/fx/cx/v21/s033/xlang/tools/node-alloc.node /root/jobs/fx/cx/v21/s033/xlang/tools/node-alloc.cc

cc -O2 -shared -fPIC -o /root/jobs/fx/cx/v21/s033/xlang/tools/alloc.so /root/jobs/fx/cx/v21/s033/xlang/tools/alloc.c
mkdir -p /root/jobs/fx/cx/v21/s033/xlang/check-alloc-taskset
XLANG_ALLOC_DIR=/root/jobs/fx/cx/v21/s033/xlang/check-alloc-taskset LD_PRELOAD=/root/jobs/fx/cx/v21/s033/xlang/tools/alloc.so taskset -c 0-7 /root/jobs/fx/cx/v21/s033/xlang/tools/counter-check
python3 - <<'PY'
import pathlib,struct,json
rows=[]
for p in pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang/check-alloc-taskset').glob('*.bin'):
 b=p.read_bytes();v=struct.unpack_from('4Q',b,4096);assert v[1:]==(3,15360,2),v;rows.append({'file':str(p),'count':v[1],'bytes':v[2],'frees':v[3]})
pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang/alloc-taskset-check.json').write_text(json.dumps(rows,indent=2));print(rows)
PY
node --expose-gc /root/jobs/fx/cx/v21/s033/xlang/tools/node-counter-check.cjs > /root/jobs/fx/cx/v21/s033/xlang/node-counter-check.json
cc -O2 -shared -fPIC -I/usr/lib/jvm/java-25-openjdk-amd64/include -I/usr/lib/jvm/java-25-openjdk-amd64/include/linux -o /root/jobs/fx/cx/v21/s033/xlang/tools/jvm-alloc.so /root/jobs/fx/cx/v21/s033/xlang/tools/jvm-alloc.c
/usr/lib/jvm/java-25-openjdk-amd64/bin/javac -J-Xmx256m /root/jobs/fx/cx/v21/s033/xlang/tools/AllocCheck.java
mkdir -p /root/jobs/fx/cx/v21/s033/xlang/check-jvm-jfr
XLANG_ALLOC_DIR=/root/jobs/fx/cx/v21/s033/xlang/check-jvm-jfr taskset -c 0-7 /usr/lib/jvm/java-25-openjdk-amd64/bin/java -Xmx256m -agentpath:/root/jobs/fx/cx/v21/s033/xlang/tools/jvm-alloc.so -XX:StartFlightRecording=filename=/root/jobs/fx/cx/v21/s033/xlang/jvm-counter-check.jfr,settings=/root/jobs/fx/cx/v21/s033/xlang/tools/alloc.jfc -cp /root/jobs/fx/cx/v21/s033/xlang/tools AllocCheck > /root/jobs/fx/cx/v21/s033/xlang/jvm-jfr-counter-check.log 2>&1
