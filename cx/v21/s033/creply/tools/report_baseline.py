from pathlib import Path
import json,re
P=Path('/root/jobs/fx/cx/v21/s033/creply');p=P/'report.md';s=p.read_text();s=s[s.index('경로의 줄 번호'):s.index('패킷·CPU·mutex 표는 측정 대기 중이다.')]
t=json.loads((P/'tables.json').read_text())['same_rate']
intro='''C++에서도 Framework reply가 binding보다 더 많고 작은 TCP data packet으로 전송됐다. 40k req/s의 두 perf 구간에서 packet 수는 Framework 약 0.886/request, binding 0.683–0.693/request였다. 평균 data segment는 약 1,600 B와 1,808–1,834 B였다. Packet당 kernel 비용도 Framework가 더 높았다. Java의 packet 수만으로 거의 전부 설명되는 결과와 차이가 있다.

Framework mutex 경합도 관측했다. 유효 요청 1,200,000건 중 130,530건에서 trylock이 EBUSY였고, 뒤따르는 pthread_mutex_lock의 누적 경과 시간은 1,059,849,359 ns였다. 요청당 0.883208 µs, 경합 호출당 약 8.120 µs다. Mutex가 보호하는 C++ 객체 접근은 유지하고 native submit만 lock 밖으로 옮기는 조건은 성립했다. 현재 프로토타입 빌드와 close 검증을 기다리고 있으며, 아래에 아직 없는 처리량 수치를 추정하지 않았다.

'''
text='''
Source CPU 0–7, target CPU 8–15의 condition B다. Pattern은 C++의 기존 request-backpressure이고 실제 application window는 0, 즉 무제한이다. Trigger JSON의 requestWindow 100은 C++ trigger 계약의 고정값이며 실제 window는 CLI pattern이 결정한다. Payload는 1,024 B, sendConcurrency는 8이다. Warmup 30초, active 40초 안에서 perf 25초와 겹치는 ss 카운터를 사용했다. 사전 실행의 실제 처리율은 Framework 39,995.984, binding 39,999.584 req/s였다.

| 40k perf 실행 | 실제 perf 구간 req/s | reply data packet/request | 평균 data segment B | reply 방향 pure ACK/request | tcp_sendmsg 이하 cycles/request | cycles/data packet |
|---|---:|---:|---:|---:|---:|---:|
'''
for case in ('main-perf-1','binding-perf-1','main-perf-2','binding-perf-2'):
 r=t[case];text+=f"| {case} | {r['rate']:,.3f} | {r['packets_per_request']:.6f} | {r['average_data_segment_bytes']:,.3f} | {r['reply_ACKs_per_request']:.6f} | {r['send_cycles_per_request']:,.0f} | {r['send_cycles_per_packet']:,.0f} |\n"
text+='\nReply TCP payload는 Framework 약 1,417 B/request, binding 약 1,252 B/request다. TCP/IP header는 제외한다. Kernel cycles는 Java 수집기와 동일하게 thread별 cycles:k counter에 tcp_sendmsg stack 표본 period 비율을 적용한 추정치다. 표본은 249 Hz다. 다른 함수의 inclusive 비용을 합산하지 않았다. Packet과 ACK는 perf 안에 들어가는 ss 구간의 완료 요청 수로 나눴다. ACK는 reply sender에 들어온 반대 방향 pure ACK다.\n\n'
for i in (1,2):
 f,b=t[f'main-perf-{i}'],t[f'binding-perf-{i}'];gap=f['send_cycles_per_request']-b['send_cycles_per_request'];explained=(f['packets_per_request']-b['packets_per_request'])*b['send_cycles_per_packet'];text+=f'{i}회에서 binding의 packet당 비용을 고정하면 packet 증가가 추가 {gap:,.0f} cycles/request 중 {explained:,.0f} cycles/request를 설명한다. '
text+='이 분해의 범위는 target의 tcp_sendmsg 이하 비용이다.\n\n'
text+='''| 별도 BPF 구간 | target reply sendto/request | 반환 B/성공 call | target reply I/O wakeup/request | Native reply producer TID |
|---|---:|---:|---:|---|
| Framework | 0.853941 | 1,662.385 | 0.457380 | 2010860, 2011025, 2011149 |
| binding | 0.937249 | 1,337.689 | 0.709926 | 2011853 |

Framework의 실제 sendto는 target I/O TID 2009759, binding은 2011850에서 관측했다. 둘 다 comm은 ZLINKbg/IO/1이다. Producer와 실제 TCP 송신은 별도 thread다. Syscall은 target의 모든 thread를 대상으로 수집했고 표는 reply data FD를 골랐다. 해당 FD의 short send와 EAGAIN은 두 조건 모두 0이었다. Native reply 호출 경계에서 producer TID를 계수했다. 각 소스 단계의 OS thread를 메시지별로 연결하는 추적은 하지 않았다. 동기 echo의 handler completion과 reply 제출이 같은 worker에서 수행되는 것은 위 소스 경로로 확인했다.

BPF 구간의 묶음 전송과 wakeup은 perf 구간과 다르다. 따라서 위 sendto 수나 wakeup 수를 perf 비용과 곱해 원인을 계산하지 않았다. Packet/CPU 표는 같은 perf 구간 안의 ss만 사용한다. 추가 native 제출 직렬화를 없앤 뒤에도 application dispatch와 wire framing 비용은 남는다.

Mutex는 reply 함수의 직접 pthread_mutex_lock call site만 선택했다. LD_PRELOAD 계측기는 trylock 성공 호출을 대기 0으로 처리하고, EBUSY 호출만 실제 lock 경과 시간을 잰다. Active 직전·직후 /proc/PID/mem으로 카운터를 읽어 warmup을 제외했다. Calls 1,200,000건과 active 완료 요청 수가 정확히 일치했다. 이 값은 계측된 wall wait이며 CPU 시간과 구분한다. Clock과 trylock 계측의 영향은 포함된다. 정상 100회, 의도한 50 ms 경합 1회, 범위 밖 호출 제외를 모두 검사했다(tools/mutex-validation.json).

확인한 중복 작업은 다음과 같다. 현재 runtime 프로토타입은 아직 적용되지 않았다.

| 중복 작업 | 파일:줄 | 상태 |
|---|---|---|
| Native record 제출을 Framework mutex와 Core admission이 각각 직렬화 | raw_route_port.cpp:506, Core socket_request_reply_submit_api.cpp:495 | submit의 Framework lock 범위를 제거할 예정 |
| Router reply factory와 terminal이 같은 token owner를 확인 | bindings/cpp/src/Runtime/Sockets/router.cpp:114, bindings/cpp/src/Runtime/Messaging/reply_operations.cpp:24 | binding 수정 금지 범위이므로 유지 |
| Protobuf native buffer에 쓴 body를 service wire buffer에 다시 복사 | framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:30, runtime/protocol/service_wire_codec.cpp:3201 | wire·codec 변경 없이 유지. Binding bench도 string 직렬화 뒤 native message 복사가 있다(cpp/zlink/bench_zlink_cpp_server.cpp:63–65). |
| Source RID를 binding 값으로 만들고 operation 값으로 다시 복사 | raw_route_port.cpp:510, bindings/cpp/src/Runtime/Sockets/router.cpp:118 | 새 cache나 상태를 추가하지 않고 유지 |

현재 남은 일은 프로토타입 close 경합 검사, RUNS=5 처리량 비교, 동일 요청률 재계측이다. 최초 빌드는 MemAvailable 11.916 GiB를 관측한 즉시 SIGTERM으로 중단했다. 재개는 -j1과 13 GiB 중단 여유로 수행했다. 첫 trigger 사전 실행은 requestWindow 값이 잘못돼 HTTP 400으로 끝났으며 보존했다. Close fixture의 최초 컴파일은 errno getter 선언 오류로 중단됐고 수정했다. 이 실행들에서 성능 수치를 가져오지 않았다.
'''
p.write_text(intro+s+text)
