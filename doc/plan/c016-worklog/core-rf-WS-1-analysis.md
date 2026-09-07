# WS-1 — WS 왕복 payload 크기 비례 비용 위치 분석

- 기준 revision: `40bc3e4910482b79e050761f9c35696fa063565a` (`main`, Core 0.17.2)
- 분석 worktree: `/home/hep7hep7/project/zlink-work/ws1` (detached)
- 분류: **B — 기존 Core/transport 결함**
- 적용 변경: 없음. 아래 patch와 수치는 실험용이며 main에 적용하지 않았다.

## 1. 결론

주원인은 WS framing header가 아니라 vendored Boost.Beast 1.85의 **client-to-server payload 전 바이트 copy + 4 byte 폭 XOR masking/unmasking**이다. `mask_inplace()`는 64 KiB 왕복 축소 셀에서 client 32.27M Ir(프로세스의 27.9%), server 32.28M Ir(48.2%)를 사용했다. TCP에는 이 함수가 없다. client 송신에서 mask를 만들고 server 수신에서 같은 byte를 다시 unmask하므로, client→server payload마다 두 프로세스에서 선형 pass가 하나씩 생긴다.

두 번째 원인은 **16 KiB ZMP encoder batch와 64 KiB Beast client scratch 경계**다. 65,536 B payload + ZMP header + 빈 FINAL part는 기본 설정에서 방향마다 세 번의 Beast write가 된다. 첫 16 KiB batch, encoder가 zero-copy로 돌려주는 나머지 payload, 빈 FINAL frame이다. 왕복은 reply가 이 연속 completion 뒤에만 진행되므로 이 비용이 feedback loop의 cycle time에 들어간다. 단방향은 여러 socket과 계속 찬 queue가 이 직렬 지연을 겹쳐 실행해 throughput 병목이 다른 곳으로 이동한다.

실험 근거는 서로 독립적이다.

1. masking loop만 8 byte scalar 처리로 바꾸면 16 KiB chunk당 46.9k→10.2k Ir, **-78.2%**였다.
2. WS encoder batch만 16→256 KiB로 키우면 64 KiB 왕복이 12.74k→15.80k ops/s, **+24.0%**였다. Beast write buffer도 64→256 KiB로 맞추면 17.48k, **+37.2%**였다. 다만 측정 당시 load가 서로 달라 절대 개선율은 방향성 근거로만 사용한다.
3. 64 KiB syscall 계열은 동일한 4 client 축소 셀에서 TCP 8.53회/completed op, WS 22.44회/completed op로 **2.63배**였다.

따라서 1차 수정은 RFC 동작을 그대로 둔 채 masking 구현 폭을 넓히는 것이다. 2차 수정은 WS 전용 batch를 payload 하나와 multipart 경계까지 담을 수 있게 하되, connection당 고정 buffer 증가를 피하도록 기존 encoder buffer의 성장/회수 정책 안에서 해결해야 한다. 전역 gather opt-in은 같은 셀에서 -57.9%였으므로 해법이 아니다.

## 2. 재현

요청된 C multi runner를 `ZLINK_CORE_SOURCE=local`, `--duration 3 --runs 1 --reuse-build`, 100 clients, 기본 4/4 I/O thread로 한 번 실행했다. 두 실행 모두 `PERF_LOCK`을 잡았고 시작 전 `pgrep -c -x ninja == 0`을 확인했다. local runtime은 `/home/hep7hep7/project/zlink-work/ws1/core/build-dev/lib/libzlink.so.0.17.2`였다.

| 패턴 | 크기 | TCP | WS | WS/TCP | mean latency TCP / WS | 시작 load average |
|---|---:|---:|---:|---:|---:|---:|
| DEALER_DEALER 단방향 | 1,024 B | 956,421.7 msg/s | 895,829.0 msg/s | 0.937 | 0.996 / 3.961 ms | 2.32 / 4.21 / 2.43 |
| DEALER_DEALER 단방향 | 65,536 B | 71,574.7 msg/s | 53,488.3 msg/s | 0.747 | 22.557 / 28.088 ms | 2.32 / 4.21 / 2.43 |
| DEALER_ROUTER_SENDSEND 왕복 | 1,024 B | 264,840.3 ops/s | 234,059.0 ops/s | 0.884 | 0.762 / 13.907 ms | 2.51 / 4.13 / 2.44 |
| DEALER_ROUTER_SENDSEND 왕복 | 65,536 B | 39,501.0 ops/s | 12,737.3 ops/s | **0.322** | 23.170 / 309.294 ms | 2.51 / 4.13 / 2.44 |

단방향 64 KiB의 local dev 결과도 0.747로 고정 0.17.2 기준선(0.936~1.045)보다 낮았다. 이 세션의 높은 load와 dev library 때문에 절대값은 release 기준선 대체값으로 쓰지 않는다. 그러나 같은 invocation 안 왕복 비율은 1 KiB 0.884→64 KiB 0.322로 하강했고, 단방향보다 64 KiB에서 추가로 0.425 낮아 결함을 재현했다.

원자료:

- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_075027_ws1-repro-dd.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_075055_ws1-repro-dr.txt`

## 3. callgrind 방법과 상위 20 함수

왕복 셀을 4 clients, duration 1 s, server/client 각 1 I/O thread로 줄였다. `valgrind --tool=callgrind --instr-atstart=yes`를 runner가 server와 client command 각각에 붙이도록 실험용 prefix를 추가했다. 모든 callgrind 실행 직전에 ninja 0을 확인했고 `PERF_LOCK` 아래 포그라운드로 실행했다. 계측 중 처리량은 TCP 489 ops/s, WS 111 ops/s였다.

아래 값은 exclusive Ir다. `libc.so.6:0x188c00`은 stripped IFUNC-resolved routine이라 함수명이 남지 않았다. call tree만으로 특정 memcpy 구현이라고 단정하지 않고 주소 그대로 기록한다. 짧은 프로세스의 loader/startup 비용이 포함되므로 payload 경로와 무관한 하위 순위는 원인으로 채택하지 않는다.

### 3.1 Client process

| 순위 | TCP 함수 | TCP Ir (%) | WS 함수 | WS Ir (%) |
|---:|---|---:|---|---:|
| 1 | libc `0x188c00` | 48,053,519 (61.66) | libc `0x188c00` | 53,347,321 (46.11) |
| 2 | `pthread_mutex_lock` | 2,253,353 (2.89) | Beast `mask_inplace` | **32,266,176 (27.89)** |
| 3 | `pthread_mutex_unlock` | 1,773,843 (2.28) | `pthread_mutex_lock` | 1,339,721 (1.16) |
| 4 | loader `0xa530` | 992,098 (1.27) | `pthread_mutex_unlock` | 1,046,440 (0.90) |
| 5 | loader `0xb0d0` | 873,464 (1.12) | loader `0xa530` | 996,004 (0.86) |
| 6 | loader `0xf240` | 814,023 (1.04) | loader `0xb0d0` | 878,271 (0.76) |
| 7 | `socket_base_t::process_commands` | 693,536 (0.89) | loader `0xf240` | 814,023 (0.70) |
| 8 | libc `0x189600` | 431,941 (0.55) | `any_executor_base` move ctor | 619,602 (0.54) |
| 9 | libc `0xab650` | 386,482 (0.50) | libc `0x189600` | 441,069 (0.38) |
| 10 | `msg_t::close` | 376,777 (0.48) | Beast `read_some_op` ctor | 369,180 (0.32) |
| 11 | libc `0xab250` | 365,443 (0.47) | `socket_base_t::process_commands` | 352,280 (0.30) |
| 12 | `get_events_internal` | 331,313 (0.43) | libc `0xab250` | 311,641 (0.27) |
| 13 | `malloc` | 316,567 (0.41) | WS handshake callback | 298,536 (0.26) |
| 14 | `msg_t::clear_auxiliary` | 305,168 (0.39) | `any_executor_base::destroy_object` | 294,840 (0.25) |
| 15 | benchmark `run_echo_window_round_robin` | 254,413 (0.33) | libc `0xab650` | 263,337 (0.23) |
| 16 | `mailbox_t::recv` | 240,247 (0.31) | `malloc` | 262,534 (0.23) |
| 17 | `free` | 225,401 (0.29) | `getenv` | 223,839 (0.19) |
| 18 | `getenv` | 219,274 (0.28) | Beast `write_some_op` ctor | 220,440 (0.19) |
| 19 | `zlink_recv_part` | 203,011 (0.26) | `msg_t::close` | 212,752 (0.18) |
| 20 | libc `0xab070` | 189,537 (0.24) | `free` | 192,417 (0.17) |
| 합계 | process total | **77,931,149** | process total | **115,707,740** |

### 3.2 Server process

| 순위 | TCP 함수 | TCP Ir (%) | WS 함수 | WS Ir (%) |
|---:|---|---:|---|---:|
| 1 | libc `0x188c00` | 10,010,850 (29.39) | Beast `mask_inplace` | **32,279,296 (48.19)** |
| 2 | `pthread_mutex_lock` | 1,151,141 (3.38) | libc `0x188c00` | 5,645,228 (8.43) |
| 3 | loader `0xa530` | 1,004,223 (2.95) | loader `0xa530` | 1,007,466 (1.50) |
| 4 | loader `0xb0d0` | 879,403 (2.58) | `pthread_mutex_lock` | 885,913 (1.32) |
| 5 | `pthread_mutex_unlock` | 870,185 (2.55) | loader `0xb0d0` | 883,345 (1.32) |
| 6 | loader `0xf240` | 816,441 (2.40) | loader `0xf240` | 816,441 (1.22) |
| 7 | `msg_t::close` | 436,474 (1.28) | `pthread_mutex_unlock` | 667,278 (1.00) |
| 8 | libc `0xab250` | 423,746 (1.24) | `any_executor_base` move ctor | 598,344 (0.89) |
| 9 | `malloc` | 360,897 (1.06) | libc `0xab250` | 435,458 (0.65) |
| 10 | `msg_t::clear_auxiliary` | 358,272 (1.05) | `malloc` | 364,002 (0.54) |
| 11 | `socket_base_t::process_commands` | 276,933 (0.81) | Beast `write_some_op` ctor | 326,370 (0.49) |
| 12 | `free` | 264,433 (0.78) | WS handshake callback | 279,372 (0.42) |
| 13 | benchmark `drain_recv_and_relay` | 215,280 (0.63) | `free` | 279,306 (0.42) |
| 14 | `asio_poller_t::loop` | 194,880 (0.57) | Beast `read_some_op` ctor | 278,880 (0.42) |
| 15 | libc `0xab650` | 187,631 (0.55) | `any_executor_base::destroy_object` | 264,240 (0.39) |
| 16 | loader `0xa390` | 174,905 (0.51) | `msg_t::close` | 255,328 (0.38) |
| 17 | loader `0x274e0` | 164,093 (0.48) | Beast `buffers_prefix_view` ctor | 237,360 (0.35) |
| 18 | `msg_t::move` | 157,260 (0.46) | `msg_t::clear_auxiliary` | 209,728 (0.31) |
| 19 | inline `msg_t::close` | 156,744 (0.46) | `socket_base_t::process_commands` | 191,662 (0.29) |
| 20 | `encoder_base_t::encode` | 149,418 (0.44) | `asio_poller_t::loop` | 180,390 (0.27) |
| 합계 | process total | **34,061,121** | process total | **66,988,178** |

TCP→WS에서 추가된 지배 함수가 양쪽 모두 같은 `mask_inplace`이고, WS frame header parser는 64 KiB에서 client 182,039 Ir(0.16%), server 136,900 Ir(0.20%)뿐이다. 따라서 header parsing이나 fragmentation state가 payload 비례 붕괴의 주원인이라는 가설은 기각한다.

## 4. 크기 비례성

동일한 WS 왕복 축소 셀을 1,024/4,096/65,536 B에서 실행했다. duration 고정 계측은 종료 시 in-flight 요청 수가 달라 `Ir/completed op`가 완전한 steady-state 단가가 아니다. 이 열은 관측 상한이며, 함수 구현 자체의 선형성은 `mask.ipp:46-51`의 `n -= 4` loop와 64 KiB chunk당 call 비용으로 판정한다.

| payload | 완료 ops/s | client total Ir | client mask Ir (%) | server total Ir | server mask Ir (%) | 양쪽 mask Ir / completed op |
|---:|---:|---:|---:|---:|---:|---:|
| 1,024 B | 2,450 | 126,317,855 | 7,392,892 (5.85) | 113,028,117 | 7,406,852 (6.55) | 6,041 |
| 4,096 B | 720 | 159,239,300 | 22,829,496 (14.34) | 132,744,303 | 22,867,564 (17.23) | 63,468 |
| 65,536 B | 111 | 115,707,740 | 32,266,176 (27.89) | 66,988,178 | 32,279,296 (48.19) | 581,491 |

64 KiB에서 payload-sized mask call은 client 656회/32,166,400 Ir, server 656회/30,761,104 Ir였다. 즉 16 KiB급 chunk당 각각 약 49.0k/46.9k Ir다. 실험한 8 byte loop는 같은 유형의 416회 call에 4,252,480/4,075,324 Ir, 약 10.2k/9.8k Ir를 사용했다. 호출당 비교에서 **78~79% 감소**하므로 duration이나 완료 op 차이에 의존하지 않는다.

`buffer_copy` 심볼들의 exclusive Ir 합과 frame header parse는 다음과 같다. payload loop만큼 증가하지 않고 전체에서 0.2% 아래여서 1차 원인이 아니다. 다만 client `do_mask_nofrag`가 scratch로 복사한 실제 byte 비용 일부는 stripped libc routine에 귀속되므로 이 합은 copy 전체 비용이 아니다.

| payload | client `buffer_copy` | server `buffer_copy` | client `parse_fh` | server `parse_fh` |
|---:|---:|---:|---:|---:|
| 1,024 B | 110,572 | 106,578 | 100,004 | 98,355 |
| 4,096 B | 269,882 | 206,367 | 281,140 | 194,430 |
| 65,536 B | 173,850 | 144,121 | 182,039 | 136,900 |

## 5. syscall과 왕복/단방향 차이

system `strace`가 없어 Ubuntu `strace` 6.8 package를 `/tmp/ws1-tools`에 압축 해제해 사용했으며 시스템에는 설치하지 않았다. 같은 4 client, 1 s, 1/1 I/O thread 왕복 셀에서 `strace -c -f`를 양쪽 프로세스에 붙였다.

| transport | 완료 ops/s | client send / recv | server send / recv | 양쪽 network syscall 합 | syscall/completed op |
|---|---:|---:|---:|---:|---:|
| TCP | 1,331 | `sendto` 2,810 / `recvfrom` 2,839 | `sendto` 2,822 / `recvfrom` 2,881 | 11,352 | 8.53 |
| WS | 823 | `sendmsg` 2,785 / `recvmsg` 6,006 | `sendmsg` 4,134 / `recvmsg` 5,540 | 18,465 | **22.44** |

WS는 network syscall/completed op가 TCP의 2.63배다. total syscall도 TCP 38,870(29.20/op), WS 52,799(64.15/op)였다. 완료 op 정규화는 종료 시 in-flight를 포함하지 않아 상한이지만, 동일 harness 대조에서 WS의 partial receive와 frame write 증폭을 확인한다.

같은 축소 조건의 단방향 callgrind 대조도 별도로 실행했다. 단방향 WS에도 masking 비용 자체는 존재하지만 처리량 비는 0.895인 반면, 왕복 WS/TCP는 0.227이었다.

| 패턴 | TCP / WS 처리량 | WS/TCP | WS client mask Ir (%) | WS server mask Ir (%) |
|---|---:|---:|---:|---:|
| DEALER_DEALER 단방향 | 257 / 230 msg/s | **0.895** | 24,888,468 (25.88) | 24,898,628 (62.37) |
| DEALER_ROUTER_SENDSEND 왕복 | 489 / 111 ops/s | **0.227** | 32,266,176 (27.89) | 32,279,296 (48.19) |

단방향에서도 mask는 WS 프로세스 Ir의 큰 몫이므로 “단방향에는 mask 비용이 없다”는 설명은 틀리다. 같은 비용이 producer/consumer pipeline에서는 겹쳐지고, request→reply dependency가 있는 왕복에서는 completion chain의 cycle time이 된다는 설명만 두 패턴을 함께 만족한다.

코드상 직렬 경로는 다음과 같다.

1. `asio_engine_t::process_output()`가 `out_batch_size`까지 encoder output을 모은다 (`asio_engine.cpp:1410-1453`).
2. WS listener/connecter가 non-STREAM `out_batch_size`를 16 KiB로 강제한다 (`asio_ws_listener.cpp:288-292`, `asio_ws_connecter.cpp:393-397`).
3. `encoder_base_t::encode()`는 큰 body가 batch 시작에 오면 나머지를 zero-copy span으로 따로 반환한다 (`encoder.hpp:72-87`). 64 KiB payload+header는 첫 16 KiB와 나머지 span으로 갈리고, 빈 FINAL part도 별도 batch가 된다.
4. engine은 prepared span마다 `async_write_some()` 하나를 두고 completion 뒤 다음 span을 시작한다 (`asio_engine.cpp:643-717`). WS adapter는 그 span 전체를 Beast `async_write` 하나로 제출한다 (`ws_transport.cpp:223-249`).
5. RFC client role이면 Beast `do_mask_nofrag`가 scratch에 copy하고 mask한 뒤 `wr_buf_size`마다 `async_write`한다 (`write.hpp:308-387`). server role unmasked write는 header와 원본 buffer를 scatter해서 보낸다 (`write.hpp:211-241`).
6. server read는 masked payload를 `read.hpp:445-499`에서 unmask한다. 실제 XOR은 `mask.ipp:41-58`의 4 byte loop다.

| 후보 | 함수:행 | 계측/코드 근거 | 판정 |
|---|---|---|---|
| payload mask/unmask | `boost::beast::websocket::detail::mask_inplace`, `mask.ipp:41-58`; 호출부 `write.hpp:308-387`, `read.hpp:445-499` | WS 64 KiB client/server 각각 32.3M Ir, TCP 0; chunk당 비용 78% 축소 실험 | **주원인** |
| encoder batch/flush | `ws_batch_policy::zmp_send_batch_size`, `ws_batch_policy.hpp:10-16`; `asio_engine_t::process_output`, `asio_engine.cpp:1410-1453` | 16→256 KiB에서 +24%; 64 KiB payload+empty FINAL이 방향당 3→1 submission | **2차 원인** |
| WS frame 내부 write 분할 | Beast `write_some_op`, `write.hpp:308-387`; `ws_write_buffer_bytes`, `ws_transport.cpp:30-34,102-107` | batch와 scratch를 함께 256 KiB로 키우면 +37%; WS network syscall/op이 TCP의 2.63배 | **2차 원인** |
| frame header parse | Beast `impl_type::parse_fh` | 64 KiB에서 0.16~0.20% Ir | 주원인 기각 |
| 수신 buffer 재할당/copy | `ws_transport_t::async_read_some`, `ws_transport.cpp:138-175` | engine decoder buffer에 Beast가 직접 읽음; `buffer_copy` 명시 심볼 합 0.22% 이하 | 주원인 기각 |
| compression | WS open `ws_transport.cpp:102-107` | permessage-deflate를 설정하지 않음; 크기 곡선을 설명할 compression work 없음 | 원인 기각, 수정 후보도 제외 |

왕복은 request의 위 completion들이 server receive/reply보다 먼저, reply의 completion들이 다음 관측 가능한 완료보다 먼저 놓인다. 크기가 커지면 mask byte pass와 write completion 수가 feedback loop에 직접 더해진다. 단방향은 reply dependency가 없고 100개 socket이 계속 제출하므로 여러 connection의 write를 I/O thread가 겹치며, queue/application 처리량이 지배해 동일 비용이 WS/TCP throughput 비에 덜 드러난다. 즉 **단방향에 비용이 없는 것이 아니라 비용이 critical path에 있지 않다.**

## 6. 계약과 구현 선택의 경계

| 항목 | 계약상 변경 불가 | 현재 구현 선택 |
|---|---|---|
| masking | RFC 6455 §5.3: client→server frame은 fresh 32-bit key로 mask, server→client는 mask 금지, payload byte는 key modulo 4로 XOR | XOR을 4 byte마다 scalar loop로 수행하는 폭과 구현은 계약이 아니다. 8/16/32 byte 단위 구현으로 바꿔도 wire byte가 같다. |
| WS message 경계 | `01-zmp` §8(`:381-392`): binary message 경계는 ZMP frame 경계가 아니며 decoder가 byte stream에서 frame을 복원 | `out_batch_size=16 KiB`, batch당 Beast binary write 1회는 내부 batching 정책이다. 경계를 합치거나 나눠도 byte 순서가 같으면 계약 유지. |
| fragmentation | RFC 6455 §5.4와 `01-zmp` §8: endpoint는 fragmented/unfragmented message를 처리하고 ZMP는 그 경계와 독립 | `auto_fragment(false)`, Beast scratch 64 KiB, 한 encoder batch를 한 WS message로 만드는 것은 구현 선택. |
| 압축 | 협상되지 않은 extension을 임의 사용하지 않는 RFC 동작은 유지 | 현재 permessage-deflate를 켜지 않는다. 압축 도입은 CPU/latency와 wire extension 정책을 새로 만들므로 이 수정 후보에서 제외. |
| 소유 계층 | `08-runtime-boundary` §6(`:127-129`): engine이 TCP/WS/TLS framing 소유 | socket/runner에서 보상하지 않고 `runtime/transports/ws`와 그 vendored Beast 호출 경로에서 고친다. |

`08-stream.ko.md:308-337`의 WS 특성은 raw STREAM 전용이다. 이번 DEALER/ROUTER는 ZMP engine이므로 주 계약은 `01-zmp.ko.md:379-392,481-492`다. 특히 §9는 bounded batch를 Beast write 한 번에 제출해 per-ZMP-frame async write 비용을 없애는 구조를 명시한다. 16 KiB 경계가 64 KiB frame을 여러 write로 만드는 현재 결과는 그 성능 의도와 맞지 않는다.

RFC 출처: [RFC 6455 §5.3 Client-to-Server Masking](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.3), [§5.4 Fragmentation](https://www.rfc-editor.org/rfc/rfc6455.html#section-5.4).

## 7. 수정 방향, 기대 이득과 위험

| 우선순위 | 방향 | 실험/예상 이득 | 위험과 필요한 검증 |
|---:|---|---|---|
| 1 | vendored Beast `mask_inplace`를 unaligned-safe 64-bit 이상 XOR로 교체. mask key byte 순서와 tail `rol`은 유지 | 16 KiB chunk mask Ir -78~-79%; 64 KiB baseline에서 mask가 client 27.9%, server 48.2%이므로 CPU headroom의 가장 큰 직접 회수 | big/little endian, unaligned buffer, 0~31 B tail, fragmented continuation에서 key rotation, sanitizer 검증. 가능하면 upstream patch 형태로 유지 |
| 2 | non-STREAM WS encoder batch가 최소한 현재 큰 ZMP frame+multipart FINAL을 한 batch에 담도록 기존 batch sizing을 정리 | 단순 256 KiB 실험 +24%; direction당 engine/Beast submission이 64 KiB에서 3→1 | 고정 256 KiB를 모든 connection에 주면 encoder memory가 connection 수에 비례해 증가. 기존 buffer 성장/회수 소유자 안에서 해결하고 새 option/state를 만들지 말 것 |
| 3 | batch와 Beast client `write_buffer_bytes` 경계를 함께 조정하거나 pooled scratch를 사용 | batch 256 KiB + write buffer 256 KiB 실험 +37%; client의 65,552 B masked frame이 64 KiB 경계에서 2회 쓰이는 현상 제거 | connection당 Beast scratch 증가가 가장 큼. 100/1,000/10,000 connection RSS와 HWM accounting을 측정해야 함 |
| 기각 | `ZLINK_ASIO_GATHER_WRITE=1`을 기본화 | 12.74k→5.36k ops/s, **-57.9%** | 한 message body만 gather하고 기존 multi-frame batching을 잃어 규칙과 성능 모두 악화 |
| 기각 | client masking 비활성화, server reply도 mask, 압축 강제 | 일부 copy/XOR 제거 가능 | RFC 위반 또는 새 negotiation/정책. 계약 변경이므로 허용 불가 |

실험 wall-clock 표다. baseline과 실험 사이 load가 달라 acceptance 수치로 사용하지 않는다. masking 실험의 wall-clock은 load 4.00이라 폐기하고 callgrind 호출당 Ir만 채택했다.

| 실험 | 64 KiB WS ops/s | baseline 대비 | load average | 판정 |
|---|---:|---:|---:|---|
| baseline | 12,737.3 | — | 2.51 / 4.13 / 2.44 | 재현 기준 |
| gather opt-in | 5,364.7 | -57.9% | 0.31 / 1.05 / 1.57 | 기각 |
| batch 256 KiB | 15,799.7 | +24.0% | 1.31 / 1.08 / 1.51 | 방향성 확인 |
| batch 256 KiB + Beast buffer 256 KiB | 17,477.0 | +37.2% | 1.37 / 1.12 / 1.51 | 방향성 확인, RSS 위험 |
| 8 B mask loop | 6,561.0 | 폐기 | 4.00 / 2.14 / 1.79 | wall-clock 폐기; 호출당 Ir -78%만 채택 |

두 대안 중 “mask 최적화 먼저, batching은 memory 증거 뒤”를 선택한다. mask 변경은 새 상태/옵션 없이 기존 단일 소유 함수의 같은 규칙을 더 넓게 실행한다. 반면 고정 batch 확대는 속도 이득이 있지만 connection당 두 buffer 크기라는 새 비용을 만든다.

## 8. 회귀 테스트 제안

기능 test와 perf gate를 분리한다.

1. **결정적 기능 test**: Beast mask helper에 0, 1, 3, 4, 7, 8, 15, 16, 65,535, 65,536, 65,537 B와 모든 start key offset을 넣어 기존 byte-reference XOR와 비교한다. 같은 buffer를 두 번 mask하면 원문이 되는지, fragmented call 사이 `rol` key가 이어지는지 확인한다. ASan/UBSan으로 unaligned access를 포함한다.
2. **WS framing integration**: 64 KiB payload+빈 FINAL multipart를 loopback WS/WSS로 보내 payload와 part 경계가 그대로인지 확인한다. 내부 test hook 또는 deterministic fake transport로 한 방향 engine submission 수를 세어 기본 64 KiB 셀이 payload 크기에 따라 불필요하게 여러 submission으로 늘지 않는지 검증한다. 공개 contract assertion은 바꾸지 않는다.
3. **D-BP28 perf gate**: 기존 C multi를 release, 같은 host/load, 5-run median으로 실행해
   `Q(size) = (WS 왕복/TCP 왕복) / (WS 단방향/TCP 단방향)`을 계산한다. 1 KiB와 64 KiB에서 `Q`의 하락을 비교하며, 권장 초기 상한은 `Q(64KiB) >= 0.80 * Q(1KiB)`다. 절대 ops/s가 아닌 이 이중 비율을 써 host 성능 변동을 제거한다. threshold는 수정 후 clean-host 분포로 확정한다.
4. WSS도 같은 Beast masking/framing 경로이므로 동일 gate를 WS/WSS 각각 둔다. 기존 C와 C++ 교차 runner는 release 후보 확인에 유지하되 unit gate의 중복 구현으로 쓰지 않는다.

## 9. 실험 patch (적용 금지)

### 9.1 계측용 runner prefix

이 patch만 worktree에 남아 있다. server/client 각각 callgrind/strace wrapper를 붙이기 위한 진단용이며 제품 수정안이 아니다.

```diff
diff --git a/bindings/c/perf/run_comparison.py b/bindings/c/perf/run_comparison.py
@@
+import shlex
@@
+def apply_diagnostic_command_prefix(cmd, role):
+    raw = os.environ.get(f"PERF_DIAGNOSTIC_{role.upper()}_COMMAND_PREFIX", "").strip()
+    if not raw:
+        return cmd
+    return shlex.split(raw) + cmd
@@
     client_cmd = build_bench_cmd(shared_client_path, shared_client_args)
+    server_cmd = apply_diagnostic_command_prefix(server_cmd, "server")
+    client_cmd = apply_diagnostic_command_prefix(client_cmd, "client")
@@
     client_cmd = build_bench_cmd(
         client_binary_path, [lib_name, transport, str(fallback_size)]
     )
+    server_cmd = apply_diagnostic_command_prefix(server_cmd, "server")
+    client_cmd = apply_diagnostic_command_prefix(client_cmd, "client")
```

### 9.2 8 byte masking 가설

계측 뒤 기준 소스로 복원했다. 아래는 적용하지 않은 실험 diff다.

```diff
diff --git a/core/external/boost/boost/beast/websocket/detail/mask.ipp b/core/external/boost/boost/beast/websocket/detail/mask.ipp
@@
+#include <cstdint>
+#include <cstring>
@@
-    while(n >= 4)
+    std::uint32_t mask32;
+    std::memcpy(&mask32, mask.data(), sizeof(mask32));
+    std::uint64_t const mask64 = mask32 | (std::uint64_t(mask32) << 32);
+    while(n >= 8)
     {
-        for(int i = 0; i < 4; ++i)
-            p[i] ^= mask[i];
+        std::uint64_t word;
+        std::memcpy(&word, p, sizeof(word));
+        word ^= mask64;
+        std::memcpy(p, &word, sizeof(word));
+        p += 8;
+        n -= 8;
+    }
+    if(n >= 4)
+    {
+        std::uint32_t word;
+        std::memcpy(&word, p, sizeof(word));
+        word ^= mask32;
+        std::memcpy(p, &word, sizeof(word));
         p += 4;
         n -= 4;
     }
```

### 9.3 batch 가설

계측 뒤 기준 16 KiB로 복원했다.

```diff
diff --git a/core/src/runtime/transports/ws/ws_batch_policy.hpp b/core/src/runtime/transports/ws/ws_batch_policy.hpp
@@
-    return 16 * 1024;
+    return 256 * 1024;
```

## 10. 검증과 남은 위험

- `JOBS=4 scripts/build-core.sh dev`: 성공(기준/각 실험 재빌드).
- `python3 -m py_compile bindings/c/perf/run_comparison.py`: 성공.
- `ctest --test-dir core/build-dev -R '(^test_asio_ws$|^test_zmp_ws_wss$|^unittest_ws_transport_config$)' --output-on-failure`: 3/3 성공.
- 제품 source patch는 남기지 않았고 public API/spec/commit/stash를 변경하지 않았다.
- callgrind 원자료: `/tmp/ws1-callgrind/{tcp,ws}-{65536}-{client,server}.*`, 크기 대조 `/tmp/ws1-callgrind/ws-{1024,4096}-{client,server}.*`, mask 실험 `/tmp/ws1-callgrind/wsopt-65536-{client,server}.*`.
- 단방향 callgrind 원자료: `/tmp/ws1-callgrind/dd-{tcp,ws}-65536-{client,server}.*`(TCP 257 msg/s, WS 230 msg/s, load 2.76/4.02/4.64와 1.46/3.93/4.63).
- strace 원자료: `/tmp/ws1-callgrind/{tcp,ws}-65536-{client,server}.strace`.
- 남은 위험: load가 완전히 idle한 release 성능 검증, 64-bit mask의 전체 플랫폼 correctness, batch 확대 시 high-connection RSS, WSS에서 TLS가 개선분을 얼마나 가리는지는 구현 job에서 재측정해야 한다.

소유 계층: Core `runtime/transports/ws` + vendored Beast WS framing.

Spec 조항: `core/doc/spec/core/protocol/01-zmp.ko.md` §8, WebSocket 구현 §9; `08-runtime-boundary.ko.md` §6.

교차 구현: C와 C++ runner가 같은 크기 곡선(D-BP28); language binding 차이가 아닌 공통 Core 경로다.

수정 전/후 규칙 수: 적용 변경 없음. 제안 1은 “4 B XOR” 구현 폭만 교체하여 프로토콜 규칙과 상태 수가 0개 증가한다.
