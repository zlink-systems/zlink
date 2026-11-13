# #3868 C++ Framework / binding Core 송수신 커널 비용 진단

## 1. 방법과 처리율

고정 바이너리 `/root/jobs/fx/cx/v21/s033/csendb/build-main`과 고정 소스 `csendb/main-source`로 send-saturation, 1 KiB, condition B를 실행했다. `CSEND_PACE=136889`, source CPU 0–7, target CPU 8–15로 맞추고 각 조합 3회 측정했다. 각 측정은 warmup 후 25초 active 구간이었으며 모든 측정 구간은 `/root/jobs/fx/cx/measure.lock`을 exclusive flock으로 획득한 동안만 실행했다. 측정 직전 MemAvailable은 12 GiB보다 컸다.

Framework 세 처리율은 136,888.88–136,888.99 msg/s, binding은 136,888.70–136,890.43 msg/s였다. 표는 세 회차의 msg당 값 중앙값이다. syscall 횟수와 task-clock은 프로세스에 붙인 `perf stat`으로 측정했고, context switch는 프로세스 전체와 각 TID의 `/proc/.../status` 변화량, wakeup은 `sched:sched_wakeup`의 wakee TID를 수집했다. TCP `InSegs/OutSegs`는 `/proc/net/snmp` 시스템 전체 delta다.

`perf trace -s`는 개별 syscall의 호출 경과시간을 확인하는 보조 측정으로 별도 실행했다. 이벤트 손실이 프로세스별 최대 59.9–98.8%여서 이 자료의 syscall 호출 횟수는 버리고, 정확한 per-thread syscall 집계에도 사용하지 않았다. 따라서 syscall 유형별 수치는 프로세스 합계이며, per-thread 수치는 context switch와 wakeup에 한정된다. `perf trace`의 경과시간은 커널 CPU 시간이 아니다. send syscall return bytes 합계도 trace 손실 때문에 채택하지 않았다. Framework source trace는 perf stat에서 예상되는 호출 수보다 훨씬 적은 93,980건만 남았고, binding trace도 52,379건만 남아 두 값 모두 bytes/send 또는 bytes/message 산출에 사용할 수 없다.

## 2. msg당 계측 비교

| 프로세스·방향 | syscalls: sendto | recvfrom | epoll_wait | futex | read | write | poll | context switch (perf) | TID별 voluntary / involuntary | sched_wakeup | TCP segment | task-clock |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Framework source | 0.342 | 0.000 | 0.358 | 0.534 | 0.000 | 0.0002 | 0.000 | 0.045 | 0.0313 / 0.0001 | 0.0291 | Out 0.500 | 8.970 µs |
| binding source | 0.079 | 0.000 | 0.096 | 0.691 | 0.000 | 0.0001 | 0.000 | 0.040 | 0.0247 / 0.0001 | 0.0233 | Out 0.120 | 2.604 µs |
| Framework target | 0.000 | 0.370 | 0.261 | 0.246 | 0.351 | 0.085 | 0.116 | 0.206 | 0.1830 / 0.0002 | 0.1932 | In 0.500 | 12.519 µs |
| binding target | 0.000 | 0.195 | 0.157 | 0.116 | 0.000 | 0.000 | 0.000 | 0.078 | 0.0786 / 0.0001 | 0.0756 | In 0.120 | 2.067 µs |

`sendmsg`, `recvmsg`, `eventfd2`, `clock_gettime`은 네 프로세스 모두 0.0002회/msg 이하였고 `epoll_pwait`와 `ppoll`은 0회였다. Framework 수신 프로세스의 `read`, `write`, `poll`은 각각 0.351, 0.085, 0.116회/msg 증가했다. source의 `futex`는 Framework가 오히려 0.157회/msg 적었다. 프로세스당 TID 수는 Framework source/target 48/38, binding source/target 42/42였다. Framework target에서는 active 측정 동안 새 TID 하나가 생겼다.

wakee 기준 상위 TID는 Framework target의 `ZLINKbg/IO/0` 0.101회/msg와 Framework 처리 thread 0.076회/msg, binding target의 같은 이름 IO thread 0.036회/msg와 `bench_cpp_zlink` thread 0.032회/msg였다. source는 Framework `ZLINKbg/IO/0` 0.014회/msg, binding `ZLINKbg/IO/0` 0.015회/msg로 IO thread wake 횟수 차이가 거의 없었다. context switch의 TID별 합계는 프로세스 내 모든 TID의 voluntary/involuntary 변화를 더한 값이다. 아래는 run 1의 대표 thread 두 개씩이며 TID는 해당 실행에서만 유효하다.

| 프로세스·방향 | thread (TID) | voluntary switch/msg | involuntary switch/msg | wakeup/msg |
|---|---|---:|---:|---:|
| Framework source | ZLINKbg/IO/0 (1369110) | 0.01472 | 0.00007 | 0.01424 |
| Framework source | bench_cpp_clien (1369079) | 0.00747 | 0.00000 | 0.00672 |
| binding source | ZLINKbg/IO/0 (1372163) | 0.01570 | 0.00008 | 0.01526 |
| binding source | bench_cpp_clien (1372133) | 0.00758 | 0.00000 | 0.00680 |
| Framework target | ZLINKbg/IO/0 (1369051) | 0.10945 | 0.00009 | 0.10766 |
| Framework target | bench_cpp_frame (1369055) | 0.07987 | 0.00016 | 0.07788 |
| binding target | ZLINKbg/IO/0 (1372117) | 0.03640 | 0.00004 | 0.03560 |
| binding target | bench_cpp_zlink (1372122) | 0.03281 | 0.00009 | 0.03175 |


source 프로세스에서 Framework는 binding보다 `sendto`가 0.263회/msg 많다. 기존 csprof2 kernel CPU 값의 source 차이 4.687−1.017 = 3.670 µs/msg를 이 차이로 나누면 추가 호출 하나당 13.98 µs kernel CPU가 필요하다는 역산값이다. 별도 5초 `perf trace`에서 Framework source `sendto`의 평균 경과시간은 19.44 µs였지만 이벤트 손실이 커서 이를 CPU 비용으로 등치할 수 없다. 따라서 호출 수 증가는 kernel gap과 크기상 양립하지만 이 자료만으로 추가 호출 하나의 kernel CPU 비용을 직접 측정했다고 할 수 없다.

target의 추가 `recvfrom` 0.174/msg, `epoll_wait` 0.104/msg, `futex` 0.130/msg, `read` 0.351/msg, `write` 0.085/msg, `poll` 0.116/msg를 합하면 0.961회/msg다. 기존 csprof2 target kernel CPU 차이 4.341−1.020 = 3.321 µs/msg를 나누면 추가 syscall 하나당 평균 3.46 µs가 필요하다. 이 또한 syscall별 CPU 측정값이 아니라 gap과 호출 수의 일치 여부를 보는 역산이다. blocking 및 대기 syscall 경과시간을 kernel CPU로 간주하지 않았다.

TCP segment 비율은 시스템 전체 카운터에서 Framework 약 0.500 segment/msg, binding 약 0.120 segment/msg로 4.2배 차이가 관찰됐다. 이 수치는 per-connection 계측이 아니므로 해당 연결이 만든 segment 수로 단정하지 않는다. 송신 `sendto` 증가와 방향별 segment 증가가 함께 보여 syscall/전송 묶음 크기 차이를 의심하게 하지만, 개별 TCP 연결의 계측이 없어 segment 차이를 원인으로 확정할 수는 없다.

## 3. context·socket·제출 경로 대조

| 항목 | C++ Framework | C++ binding 벤치 |
|---|---|---|
| context | App당 공유 `zlink::context_t` 생성. IO_THREADS는 따로 설정하지 않아 Core 기본 4. Auto HWM을 `true`, profile을 Framework 기본 `balanced`로 설정. budget과 memory limit은 기본 0이어서 설정 호출을 생략한다. `framework/languages/cpp/framework/src/runtime/host/app.cpp:1614-1625`, 기본값 `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/detail/framework_options_state.hpp:439-441`. | 기본 `zlink::context_t` 하나, 옵션 변경 없음. client: `framework/bench/grpc/cpp/client/bench_cpp_client.cpp:785`; server: `framework/bench/grpc/cpp/zlink/bench_zlink_cpp_server.cpp:133`. Core 기본 IO_THREADS=4, Auto HWM enabled, BALANCED profile을 그대로 사용한다. `core/doc/spec/core/01-context.ko.md:89-103,121-130`. |
| socket과 연결 | 프로세스마다 Route Mesh ROUTER 하나. target mesh ROUTER socket은 HANDOVER=true, MANDATORY=true, LINGER=0이며 mesh RID를 설정한다. bind/connect는 각 프로세스의 역할에 따른다. `raw_mesh_node_owner.cpp:515-522,531`. source도 mesh ROUTER 하나로 endpoint에 bind하고 peer에 connect한다. `bench_framework_cpp_server.cpp:63-68`, `client/bench_cpp_client.cpp:821-825`. 두 프로세스 사이 활성 TCP 연결은 하나다. | source ROUTER 하나가 target에 connect하고 RID를 지정한다. `client/bench_cpp_client.cpp:443-455`. target은 request와 command ROUTER 두 개를 만들고 둘 다 RID 지정 및 bind하며 두 loop thread를 둔다. send-saturation에서 command socket/loop만 사용하므로 활성 TCP 연결은 하나이고 request socket/loop는 유휴 상태다. `zlink/bench_zlink_cpp_server.cpp:133-145`. |
| socket 옵션 | HWM, IMMEDIATE, TCP_NODELAY, TCP keepalive 등은 벤치에서 별도 설정하지 않는다. 명시 옵션 중 LINGER=0, HANDOVER, MANDATORY가 binding과 다른 점이다. `raw_mesh_node_owner.cpp:516-520`. C++ binding의 `async()`는 Core DONTWAIT completion 계약을 쓰며 binding은 별도 payload 재전송 queue를 두지 않는다. `bindings/doc/spec/cpp/README.ko.md:377-385,556-560`. | HWM, LINGER, IMMEDIATE, TCP_NODELAY, TCP keepalive, HANDOVER, MANDATORY에 대한 별도 설정이 없다. | 
| wire와 제출 | `send_to_node(...).async()`로 msg당 한 번 제출한다. `client/bench_cpp_client.cpp:889-896`. payload에는 header와 application payload 2 parts가 있으며 metadata는 없다. `raw_mesh_node_owner.cpp:1754-1777`. backend가 parts를 한 async Core send operation에 넣는다. `raw_route_port.cpp:181-200`. | msg당 한 번 `send_operation().message(part1).message(part2).async()`로 제출한다. `client/bench_cpp_client.cpp:726-746`; 두 part는 JSON envelope와 payload다. `client/bench_cpp_client.cpp:434,612-625`. 명시적 DONTWAIT나 per-message flush는 없다. |
| pacing·응답 | 64개 제출마다 rate schedule의 `sleep_until`을 호출한다. `client/bench_cpp_client.cpp:940-947`. send-saturation은 one-way이며 application reply를 요구하지 않는다. | 동일하게 64개마다 pacing sleep. `client/bench_cpp_client.cpp:731-734`. command loop는 blocking `recv(..., none)`이며 reply를 보내지 않는다. `zlink/bench_zlink_cpp_server.cpp:80-100`. |

Core 기본값 문서에서 LINGER 기본은 -1, SNDHWM/RCVHWM은 각각 4,096,000 accounted bytes, IMMEDIATE는 0, TCP_NODELAY는 1이다. `core/doc/spec/core/socket/README.ko.md:359-375,393`. Framework LINGER=0 차이는 종료 처리에 관한 값이다. 현재 steady-state syscall/wakeup 격차의 설명으로는 맞지 않는다. 자동 HWM은 양쪽 모두 기본 enabled/BALANCED이므로 HWM 설정 차이도 확인되지 않았다. 이 경로에서 각 data message는 양쪽 모두 2 parts, async 제출 1회이며 애플리케이션 제어 메시지를 data message마다 추가 제출하는 코드는 확인되지 않았다. 별도 mesh discovery/liveness 제어 작업 가능성은 있지만 msg당 추가 제어 송신으로 계측되지 않았다.

## 4. 원인 해석과 수치

Framework source는 `send_to_node`에서 Route Mesh owner, `raw_route_port`를 거쳐 동일한 Core async multipart send를 제출한다. 코드상 msg당 두 parts를 한 번의 Core send operation에 담으므로 애플리케이션 수준의 중복 send는 보이지 않는다. 그럼에도 Framework source의 `sendto` 횟수가 binding의 4.32배이고, 주어진 kernel CPU 격차는 3.670 µs/msg다. 이는 Framework 실행 경로에서 Core send 완료까지의 묶음 또는 전달 cadence가 binding 직접 호출과 다르게 작동한다는 강한 계측 증거다. 다만 실제 syscall CPU 시간을 호출별로 측정하지 못했고, raw socket 옵션의 steady-state 차이도 없어서 특정 batching 결정이 이 차이를 만들었다고 확정할 수는 없다. 후보 Framework 경로는 `raw_mesh_node_owner_t::send_with_header_result`와 `raw_route_port_t::submit_send_impl`이다.

Framework target 수신은 Core ROUTER에 poller를 연결하고 `raw_route_port_t::poll()`에서 wait한 뒤 `receive_if_ready()`가 `recv(...dontwait)`를 수행한다. `raw_mesh_node_owner.cpp:541-548`, `raw_route_port.cpp:404-451,459-475`. binding target은 전용 command loop에서 blocking `recv(...none)`를 반복한다. 이 구조 차이는 Framework target의 추가 `epoll_wait` 0.104, `recvfrom` 0.174, `read` 0.351, `write` 0.085, `poll` 0.116회/msg와 wakeup 0.118회/msg 증가에 대응한다. 수신 poller/IO/Framework 처리 thread 간 전달에서 발생하는 추가 scheduling과 syscall 수는 코드 구조로 설명되며, target kernel CPU 격차 3.321 µs/msg에 포함되는 유력 원인이다. 하지만 개별 syscall kernel CPU 계측이 없으므로 syscall별 µs 기여량은 확정할 수 없다.

Framework target socket의 `pollin|pollroute|pollconnect`와 `pollout|pollcompletion` 등록은 `raw_route_port`가 route/completion 이벤트도 같은 poller에서 처리하게 한다. `raw_mesh_node_owner.cpp:541-548`, `raw_route_port.cpp:410-420`. 하지만 측정만으로 어느 이벤트 종류가 초과 wakeup 중 몇 개를 만들었는지는 분리할 수 없다. raw binding target에도 IO thread와 poller가 존재하므로 단순히 “poller 유무”가 아니라 Framework의 mesh ingress 및 completion 처리 경로가 추가된 이유다.

비교에서 syscall/wakeup 차이는 수치로 관찰됐고 코드의 실행 구조와 방향도 맞는다. kernel CPU 차이를 측정된 syscall별 CPU 시간으로 산출하라는 요구는 현재 계측으로 완결할 수 없다. `perf stat` syscall tracepoint는 호출 횟수만 제공했고 `perf trace -s`는 이벤트 손실과 wall duration만 제공했다. 따라서 위 13.98 µs/sendto와 3.46 µs/추가 syscall은 kernel gap으로부터의 역산이며 실측 syscall CPU 비용이 아니다. 별도로 주어진 perf call chain의 `tcp_sendmsg → lock_sock_nested → spin lock slowpath`는 Framework 송신에서 lock 경쟁이 실제 비용 경로임을 가리키지만, 이 실행에서 lock 대기 CPU 시간을 호출별로 다시 계측하지는 않았다.

## 5. 중복 작업 점검

- 관찰된 초과 작업: Framework source `sendto` +0.263회/msg, target `recvfrom` +0.174, `epoll_wait` +0.104, `read` +0.351, `write` +0.085, `poll` +0.116, target wakeup +0.118회/msg. 관련 Framework 경로는 `raw_route_port.cpp:404-451,459-475`와 `raw_mesh_node_owner.cpp:541-548`다.
- source wakeup은 +0.006회/msg로 작고, target wakeup의 대부분은 Framework IO thread와 Framework 처리 thread에서 관찰됐다. wakeup이 같은 이벤트의 중복 통지인지, 서로 다른 필수 상태 전달인지는 현재 trace만으로 판별되지 않는다.
- 사용자 data message의 중복 송신, per-message flush, 추가 command/control frame은 제출 코드에서 발견되지 않았다. 양쪽 모두 2 parts와 async submit 1회를 사용한다. Framework target의 추가 등록 이벤트와 poller 경로가 제어 진행/완료 관찰을 위한 추가 작업으로 보인다.
- TCP segment delta는 시스템 전역이라 연결에 귀속할 수 없다. syscall 추적의 높은 손실률로 per-thread syscall 분포도 결론에 사용하지 않았다.
