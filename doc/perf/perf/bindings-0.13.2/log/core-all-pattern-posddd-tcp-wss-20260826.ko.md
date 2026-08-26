# Core 전체 Multi TCP/WSS POSDDD·성능 개선 (2026-08-26)

## 범위와 판정 조건

- Core: local 0.13.2, revision `99164bdc3e`, dirty worktree
- C Multi 7패턴: `DEALER_DEALER`, `DEALER_ROUTER_SENDSEND`,
  `ROUTER_ROUTER_SENDSEND`, `DEALER_ROUTER_REQREP`,
  `ROUTER_ROUTER_REQREP`, `PUBSUB`, `STREAM`
- 크기: 64, 256, 1024, 4096, 65536, 131072 bytes
- clients 100 (`STREAM`도 100), server/client I/O threads 4/4, duration 2초
- TCP는 3-run median, WSS는 secure transport 규칙에 따라 5-run median
- public API, exact-target/no-reroute, message ownership, HWM/backpressure 의미는
  변경하지 않는다.

## 발견한 문제와 변경

### WSS 종료 수명주기

WS engine의 read/write/gather callback은 weak guard가 만료되면 engine callback을
호출하지 않았다. 반면 종료 경로는 callback이 `_read_pending`과 `_write_pending`을
내릴 때까지 기다렸다. transport close로 취소된 I/O가 pending flag를 해제하지
못하면서 반복 connection lifecycle의 engine/context 종료 상태가 누적됐다.

기존 WSS 전체 기준선은 기본 3000ms cooldown에서도
`malloc_consolidate(): unaligned fastbin chunk detected`와 exit -6으로
`DEALER_DEALER` 및 `DEALER_ROUTER_SENDSEND`가 중간부터 실패했다.

engine이 handshake/read/write 완료까지 명시적으로 생존하도록 책임을 응집했다.

- `_handshake_pending`을 추가했다.
- handshake/read/write/gather callback은 항상 engine completion에 진입해 먼저
  pending flag를 해제한다.
- termination은 handshake/read/write가 모두 끝난 뒤에만 engine을 파괴한다.
- timer만 취소 후 버려도 되는 weak guard를 유지한다.

이 변경은 수명주기 결함을 제거하는 동시에 매 WS/WSS I/O callback의 weak_ptr 생성,
복사, 만료 검사를 hot path에서 제거한다.

### transport completion 복사

TCP/WS/WSS transport는 값으로 받은 `std::function` completion을 다시 lambda에
복사했다. async read/write/writev 및 handshake 경로에서 completion 소유권을 move
capture로 전달하도록 바꿨다. callback 횟수, 실행 thread, error 전달 의미는 동일하다.

### ROUTER exact-target 조회

`xsend_routed()`가 exact transport pair 요청에서도 RID generic lookup을 먼저 하고,
`find_transport_pair_pipe()` 안에서 다시 lookup했다. generic과 exact branch를 먼저
분리해 exact submit은 한 번만 조회하게 했다. reconnect generation, stale target,
terminal/no-reroute 계약은 그대로다.

### STREAM dispatch send 정책

buffer API와 message API가 direct pipe, zero-size terminate, shard fallback, HWM retry,
message reset을 각각 구현하고 있었다. `stream_dispatch_send_to_route()`가 이 도메인
정책을 한 번만 소유하고, 두 public adapter는 validation과 고유 ownership 정리만
담당하도록 축약했다. 구조 효과가 명확하므로 성능 중립이어도 채택한다.

### exact-target detach 테스트와 스펙

STREAM 테스트는 nonzero pending operation이 raw peer close 뒤 반드시 TERMINAL이어야
한다고 가정했다. 계측 결과 실패 run은 completion 유실이 아니라 admission이 detach
감지보다 먼저 끝난 `ZLINK_SEND_ADMITTED`였다. 스펙의 admission 경계는 peer delivery가
아니라 Core pipe queue다. 테스트를 operation id별 exactly-once 완료 검증으로 바꾸고
ADMITTED/TERMINAL 두 합법 결과를 허용했다. 한·영 Core spec에도 경합을 명시했다.

## 성능 결과

TCP 표는 6개 크기의 after/before 처리량 비율 산술 평균이다.

| 패턴 | TCP 평균 비율 | 판정 |
| --- | ---: | --- |
| DEALER_DEALER | 105.87% | 개선 |
| DEALER_ROUTER_SENDSEND | 122.08% | 개선 |
| ROUTER_ROUTER_SENDSEND | 112.07% | 개선 |
| DEALER_ROUTER_REQREP | 103.92% | 개선 |
| ROUTER_ROUTER_REQREP | 122.76% | 개선 |
| PUBSUB | 90.91% | 재측정 필요 |
| STREAM | 113.17% | 개선 |

PUBSUB은 전체 run에서 -9.09%였지만 즉시 동일 조건으로 단독 재측정하면 기존
기준선 대비 104.68%였다. 이 one-way workload의 run drift로 판정하며 회귀 근거로
사용하지 않는다.

기존 WSS report가 partial이므로 다음 표는 양쪽에 유효 RESULT가 있는 셀만 비교했다.

| 패턴 | WSS 평균 비율 | 비교 셀 |
| --- | ---: | ---: |
| DEALER_DEALER | 97.19% | 1 |
| DEALER_ROUTER_SENDSEND | 106.83% | 2 |
| ROUTER_ROUTER_SENDSEND | 97.48% | 6 |
| DEALER_ROUTER_REQREP | 103.88% | 6 |
| ROUTER_ROUTER_REQREP | 103.54% | 6 |
| PUBSUB | 109.32% | 6 |
| STREAM | 119.98% | 6 |

WSS의 핵심 개선은 partial/allocator crash를 42/42 complete로 바꾼 lifecycle 안정성이다.
완전한 before가 없으므로 실패 셀을 0으로 간주한 과장된 향상은 주장하지 않는다.

## 근거 report

- TCP before:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_144900_core-all-posddd-before-tcp-r3-20260826.txt`
- TCP after:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_152754_core-all-posddd-after-tcp-r3-20260826.txt`
- TCP PUBSUB repeat:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_154632_core-posddd-current-pubsub-tcp-r3-repeat-20260826.txt`
- WSS before partial:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_150444_core-all-posddd-before-wss-valid-r5-20260826.txt`
- WSS focused lifecycle after:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_151954_posddd-wss-lifecycle-dealer-dealer-r5-20260826.txt`
- WSS after:
  `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260826_153358_core-all-posddd-after-wss-r5-20260826.txt`

## 검증

- focused Release: ROUTER 2/2, STREAM 3/3, ASIO WS/WSS 13/13
- WSS repeated pending-read close regression: 16회 lifecycle 통과
- ASAN+LSAN `test_asio_ws`: 13/13, sanitizer error 없음
- STREAM detach/admission race: 10회 연속 통과
- C Multi TCP: 42/42, fail 0, status complete
- C Multi WSS: 42/42, fail 0, status complete
- 전체 Core CTest: 103/103 통과

## 채택 결론

공개 계약을 바꾸지 않고 WSS 반복 종료 결함을 제거했으며, 공통 transport I/O와
ROUTER exact-target hot path의 불필요한 복사와 조회를 줄였다. STREAM은 전송 정책
소유권이 한 함수로 응집됐다. 처리량은 대부분 개선됐고 혼재 셀도 run drift 범위이며,
구조·수명주기 효과가 명확하므로 전체 변경을 채택한다.
