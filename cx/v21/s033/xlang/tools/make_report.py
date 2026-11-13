import collections,datetime,json,pathlib,statistics
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');W=pathlib.Path('/root/worktree/zlink-xlang')
def load(p):return json.loads(p.read_text())
S=load(P/'summary.json');PR=load(P/'profile-summary.json');CP=load(P/'tools/copy-paths.json');patterns=['send-saturation','request-backpressure','request-serial'];langs=['cpp','java','dotnet','node'];names={'cpp':'C++','java':'Java','dotnet':'.NET','node':'Node'};refs={'send-saturation':'dotnet','request-backpressure':'node','request-serial':'java'}
def pick(mode,lang,fw,pattern):
 rows=[r for r in S if (mode=='unpaced' or r['job'].get('rate')==load(P/'rates.json')[pattern]) and r['job']['mode']==mode and r['job']['lang']==lang and r['job']['fw']==fw and r['job']['pattern']==pattern]
 if mode=='unpaced':
  assert len(rows)==3,(mode,lang,fw,pattern,len(rows));return sorted(r['throughput'] for r in rows)
 assert len(rows)==1,(mode,lang,fw,pattern,len(rows));return rows[0]
def profile(lang,fw,pattern):
 a=[r for r in PR if r['job']['lang']==lang and r['job']['fw']==fw and r['job']['pattern']==pattern];assert len(a)==1;return a[0]
def number(v,d=3):return '미측정' if v is None else f'{v:,.{d}f}'
def inverse(lang,fw,p):return 1e6/statistics.median(pick('unpaced',lang,fw,p))
def rate_range(v):return f'{v[1]:,.0f} ({v[0]:,.0f}–{v[2]:,.0f})'
def us_range(v):return f'{1e6/v[1]:.3f} ({1e6/v[2]:.3f}–{1e6/v[0]:.3f})'
def scaled_roles(lang,fw,p):
 base=pick('paced',lang,fw,p);pro=profile(lang,fw,p);prun=pick('profile',lang,fw,p);d=collections.Counter()
 for side in ['source','target']:
  core=collections.Counter()
  for t in base['threads']:
   if t['side']==side and 'ZLINKbg/IO/' in t['comm']:
    k='Core I/O send' if t['sendto_per_msg']>t['recvfrom_per_msg'] else 'Core I/O receive' if t['recvfrom_per_msg']>0 else 'Core I/O other'
    core[k]+=t['cpu_us']
  d.update(core)
  b=base['roles'][side]['cpu_us']-sum(core.values())
  pc=pro['roles'][side]['categories'];den=sum(v for k,v in pc.items() if not k.startswith('Core I/O'))
  for k,v in pc.items():
   if not k.startswith('Core I/O'):d[k]+=v*b/den if den else 0
 return d

def alloc(lang,fw,p):
 a=pick('alloc',lang,fw,p)
 if lang=='cpp':v=a.get('native_alloc',{});return sum(r['bytes_per_msg'] for r in v.values()),sum(r['count_per_msg'] for r in v.values()),''
 if lang=='java':v=a.get('jvm_alloc',{});return sum(r['bytes_per_msg'] for r in v.values()),sum(r['count_per_msg'] for r in v.values()),''
 if lang=='node':v=a.get('v8_alloc',{});return sum(r['bytes_per_msg'] for r in v.values()),sum(r['count_per_msg'] for r in v.values()),' 추정'
 v=a.get('dotnet_alloc',{});return v.get('bytes_per_msg'),v.get('count_per_msg_estimate'),' count 추정'

lines=['# Framework 네 언어의 동일 세션 측정','', 'Framework÷binding 목표는 0.6으로 유지한다. 1,024 B의 send-saturation, request-backpressure, request-serial을 같은 배타 lock 세션에서 측정했다. 기준 언어는 11-01 표에 따라 각각 .NET, Node, Java로 고정했다. 이번 세션의 최고값으로 기준을 바꾸지 않았다.','', '처리량의 역수 µs/msg는 처리 능력을 비교하는 값이다. 두 프로세스의 user+kernel CPU µs/msg와 다르다. request의 한 메시지는 request/reply 왕복 한 건이며, send는 target에 전달된 command 한 건이다. F는 Framework, B는 binding을 뜻한다.','', '무제한 median에서 C++ Framework는 send 184,781 msg/s, backpressure 52,802 req/s, serial 8,001 req/s였다. 기준의 Framework 계층 시간과 비교한 차이는 각각 +2.861, −3.697, +23.119 µs/msg다. request-backpressure의 음수는 고정 기준인 Node보다 자기 F−B의 절대 시간이 작다는 뜻이다. F÷B 비율과 절대 F−B 시간의 순위는 binding 처리 능력이 달라 같지 않다.','', '해석 범위는 측정값과 코드로 확인한 단계까지다. 역할 CPU의 other가 남아 있어 C++·Java send-saturation과 .NET request-serial의 세부 병목 및 추가 논리 단계를 확정하지 못했다. 처리량 역수의 차이를 paced CPU로 완전히 분해한 결과로 해석하지 않는다.','', '## 무제한 처리량','', '값은 RUNS=3의 median과 min–max다. request 셀의 단위는 req/s, send 셀은 msg/s다. µs/msg의 범위는 처리량 범위를 역수로 바꿨다.','', '| 셀 | 언어 | Framework 처리량 | binding 처리량 | Framework µs/msg | binding µs/msg | F÷B |','|---|---|---:|---:|---:|---:|---:|']
for p in patterns:
 for lang in langs:
  f=pick('unpaced',lang,True,p);b=pick('unpaced',lang,False,p)
  lines.append(f"| {p} | {names[lang]}{' **기준**' if refs[p]==lang else ''} | {rate_range(f)} | {rate_range(b)} | {us_range(f)} | {us_range(b)} | {f[1]/b[1]:.3f} |")
for p in patterns:
 cols=[(l,fw) for fw in [True,False] for l in langs];reference=refs[p];rate=load(P/'rates.json')[p]
 lines+=['',f'## {p} 1 KiB', '',f'공통 pacing 목표는 {rate:,} msg/s다. 기준 언어는 {names[reference]}이다. CPU 행은 두 프로세스 합계이며, Core I/O 행은 thread schedstat 실측값이다. 나머지 역할 행은 profile의 표본 비율을 같은 rate의 나머지 CPU에 배분한 추정이다. 역할이 같은 OS thread를 공유해도 CPU를 중복 합산하지 않았다. 표본에 잡히지 않은 역할의 0.000은 해당 단계가 없다는 증거가 아니다. 함수 inlining과 stack 해석 한계로 분류하지 못한 CPU는 other에 남겼다. other는 총 CPU에서 빠진 값이 아니라 활동별 소유를 확정하지 못한 값이다.','', '| 측정값 | '+' | '.join(names[l]+(' F' if fw else ' B')+(' **기준**' if l==reference else '') for l,fw in cols)+' |','|---|'+'---:|'*8]
 def row(label,fn):
  vals=[]
  for l,fw in cols:
   vals.append(str(fn(l,fw)))
  lines.append('| '+label+' | '+' | '.join(vals)+' |')
 row('무제한 µs/msg',lambda l,fw:number(inverse(l,fw,p)))
 row('paced 실제 msg/s',lambda l,fw:number(pick('paced',l,fw,p)['actual_rate'],1))
 row('총 CPU µs/msg',lambda l,fw:number(pick('paced',l,fw,p)['total_us']))
 row('source CPU µs/msg',lambda l,fw:number(pick('paced',l,fw,p)['roles']['source']['cpu_us']))
 row('target CPU µs/msg',lambda l,fw:number(pick('paced',l,fw,p)['roles']['target']['cpu_us']))
 row('user CPU µs/msg',lambda l,fw:number(pick('paced',l,fw,p)['user_us']))
 row('kernel CPU µs/msg',lambda l,fw:number(pick('paced',l,fw,p)['kernel_us']))
 for role in ['caller/submit','dispatch/mesh','handler','Core I/O send','Core I/O receive','completion','binding receive/poll','codec/copy','allocation','GC','pacing','other','Core I/O other','unobserved threads/tick residual']:
  row(role+' CPU µs/msg',lambda l,fw,k=role:number(scaled_roles(l,fw,p).get(k,0)))
 row('OS context switches/msg',lambda l,fw:number(pick('paced',l,fw,p)['switches_per_msg']))
 row('thread wake 시작/msg (handoff 대용값)',lambda l,fw:number(pick('handoff',l,fw,p)['wakeups_per_msg']))
 row('동일 프로세스 thread wakeup/msg',lambda l,fw:number(pick('handoff',l,fw,p)['wakeup_breakdown_per_msg'].get('same_process',0)))
 row('프로세스 간 thread wakeup/msg',lambda l,fw:number(pick('handoff',l,fw,p)['wakeup_breakdown_per_msg'].get('cross_process',0)))
 row('프로세스 소유 미확인 wake 시작/msg',lambda l,fw:number(pick('handoff',l,fw,p)['wakeup_breakdown_per_msg'].get('unknown',0)))
 row('관리 heap allocation bytes/msg',lambda l,fw:'—' if l=='cpp' else number(alloc(l,fw,p)[0],1)+(' 추정' if l=='node' else ''))
 row('관리 heap allocation count/msg',lambda l,fw:'—' if l=='cpp' else number(alloc(l,fw,p)[1])+(' 추정' if l in ('node','dotnet') else ''))
 row('native malloc bytes/msg',lambda l,fw:number(sum(v['bytes_per_msg'] for v in pick('alloc',l,fw,p)['native_alloc'].values()),1))
 row('native malloc calls/msg',lambda l,fw:number(sum(v['count_per_msg'] for v in pick('alloc',l,fw,p)['native_alloc'].values())))
 row('payload-size copy/msg (코드 경로)',lambda l,fw:CP['counts'][l][('F' if fw else 'B')+('_send' if p=='send-saturation' else '_request')])
 row('TCP data packets/msg (loopback capture)',lambda l,fw:number(pick('paced',l,fw,p)['tcp']['data_packets_per_msg']))
 row('TCP payload bytes/msg',lambda l,fw:number(sum(v for k,v in pick('paced',l,fw,p)['tcp'].items() if k.endswith('_bytes')),1))
 row('TCP data segments/msg (TCP_INFO)',lambda l,fw:number(pick('paced',l,fw,p)['ss_data_segs_per_msg']))
 row('별도 profile 실행의 CPU µs/msg',lambda l,fw:number(pick('profile',l,fw,p)['total_us']))
 row('profile native CPU 표본 수',lambda l,fw:sum(x['native_samples'] for x in profile(l,fw,p)['roles'].values()))
 row('profile 관리 runtime 표본 수',lambda l,fw:sum(x['managed_samples'] for x in profile(l,fw,p)['roles'].values()))
 row('별도 allocation 실행의 실제 msg/s',lambda l,fw:number(pick('alloc',l,fw,p)['actual_rate'],1))
 nb=pick('paced','node',False,p);nf=pick('paced','node',True,p)
 def node_main_cpu(r):
  pid=load(pathlib.Path(r['path'])/'pids.json')['target']
  return sum(t['cpu_us'] for t in r['threads'] if t['side']=='target' and t['tid']==pid)
 lines+=['',f"Node target JS thread의 CPU는 F {node_main_cpu(nf):.3f}, B {node_main_cpu(nb):.3f} µs/msg다. binding target은 빈 receive에서도 setImmediate poll을 반복한다(node/zlink-raw-server/main.js:139). 이 bench 비용이 F−B CPU의 음수에 영향을 주므로, 그 음수를 Framework 제품 계층의 CPU 절감량으로 단정하지 않는다."]
 lines+=['','네 언어의 native malloc 행은 같은 LD_PRELOAD counter로 수집했다. 관리 heap과 native allocator의 요청은 서로 다른 단위이므로 한 allocation 합계로 합산하지 않았다. Java native malloc에는 JVMTI·JFR 실행을 지원하는 JVM 비용도 포함된다. 관리 runtime의 native malloc을 비계측 제품 경로의 count로 단정하지 않았다.','',f'### {names[reference]} 기준의 Framework 계층 차이','', '| 언어 | 자기 F−B µs/msg | 기준 F−B와의 차이 µs/msg | paced CPU의 F−B µs/msg | 기준의 paced CPU F−B와의 차이 µs/msg |','|---|---:|---:|---:|---:|']
 ref_gap=inverse(reference,True,p)-inverse(reference,False,p);ref_cpu=pick('paced',reference,True,p)['total_us']-pick('paced',reference,False,p)['total_us'];ref_roles=scaled_roles(reference,True,p);ref_br=scaled_roles(reference,False,p)
 for l in langs:
  gap=inverse(l,True,p)-inverse(l,False,p);cpu=pick('paced',l,True,p)['total_us']-pick('paced',l,False,p)['total_us']
  lines.append(f"| {names[l]}{' 기준' if l==reference else ''} | {gap:+.3f} | {gap-ref_gap:+.3f} | {cpu:+.3f} | {cpu-ref_cpu:+.3f} |")
 lines+=['','다음 값은 각 CPU 행에 `자기(F−B) − 기준(F−B)`를 적용한 차이다. 처리량 역수 차이를 이 CPU 값의 합으로 설명하지 않았다. 여러 thread가 병렬로 소비하는 CPU와 대기 시간이 처리량의 역수에 그대로 더해지지 않기 때문이다.','']
 for l in langs:
  if l==reference:continue
  f=scaled_roles(l,True,p);b=scaled_roles(l,False,p);keys=set(f)|set(b)|set(ref_roles)|set(ref_br)
  diffs=sorted(((k,f[k]-b[k]-ref_roles[k]+ref_br[k]) for k in keys),key=lambda x:abs(x[1]),reverse=True)
  lines.append(f"{names[l]}의 차이가 큰 CPU 행은 "+', '.join(f'{k} {v:+.3f} µs/msg' for k,v in diffs[:5])+'.');lines.append('')
 lines+=['','확인한 추가 단계는 관련 CPU 행 차이의 절댓값이 큰 묶음부터 적었다. 묶음의 시간은 해당 단계만의 독립 비용이 아니다. allocation·copy·wake 시작을 각각 몇 µs라고 환산하지 않았다.','']
 notes=load(P/'tools/step_notes.json')[p]
 for l in langs:
  if l==reference:continue
  f=scaled_roles(l,True,p);b=scaled_roles(l,False,p);diff={k:f[k]-b[k]-ref_roles[k]+ref_br[k] for k in set(f)|set(b)|set(ref_roles)|set(ref_br)}
  bodykey='_send' if p=='send-saturation' else '_request';cf=CP['counts'][l]['F'+bodykey];cb=CP['counts'][l]['B'+bodykey];crf=CP['counts'][reference]['F'+bodykey];crb=CP['counts'][reference]['B'+bodykey]
  pkt=lambda lang,fw:pick('paced',lang,fw,p)['tcp']['data_packets_per_msg']
  pd=pkt(l,True)-pkt(l,False)-pkt(reference,True)+pkt(reference,False)
  if abs(pd)<.0005:pd=0.0
  lines.append(f'{names[l]}의 F/B body pass는 {cf}/{cb}회이며, 기준의 {crf}/{crb}회에 대한 계층 차이는 {(cf-cb)-(crf-crb):+d}회/msg다. TCP data packet의 같은 계층 차이는 {pd:+.3f} packets/msg다.')
  extra=notes.get(l,[])
  if extra:
   ranked=sorted(((abs(sum(diff.get(k,0) for k in e['roles'])),sum(diff.get(k,0) for k in e['roles']),e) for e in extra),key=lambda v:v[0],reverse=True)
   for i,(_,value,e) in enumerate(ranked,1):lines.append(f"{names[l]} 추가 단계 {i}: {e['text']} 관련 CPU 행 {', '.join(e['roles'])}의 합계 차이는 {value:+.3f} µs/msg다.")
  else:
   lines.append(f'{names[l]}의 경우 이 셀의 코드 대조에서 기준에 없는 추가 논리 단계를 확정하지 못했다. 위 CPU 행은 공통 단계의 구현 비용 차이와 bench 비용을 포함한다.')
  if p=='send-saturation' and l=='cpp':lines.append('C++ send의 이미 완료된 task는 bench_async.hpp:46의 fast path에서 별도 completed-slot deque를 거치지 않는다. request용 queue 경로를 send 비용의 원인으로 단정하지 않았다.')
  if p=='request-serial' and l=='dotnet':lines.append('.NET과 Java Framework의 body pass는 모두 왕복 6회다. 두 언어의 F−B copy 차이 +4회/msg는 Java binding의 ByteString.copyFrom·toByteArray·Message.from 경로와 .NET binding의 직접 span encode 차이에서 생긴다. Framework가 4회 더 복사한다는 뜻이 아니다.')
  lines.append('')

lines+=['','## 경로 대조','',CP['boundary'],'']
for lang,rows in CP['paths'].items():
 lines+=[f'### {names[lang]}','']
 for text in rows:lines+=[text,'']
lines+=['## 측정 방법','', '워크트리는 `/root/worktree/zlink-xlang`이다. main 스냅샷 `d961e51e231234e8404d74164d2a27c0cf2fce41`의 detached HEAD와 기존 CPU pin 및 bench 계측 변경에서 실행했다. Core·binding·Framework 제품 소스와 spec은 수정하지 않았다. git은 status와 diff만 사용했고, metadata의 git 호출은 `bin/git` shim으로 막았다. C++ 병렬 작업의 worktree는 수정하지 않았다. 추가 pacing 변경 파일은 framework/bench/grpc 기준 cpp/client/bench_cpp_client.cpp, java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java, dotnet/Client/BenchMeasurement.cs, node/client/bench-core.js다. 네 run_local.sh의 기존 CPU pin 변경은 보존했다. diff는 [instrumentation.diff](instrumentation.diff)에 있다.','', '기존 `build-*.log`는 비어 있었고 이 worktree의 예상 실행 파일이 없어 네 언어의 누락 빌드를 실행했다. C++ 빌드는 `cpp-build-slot.sh`를 바깥 flock 없이 사용했고, Java/.NET은 `measure-shared.sh taskset -c 0-7 bash <lang>/build.sh`를 사용했다. Node도 공용 shared build wrapper를 사용했다. 빌드 로그와 실행 파일의 provenance는 같은 폴더의 로그와 manifest에 남겼다.','', '측정은 `flock -o /root/jobs/fx/cx/measure.lock python3 tools/session.py` 한 세션에서 수행했다. source는 CPU 0–7, target은 8–15로 고정했다. 무제한 측정은 warmup 10초와 active 10초를 3회 실행했다. 언어 순서와 F/B 순서는 두 번째 반복에서 뒤집었다. 공통 rate는 셀 내 24개 무제한 값의 최솟값의 0.4배를 send·backpressure는 1,000, serial은 100 단위로 내림해 정했다. 이는 목표 0.6의 변경이 아니라 CPU 비교를 위한 부하 설정이다. paced와 profile은 active 30초, allocation은 active 20초다. thread handoff의 wake 시작은 warmup 5초·active 15초의 별도 동일 rate 실행에서 수집했다.','', 'MemAvailable 12 GiB 하한을 유지하기 위해 각 Java JVM의 heap 상한은 2 GiB, direct memory 상한은 1 GiB로 고정했다. .NET은 GC heap hard limit 2 GiB, Node는 old-space 2 GiB로 고정했다. 이 설정은 모든 해당 언어의 F/B와 무제한·paced 실행에 동일하게 적용했다. 이전 세션의 절대값과 직접 같다고 가정하지 않았다.','', '총 CPU는 `/proc/PID/stat`의 user와 system tick 차이를 완료 메시지 수로 나눴다. thread CPU는 `/proc/PID/task/TID/schedstat`의 runtime 차이다. schedstat와 process tick의 양자화·시점 차이는 residual 행에 남겼다. Core I/O send/receive는 thread의 gated sendto/recvfrom 호출 수 중 더 많은 방향으로 분류한 역할명이다. 혼합 방향 thread의 CPU를 syscall별로 다시 분해하지 않았다. CPU 역할의 원본 thread/TID, syscall 방향, 표본 수와 함수는 각 실행의 `profile-summary.json`에 있다. 소유 함수가 없는 일반 executor·ThreadPool 프레임은 other로 분류했다. C++은 perf CPU stack, Java는 perf와 JFR execution sample, .NET은 perf와 EventPipe CPU sampling, Node는 perf와 V8 CPU profile을 사용했다. Node native stack의 offline symbol 해석에서는 addr2line의 inline 확장을 끄고 V8 표본으로 JS 소유 함수를 보완했다.','', '네 언어의 pacing은 공유 submit ticket을 1 ms bucket으로 내려 같은 부하 형태를 적용했다. C++은 sleep_until, Java는 parkNanos, .NET은 nanosleep, Node는 timer를 사용했다. Node serial은 Atomics.wait를 사용한다. pacing은 payload의 측정 timestamp를 찍기 전에 수행했다.','', 'thread wakeup은 별도 동일 rate 실행의 BPF sched_waking에서 깨우는 thread와 깨어나는 thread가 모두 측정 source/target에 속한 이벤트만 셌다. 동일 프로세스와 두 프로세스 사이의 wakeup을 나눠 표시했다. 이 행은 깨어남 시작을 센 handoff 대용값이다. sched_wakeup의 실행 문맥은 원래 waker가 아닐 수 있어 처음 수집한 해당 counter는 사용하지 않았다. 두 thread의 Event 왕복 1,000회 fixture에서 sched_waking이 각 방향 1,000회 이상을 잡는 것을 확인한 뒤 24개 조합을 다시 수집했다. 한 메시지의 논리적 queue 전달 횟수와 같지 않다. 이미 runnable인 thread로 전달하면 wakeup이 없을 수 있다. 별도 OS context switch 행은 각 thread의 자발적·비자발적 switch 합계다.','', '네 언어의 native allocation 행은 LD_PRELOAD로 malloc·calloc·realloc 호출 횟수와 요청 byte를 셌다. free는 byte allocation에 포함하지 않았다. aligned allocation 전용 API는 별도로 가로채지 않아 이 경로의 native allocation 전체를 보장하지 않는다. Java는 JFR의 ObjectAllocationInNewTLAB·ObjectAllocationOutsideTLAB도 기록했다. TLAB capacity bytes와 refill event는 allocation-summary.json에 보존했다. 표의 실제 객체 수는 JVMTI SampledObjectAlloc을 interval 0으로 켜 JVM heap object count와 size를 직접 셌다. interval 0 검증에서 byte[1024] 10,000개의 10,400,000 bytes와 count 10,000을 얻었다. Java native malloc은 이 숫자에 포함하지 않았다.','', '.NET bytes는 기존 bench의 GC.GetTotalAllocatedBytes(precise: true)를 사용했다. count는 EventPipe GCAllocationTick의 AllocationAmount64/ObjectSize 합계로 추정했다. 이는 모든 객체의 실제 count가 아니며, 표의 count를 추정으로 표시했다. 큰 객체와 작은 객체가 섞일 때 이 추정은 type/size 표본 편향의 영향을 받는다. Node는 V8 SamplingHeapProfiler interval 4,096 B와 major/minor GC로 수거된 객체 포함 옵션을 사용해 size/count를 추정했다. Node native malloc은 별도 LD_PRELOAD 값이다. V8 heap 표본은 JS heap을 대상으로 하며 Buffer의 외부 backing storage를 관리 heap bytes에 더하지 않았다. allocation 계측 실행의 CPU는 기본 CPU 표에 사용하지 않았다. 기본 CPU는 bench의 pacing·통계·빈 receive poll 비용도 포함한다. 특히 Node binding target은 zlink-raw-server/main.js:139의 setImmediate poll을 빈 구간에도 반복하므로 낮은 부하에서의 F−B CPU를 순수 제품 계층 비용으로 볼 수 없다. 이 차이는 allocation과 context switch에도 영향을 준다.','', 'payload copy는 코드에서 1 KiB 본문이 새 저장소에 쓰이는 횟수다. protobuf encode와 소유 bytes decode는 copy에 포함하고, view·move와 metadata copy는 제외했다. 이는 동적 memcpy 호출 수가 아니며 allocator 내부 이동, Core와 kernel의 복사는 범위 밖이다.','', 'TCP data packets는 tcpdump -Q in으로 중복을 제거한 loopback incoming capture의 TCP payload 길이가 양수인 packet 수다. HTTP stats 포트는 제외했다. 양쪽 socket TCP_INFO의 data_segs_out 차이도 함께 기록했다. loopback offload 때문에 capture packet과 TCP segment 수가 다를 수 있으므로 wire상의 물리 Ethernet packet 수로 해석하지 않았다.','', '처음 네 paced capture는 loopback에서 tcpdump -Q out이 packet을 수집하지 못해 무효 처리했다. -Q in으로 고친 뒤 같은 lock 세션에서 네 조합을 다시 측정했고, 표에는 재측정값만 사용했다. 원본과 무효 사유는 삭제하지 않았다. 초기 profile 세 개도 perf 기본 clock과 proc snapshot의 CLOCK_MONOTONIC이 달라 제외하고, --clockid mono를 명시해 같은 lock에서 재측정했다.','', '모든 유효 실행은 source errors·abandoned와 target errors가 0이고, request는 submitted=completed=target received를 확인했다. send는 completed=received+rejected를 확인했다. 검증기는 정상 fixture 3개와 의도적으로 count 또는 오류를 틀린 fixture 6개의 양쪽 결과를 시험했다. allocation counter에도 알려진 allocation 수와 bytes를 주입해 확인했다. native counter는 3 calls·15,360 bytes, Node 표본 검사는 100,000개 생성에서 104,930개 추정을 기록했다. TCP parser는 command 전용 port와 역방향 연결의 payload packet을 잡고 ACK-only packet을 제외하는 양쪽 fixture를 통과했다.','']
meta=load(P/'session.json');lines.append(f"측정 세션은 {datetime.datetime.fromtimestamp(meta['start']).isoformat(timespec='seconds')}에 시작했다. 세션에서 기록한 최소 MemAvailable은 {meta.get('min_mem_kb',0)/1048576:.3f} GiB다. SIGTERM/SIGINT만 사용했다.");lines.append("")
if meta.get('end'):lines.append(f"세션 종료 시각은 {datetime.datetime.fromtimestamp(meta['end']).isoformat(timespec='seconds')} KST다.")
build_mem=load(P/'build-memory.json');lines.append(f"빌드 구간에서 기록한 최소 MemAvailable은 {build_mem['min_mem_kb']/1048576:.3f} GiB이며, 메모리 하한 때문에 종료한 빌드는 없었다.")
lines.extend(['','원본 수치는 [summary.json](summary.json), 역할 분류는 [profile-summary.json](profile-summary.json), allocation event 해석은 [allocation-summary.json](allocation-summary.json)에 있다. 실행 명령·stats·pcap·trace는 runs/ 아래에 조합별로 보존했다. 종료한 benchmark의 JIT perf map은 symbols/로 옮겼으며, raw perf를 다시 symbol 해석할 때 해당 map을 /tmp의 원래 파일명으로 복원할 수 있다. 네 언어가 실제 로드한 Core library의 SHA-256은 모두 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01로 같았다. 실행 파일과 native library의 SHA-256은 [manifest.json](manifest.json)에 있다. 유효 168회는 오류·abandoned가 없고 count 검사를 통과했다. 무효 7회는 표에서 제외했고 미해결 실행 실패는 없다. 최종 검증은 [final-check.log](final-check.log)와 [check-cells.log](check-cells.log)에 있다.'])
(P/'report.md').write_text('\n'.join(lines)+'\n')
print('wrote',P/'report.md',len(lines),'lines')
