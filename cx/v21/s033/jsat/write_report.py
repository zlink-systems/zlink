import json,pathlib
base=pathlib.Path('/root/jobs/fx/cx/v21/s033/jsat')
items=[
 ('framework-final','Framework 무제한'),
 ('binding-paced-cadence2','Binding 57,550 req/s pacing'),
 ('binding-unpaced-final','Binding 무제한'),
]
role_ko={
 'caller/submit':'호출/제출',
 'binding/Core I/O':'binding/Core I/O',
 'completion/pump':'completion/pump',
 'Framework dispatcher/executor':'Framework dispatcher/executor',
 'Framework mesh / binding transport':'Framework mesh thread',
 'binding receive/request':'binding receive/request',
 'GC':'GC', 'JIT':'JIT', 'timer':'timer',
 'Framework dispatcher/handler carrier':'Framework dispatcher/handler carrier',
 'JVM/other':'JVM/기타',
}
lines=[]
lines.append('| 조건 | 프로세스 | TID | thread name (`/proc` comm) | 역할 | CPU cores | CPU µs/request | voluntary switch/request | involuntary switch/request | run-queue wait µs/request |')
lines.append('|---|---|---:|---|---|---:|---:|---:|---:|---:|')
for name,label in items:
 d=json.load(open(base/name/'thread-metrics.json'))
 for row in d['rows']:
  lines.append('| {} | {} | {} | `{}` | {} | {:.3f} | {:.2f} | {:.3f} | {:.3f} | {:.3f} |'.format(
   label,row['process'],row['tid'],row['thread'],role_ko.get(row['role'],row['role']),row['cores'],row['us_per_req'],row['vol_per_req'],row['invol_per_req'],row['runqueue_us_per_req']))
lines += [
 '',
 '## 판정',
 '',
 '이번 측정에서 Framework의 active 처리율은 57,549.8 req/s였다. 해당 속도에서는 source의 `bench-phase-active` 제출 thread가 0.714 core와 12.41 µs/request를 사용했고, target에서 가장 바쁜 thread는 `ZLINKbg/IO/1`로 0.773 core와 13.42 µs/request를 사용했다. 어느 thread도 실제 측정 속도에서 0.9 core에 도달하지 않았다. runnable 상태로 기다린 시간도 이 thread에서 0.07 µs/request였다. 다른 주요 source thread의 대기 시간은 0.09~0.32 µs/request였고 target의 주요 thread는 0.01~0.50 µs/request였다. OS run-queue 경쟁은 측정된 처리율의 제한 원인으로 보이지 않는다.',
 '',
 '75,645 req/s는 제시된 unpaced binding 기준 126,075 req/s의 0.6배다. 이 목표에서 thread 하나가 1 core를 넘지 않으려면 13.22 µs/request 이하여야 한다. Framework target의 `ZLINKbg/IO/1`은 13.42 µs/request이므로 선형 환산 시 1.015 core가 필요하다. 이 thread가 가장 먼저 1 core 한계를 넘는다. 같은 역할의 paced binding target `ZLINKbg/IO/1`은 7.67 µs/request, 0.441 core였다. 차이는 Framework가 5.75 µs/request 더 크며, Framework 값은 binding의 1.75배다.',
 '',
 'source 쪽도 목표 속도에서 여유가 작다. source `ZLINKbg/IO/2`는 Framework에서 13.02 µs/request이며, 75,645 req/s에서 0.985 core가 필요하다. paced binding의 같은 이름 thread는 6.43 µs/request와 0.370 core였다. source `ZLINKbg/IO/0`은 Framework 12.51 µs/request, binding 8.39 µs/request였다. Framework 제출 thread는 12.41 µs/request, binding 제출 thread는 8.19 µs/request였다. 따라서 Framework caller만을 병목으로 지목할 근거는 없다. target I/O thread 1과 source I/O thread 2가 더 높은 CPU 비용을 보였고 source mesh thread도 11.92 µs/request였다. 이 네 thread의 projected utilization은 목표 속도에서 약 0.90~1.02 core다.',
 '',
 '따라서 가장 가까운 단일 제한은 target `ZLINKbg/IO/1`의 I/O 진행이며, 전체적으로는 Framework target I/O와 source I/O 및 mesh 처리의 CPU 비용이 함께 목표 속도에 근접한다. 현재 57,550 req/s에서 75,645 req/s까지는 18,095 req/s, 31.4%가 더 필요하다. target I/O thread 1은 목표에서 1 core 예산을 약 0.20 µs/request 초과한다. 측정된 per-request 비용이 그대로 유지되면 target I/O thread만으로도 75,645 req/s를 처리할 수 없다.',
 '',
 '## 처리율과 in-flight',
 '',
 '최종 Framework 실행은 10초 active window에서 575,498건을 완료해 57,549.8 req/s였다. 평균 latency는 10.739 ms, p99는 17.935 ms, peak in-flight는 1,758이었다. Framework request-backpressure에는 application in-flight cap이 설정되어 있지 않았다. 그러므로 이 수치에서 `latency × in-flight cap`으로 처리율을 설명할 수 없다. 이 실행에서 한계로 드러난 것은 고정 cap에 따른 순차 handoff 지연이 아니라, target/source의 단일 I/O thread별 요청당 CPU 요구량이다.',
 '',
 'paced binding은 57,548.7 req/s로 Framework와 1.1 req/s 차이(0.002%)였다. 평균 latency는 0.101 ms, p99는 0.177 ms, peak in-flight는 451이었다. binding 무제한 실행은 128,818.8 req/s였고 평균 latency는 0.128 ms, p99는 1.130 ms, peak in-flight는 1,878이었다. 이번 unpaced binding의 0.6배는 77,291 req/s다. 본문에서 요청한 75,645 req/s 판정은 제공된 126,075 req/s baseline에 맞췄다.',
 '',
 '## Framework 코드 표본',
 '',
 'Framework 최종 실행의 source와 target JVM에 `settings=profile` JFR recording을 실행했다. source recording은 43초, `jdk.ExecutionSample`은 1,664건이었다. source `zlink-jvm-raw-mesh-bench` thread의 상위 Framework frame은 `ZLinkServiceOperationRegistry.take`(35 표본), `ZLinkJavaRawMeshNode.decodeRequestReply`(16 표본), `ZLinkServiceM6AWireCodec.decodeMultipartView`(18 표본)이었다. 확인 위치는 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationRegistry.java:516`, `.../binding/ZLinkJavaRawMeshNode.java:5293`, `.../internal/service/ZLinkServiceM6AWireCodec.java:534`다. 이 mesh thread는 11.92 µs/request와 0.686 core를 사용했다.',
 '',
 'target recording에서 unnamed virtual thread sample의 상위 Framework frame은 `ZLinkSpringHandlerFactory.constructHandler`(278 표본)이었다. 위치는 `framework/languages/java/zlink-framework-spring-boot-starter/src/main/java/systems/zlink/framework/spring/ZLinkSpringHandlerFactory.java:488`다. target `ZLINKbg/IO/1` 자체는 JFR Java execution sample에 나타나지 않았다. 따라서 이 Core I/O thread의 native poller 구간에 실행된 Framework Java method를 지정할 수는 없다. JFR의 Spring handler 표본은 target application 경로를 설명하지만 `ZLINKbg/IO/1`의 stack으로 해석하지 않았다.',
 '',
 '## 측정 조건과 방법',
 '',
 '세 최종 실행은 condition B로 target CPU 8–15, source CPU 0–7에 고정했고 `request-backpressure`, payload 1,024B, `RUNS=1`, 30초 warmup, 10초 active window로 실행했다. 각 최종 실행은 `/root/jobs/fx/cx/measure.lock`을 `flock -o`로 획득한 상태에서 진행했다. 빌드는 요청한 `/root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash java/build.sh`로 완료했다. paced binding 목표는 Framework 실측 처리율 57,550 req/s였다.',
 '',
 'thread별 CPU runtime과 run-queue wait는 active window 앞뒤의 `/proc/<pid>/task/<tid>/schedstat` 차이를 사용했다. context switch는 같은 시각의 thread `status` 파일에서 voluntary와 nonvoluntary 값을 읽어 계산했다. active window 경계와 가장 가까운 sample은 window 시작에서 최대 31.2 ms, 종료에서 최대 11.1 ms 떨어져 있었다. CPU cores는 schedstat runtime을 10초 window로 나눈 값이고 µs/request 및 context switch/request는 process별 완료 또는 target 수신 건수를 분모로 했다. `/proc`의 comm은 15글자 제한으로 잘릴 수 있어 역할 분류에 JFR의 전체 thread name을 함께 대조했다.',
 '',
 '초기 2초 warmup pilot과 두 번의 잘못된 pacing 실행은 최종 데이터에 포함하지 않았다. 첫 pacing 위치는 Java binding의 raw `runRaw` 경로에 적용되지 않아 117,974 req/s로 동작했다. 두 번째 실행에서는 `runRaw`로 위치를 옮겼지만 목표시각을 매회 재설정해 29,411 req/s에 그쳤다. 목표시각 누적을 적용한 최종 실행은 57,548.7 req/s로 목표와 0.002% 차이를 보였다.',
 '',
 '## 변경 파일과 검증',
 '',
 '`framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java`에서 `ZLINK_JAVA_BENCH_REQUEST_RATE`를 읽고 `zlink-java` raw request-backpressure submissions에만 pace를 적용했다. 기본값은 비활성이다. 기존 in-flight 동작은 유지했고 admission backpressure 외에 별도 cap을 추가하지 않았다.',
 '',
 '`framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchOptions.java`에서 환경 변수의 양의 정수 형식을 확인한다. 최종 변경 뒤 Java build가 성공했다. 별도 test는 실행하지 않았다. 직접 편집한 worktree 파일은 이 두 bench 파일이다.',
 '',
 '## 기준선 차이',
 '',
 '제시된 기준선은 Framework 63,500 req/s, binding 126,075 req/s였다. 최종 Framework 측정 57,550 req/s는 기준선보다 9.4% 낮았고, 최종 binding 무제한 측정 128,819 req/s는 기준선보다 2.2% 높았다. 각 조건은 RUNS=1이므로 이 차이를 코드 변화와 환경 변화로 분리할 수 없다. 0.6 목표 판정은 본문에서 제공된 126,075 req/s 기준을 유지했다.'
]
(base/'report.md').write_text('\n'.join(lines)+'\n')
print(base/'report.md', 'lines',len(lines),'bytes',(base/'report.md').stat().st_size,'thread_rows',sum(len(json.load(open(base/n/'thread-metrics.json'))['rows']) for n,_ in items))
