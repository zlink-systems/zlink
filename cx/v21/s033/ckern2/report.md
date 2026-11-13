# #3868 controlled A/B: producer cadence와 Core poller 수신

이 문서는 측정 결과만 기록한다. 제품 소스와 작업 트리는 바꾸지 않았다. 원본 벤치 소스는 `/root/jobs/fx/cx/v21/s033/csendb/main-source`에서 ckern2로 복사했고, 바이너리 기준은 `csendb/build-main`을 사용했다.

## 실행과 해시

클라이언트 `send_slot`은 성공한 제출 뒤 `CKERN_SPIN_NS`만큼 `steady_clock` busy-spin한다. sleep이나 별도 syscall은 쓰지 않으며, 변수가 없으면 기존 동작이다. zlink 서버 command loop는 `CKERN_RECV_MODE=poller`일 때 ROUTER에 `pollin` poller를 붙이고, readiness 뒤 `recv(dontwait)`를 데이터가 없을 때까지 반복한다. 기본값은 기존 blocking `recv(none)`이다.

두 벤치 타깃은 `/root/jobs/fx/cx/cpp-build-slot.sh`로 빌드했다. 병렬 3 빌드는 MemAvailable이 11.47 GiB로 내려가 SIGINT로 중단했고, 남은 자식에는 SIGTERM을 보내 종료했다. 최종 빌드는 병렬 1로 완료했다. scratch CMakeLists에는 Framework 의존성 소스를 원본 `csendb/main-source` 경로에서 읽도록 하는 빌드 전용 경로 설정을 넣었다. Core 및 C++ binding 패키지 경로는 `build-main`과 동일하며, `libzlink.so.0`도 두 빌드 모두 Core 1.23.0의 같은 설치 경로에서 로드된다.

측정은 CSEND_PACE=136889, source CPU 0–7, target CPU 8–15로 실행했다. 각 회차는 30초 warmup과 25초 active였고, 5개 셀을 3회씩 측정했다. 각 회차마다 시작 전 1분 부하가 4를 넘으면 lock 밖에서 기다린 뒤 `flock -o /root/jobs/fx/cx/measure.lock`을 잡았고, `uptime-before.txt`를 남겼다. active 구간의 MemAvailable 최솟값은 25.91 GiB였다. bench JSON의 실제 active 길이는 25.000초였고, `/proc` 및 perf 포착 구간은 phase 확인 요청 때문에 약 25.18초였다.

클라이언트 벤치 소스 SHA-256은 `b03bdd708bd3e7eef98dc0101abfe860044944e40e71a02196ce7792e3f2753a`이고, zlink 서버 벤치 소스 SHA-256은 `c3b47d577710808f721c3ef92e9e579ccb9db58aa0e62099935fa980078d05c7`이다. scratch CMakeLists SHA-256은 `f9b34476b653eb3758b0bbefe3af15676148a9e8ba359246c401f11dc4ffe79`이다.

빌드한 client SHA-256은 `ab53901847f1f38cdae61d2d1ba9f610a722ca03f5a217e5c37ea2dce80dadb3`이고, zlink server SHA-256은 `9bb5651f0bd69585d6426c26d3f0f4c8c0cf11e373de8e67975570315fc77c1c`이다. Framework same-session 기준은 `csendb/build-main`의 client `2f38fe55db3a39a41f74a071b9cca8537d598ae8d93691e0d9c5f83c3a7d1016`, Framework server `57446aaefb0bdfe0a4318093ecdf26d7c0e65b8a659c80c5274e747b813ab8b0`이다.

## 셀별 syscall 횟수

모든 수치는 활성 메시지 1개당 횟수이며 각 셀 3회의 중앙값이다. perf stat 결과는 프로세스에 속한 TID 행을 합산했다. 0.0001 미만은 0.0000으로 표시했다.

| 셀 | 프로세스 | sendmsg | sendto | sendmmsg | recvmsg | recvfrom | recvmmsg | epoll_wait | epoll_pwait |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| binding 0 ns / blocking | source | 0.0001 | 0.0839 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.1005 | 0.0000 |
| binding 0 ns / blocking | target | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.1837 | 0.0000 | 0.1601 | 0.0000 |
| binding 3000 ns / blocking | source | 0.0001 | 0.4235 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.4401 | 0.0000 |
| binding 3000 ns / blocking | target | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.4684 | 0.0000 | 0.3201 | 0.0000 |
| binding 6000 ns / blocking | source | 0.0001 | 0.7099 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.7110 | 0.0000 |
| binding 6000 ns / blocking | target | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.7591 | 0.0000 | 0.5184 | 0.0000 |
| binding 0 ns / poller | source | 0.0001 | 0.0825 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.0992 | 0.0000 |
| binding 0 ns / poller | target | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.1826 | 0.0000 | 0.1595 | 0.0000 |
| Framework / reference | source | 0.0001 | 0.3265 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.3425 | 0.0000 |
| Framework / reference | target | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.3823 | 0.0000 | 0.2732 | 0.0000 |

| 셀 | 프로세스 | futex | eventfd | eventfd2 | read | write | poll | ppoll | clock_gettime |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| binding 0 ns / blocking | source | 0.7296 | 0.0000 | 0.0001 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.0000 |
| binding 0 ns / blocking | target | 0.1241 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| binding 3000 ns / blocking | source | 0.2660 | 0.0000 | 0.0001 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.0000 |
| binding 3000 ns / blocking | target | 0.5101 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| binding 6000 ns / blocking | source | 0.1774 | 0.0000 | 0.0001 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.0000 |
| binding 6000 ns / blocking | target | 0.9036 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| binding 0 ns / poller | source | 0.7531 | 0.0000 | 0.0001 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.0000 |
| binding 0 ns / poller | target | 0.0167 | 0.0000 | 0.0000 | 0.1006 | 0.0349 | 0.0304 | 0.0000 | 0.0000 |
| Framework / reference | source | 0.5249 | 0.0000 | 0.0001 | 0.0000 | 0.0001 | 0.0000 | 0.0000 | 0.0000 |
| Framework / reference | target | 0.2744 | 0.0000 | 0.0000 | 0.3487 | 0.0865 | 0.1148 | 0.0000 | 0.0000 |

## CPU, scheduling, bytes, rate

`user`와 `kernel`은 `/proc/<pid>/stat`의 utime/stime 차이를 메시지 수로 나눈 값이다. task-clock과 context-switch는 perf stat, voluntary/involuntary switch는 측정 전후 모든 TID의 `/proc/<tid>/status`, wakeup은 `sched:sched_wakeup` tracepoint를 wakee TID별로 집계했다. bytes/sendto는 BPF에서 성공한 `sys_exit_sendto` 반환 byte 합계를 성공 호출 수로 나눈 값이다. /proc io wchar와 TCP OutSegs는 bytes/sendto 계산에 사용하지 않았다. source·target 각각 실제 완료·수신 건수로 나눴다.

| 셀 | 프로세스 | 메시지 수 | bench msg/s | user µs/msg | kernel µs/msg | task-clock µs/msg | context switch/msg | vol/invol switch/msg | wakeup/msg | bytes/sendto | raw |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| binding 0 ns / blocking | source | 3420536 | 136890.5 | 1.520 | 1.275 | 2.730 | 0.0394 | 0.0236 / 0.0001 | 0.0394 | 14829.0 | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/binding-spin0-blocking/run1/results.json) |
| binding 0 ns / blocking | target | 3420472 | 136890.5 | 1.234 | 1.131 | 2.253 | 0.0659 | 0.0659 / 0.0001 | 0.0662 | — | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/binding-spin0-blocking/run1/results.json) |
| binding 3000 ns / blocking | source | 3420457 | 136888.9 | 5.201 | 4.038 | 9.182 | 0.0394 | 0.0236 / 0.0001 | 0.0393 | 2937.3 | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/binding-spin3000-blocking/run1/results.json) |
| binding 3000 ns / blocking | target | 3420368 | 136888.9 | 1.857 | 4.187 | 5.726 | 0.2904 | 0.2903 / 0.0002 | 0.2922 | — | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/binding-spin3000-blocking/run1/results.json) |
| binding 6000 ns / blocking | source | 3309408 | 132432.8 | 8.654 | 6.560 | 15.190 | 0.0084 | 0.0084 / 0.0000 | 0.0084 | 1752.0 | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/binding-spin6000-blocking/run1/results.json) |
| binding 6000 ns / blocking | target | 3309363 | 132432.8 | 2.445 | 7.578 | 9.505 | 0.4800 | 0.4802 / 0.0001 | 0.4887 | — | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/binding-spin6000-blocking/run1/results.json) |
| binding 0 ns / poller | source | 3420535 | 136890.7 | 1.541 | 1.272 | 2.736 | 0.0394 | 0.0237 / 0.0001 | 0.0394 | 15074.7 | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/binding-spin0-poller/run1/results.json) |
| binding 0 ns / poller | target | 3420471 | 136890.7 | 1.245 | 1.120 | 2.292 | 0.0623 | 0.0623 / 0.0001 | 0.0629 | — | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/binding-spin0-poller/run1/results.json) |
| Framework / reference | source | 3420209 | 136889.0 | 4.912 | 3.933 | 8.753 | 0.0447 | 0.0298 / 0.0001 | 0.0447 | 4253.6 | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/framework-reference/run1/results.json) |
| Framework / reference | target | 3420142 | 136889.0 | 8.724 | 4.085 | 12.498 | 0.2064 | 0.1970 / 0.0003 | 0.2056 | — | [run1](/root/jobs/fx/cx/v21/s033/ckern2/raw/framework-reference/run1/results.json) |

표의 raw 링크는 해당 셀 run1의 bench JSON이다. 같은 디렉터리의 run2와 run3에도 `results.json`, `before.json`, `after.json`, perf CSV, BPF 결과, command metadata, uptime이 보존돼 있다. 집계는 `scripts/recompute.py`로 raw에서 다시 만들 수 있고, 계산된 표 자료는 `summary.json`에 있다.

## 판정

### H1: source producer cadence

같은 세션에서 binding 0 ns는 sendto 0.0839회/msg, kernel 1.275 µs/msg였고 Framework는 0.3265회/msg, 3.933 µs/msg였다. Framework의 user CPU는 4.912 µs/msg, binding 0 ns는 1.520 µs/msg로 같은 세션에서 user gap은 3.392 µs/msg였다.

요청한 6000 ns spin은 Framework 값에 가까워지지 않았다. binding은 sendto 0.7099회/msg와 1752 bytes/sendto, kernel 6.560 µs/msg까지 올라갔다. Framework와 비교하면 sendto가 0.3835회/msg 더 많고 kernel CPU는 2.627 µs/msg 더 높다. user CPU도 binding 8.654, Framework 4.912 µs/msg로 binding 쪽이 3.742 µs/msg 높았다. 실제 처리율은 binding 132,433 msg/s로 목표 136,889에 미달했다.

3000 ns가 비교상 가장 가까웠다. user CPU 5.201 µs/msg는 Framework 4.912와 0.289 차이이고 kernel CPU 4.038은 Framework보다 0.105 µs/msg 높다. 따라서 Framework minus binding kernel gap은 −0.105 µs/msg로 거의 닫혔다. 다만 sendto는 0.4235회/msg로 Framework보다 0.0970회/msg 많았다. 이 결과는 cadence가 source kernel gap 대부분을 만들 수 있다는 쪽을 지지하지만, 6000 ns는 그 효과를 지나쳐 과도하게 syscall을 쪼갰다.

cbatch(#3919)의 “batching follows submit cadence”와 binding의 방향·규모는 대체로 일치한다. 여기서 binding의 bytes/sendto와 bytes/message 비율로 환산한 평균은 0 ns 약 11.9 messages/sendto, 3000 ns 약 2.36, 6000 ns 약 1.41이다. cbatch의 uniform limiter 결과는 180k msg/s에서 binding 13.16에서 2.71 messages/sendto로 내려갔고 Framework는 4.43이었다. 0 ns와 3000 ns의 추세는 가깝다. 반면 이번 Framework은 약 3.06 messages/sendto로 cbatch의 4.43보다 낮아 정확한 값은 일치하지 않는다. 이번 측정은 136,889 pace와 64-message burst pacing이고 cbatch 값은 180k uniform busy-wait 조건이므로 수치를 등치하지 않았다.

### H2: target poller + dontwait

poller 수신 binding은 blocking binding보다 read 0.1006, write 0.0349, poll 0.0304회/msg를 추가했지만 recvfrom은 0.1826 대 0.1837, epoll_wait은 0.1595 대 0.1601로 거의 같았다. Framework target은 recvfrom 0.3823, epoll_wait 0.2732, read 0.3487, write 0.0865, poll 0.1148회/msg였다. 따라서 poller + dontwait만으로 Framework의 syscall 증가는 재현되지 않았다.

Framework target kernel CPU는 4.085 µs/msg, binding poller target은 1.120 µs/msg였다. Framework minus binding poller 잔여 kernel gap은 2.965 µs/msg다. poller 존재 여부만으로 target의 추가 비용을 설명하지 못하며 H2는 지지되지 않는다. 이 벤치 poller loop는 한 번 깨어난 뒤 recv(dontwait)로 가능한 메시지를 모두 drain한다. 현재 Framework `receive_if_ready`는 receive turn마다 한 번의 recv(dontwait)를 호출하므로 둘의 drain cadence는 완전히 같지 않다.

## Framework receive 경로 handoff 점검

같은 메시지를 중복 enqueue하거나 같은 socket readiness에 중복 wake를 거는 동작은 코드에서 확인하지 못했다. `raw_route_port_t::poll`은 poller 이벤트들을 하나의 readiness 결과로 합치고, `receive_if_ready`는 한 번의 `recv(dontwait)`를 수행한다 (`raw_route_port.cpp:404-451,459-487`). `raw_mesh_node_owner_t::receive_next`가 받은 record를 `_mailbox.try_enqueue`로 한 번 넘긴다 (`raw_mesh_node_owner.cpp:3614,3286-3353`). 이 경로에는 IO/poller 수신에서 mailbox로 넘어가는 두 단계 handoff가 있지만 같은 handoff의 중복은 아니다. 별도 `signal_activity`는 shared poller의 one-shot timer를 울리는 작업용 wake path다 (`raw_mesh_node_owner.cpp:4302-4313`, `runtime_wake_timer.hpp:41-50`).

## 남은 제한

`/proc/net/snmp`와 각 프로세스 `/proc/<pid>/io`도 run 전후 원본을 저장했다. syscall 유형은 perf stat, wakeup TID는 BPF tracepoint 집계를 사용했다. 통계 HTTP 요청은 측정 중 100 ms 간격으로 상태를 확인하므로 해당 control traffic도 source/target 프로세스 syscall과 CPU에 아주 소량 포함된다. 같은 절차를 모든 셀에 적용했다.
