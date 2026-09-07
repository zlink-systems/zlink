# G-A — 공통 경로 프로파일 (측정 전용)

> 2026-09-07, main `f127924578`. **측정용 lib는 직접 빌드했다**: `core/build/lib/libzlink.so.0.17.0`(메인 워크트리)은
> 04:20:40 산출물로 마지막 core 커밋 `2529709db6`(04:32:36)보다 오래됐고, 메인 워크트리 `core/`에는 게이트 잡의
> 미병합 패치가 staged 상태(`asio_engine.cpp`, `stream.cpp`, `decoder.hpp` 등)로 남아 있다. 따라서 fresh worktree
> `~/project/zlink-work/ga`(HEAD f127924578)에서 `JOBS=4 scripts/build-core.sh release --lib-only` 로 Release+LTO
> lib를 만들고, 같은 트리에서 bindings/c perf·with_stream 바이너리를 빌드해 측정했다.
> 원본 데이터: `<scratchpad>/GA/`(`cg_*.out` = callgrind, `*.sh` = 구동 스크립트, `parse.py`/`sys.py` = 심볼·호출수 파서).
> 계측은 `flock <PERF_LOCK>` 아래, `pgrep -x ninja` 확인 후 실행했다.

## 0. 방법과 한계

S-A(§0)의 축소 callgrind 셀 방법을 그대로 쓴다. `--tool=callgrind --cache-sim=no`, CCU 20, 1024 B, tcp.
`perf`는 이 커널에서 여전히 사용 불가(§S-A 0). 셀별 구동 방식:

| 셀 | 바이너리 | 구성 | 메시지 수 산출 |
|---|---|---|---|
| single ROUTER_ROUTER | `perf_router_router zlink tcp 1024` (단일 프로세스) | `PERF_SINGLE_DURATION_SECONDS` 12 s / 4 s **두 판** | 두 판의 **차분**(Ir·msg 모두)으로 기동분·고정비 제거 |
| multi DEALER_DEALER | `comp_src_dealer_dealer_server`(callgrind) + native client | CCU 20, 10 s | 서버 `RESULT,…,throughput` × duration |
| multi DEALER_ROUTER_REQREP | `comp_src_dealer_router_reqrep_*` | CCU 20, 6 s | 클라이언트 throughput × duration |
| multi PUBSUB | `comp_src_pubsub_*` | CCU 20, 8 s | 클라이언트 throughput × duration |
| STREAM zlink / zmq | `bench/with_stream/test_scenario_stream_{zlink,zmq}` + `bench_streamcompare_client` | CCU 20, 10 s, io-threads 4 | 서버 `METRIC … recv_msgs=` |

기동분은 클라이언트 없이 같은 서버를 띄운 idle 판(`cg_*_idle.out`)을 빼서 제거했다.

**신뢰도 등급**(이 보고서의 모든 판단은 등급 A만 근거로 쓴다):

- **A(신뢰)** — single ROUTER_ROUTER(차분법), multi DEALER_DEALER(idle 3.8 M Ir로 무시 가능), STREAM zlink/zmq(idle 4.1 M/3.3 M Ir).
- **B(참고)** — multi PUBSUB: 서버가 구독자 없이도 계속 publish 하므로 idle 판(561 M Ir)이 "무부하"가 아니다. 뺄셈이 과대/과소 어느 쪽인지 확정할 수 없다.
- **C(불가)** — multi DEALER_ROUTER_REQREP: 서버가 STOP 후 즉시 종료하지 않아 `callgrind_control -d`로 덤프해야 했고, 그 사이 유휴 폴링 Ir이 섞였다. 아래 수치는 상한이다.
- **측정 실패** — **single ROUTER_ROUTER_REQREP(−6 % 셀)**: callgrind 아래에서 `completed=0`으로 항상 FAIL 한다
  (`[perf-single-reqrep] no completed request`). `PERF_SINGLE_REQREP_TIMEOUT_MS=15000`,
  `PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS=60000`, `PERF_SINGLE_RCVTIMEO_MS=10000`, 핸드셰이크·connect-ready 60 s로
  올려도 동일하다. 즉 이 셀은 **완료(completion) 경로가 valgrind 직렬화 아래에서 진행하지 않는다** — 축소 셀 방법이
  적용되지 않는 유일한 패턴이다. G-6에 별도 job으로 뺐다.

## 1. 셀별 메시지당 Ir

| 셀 | Ir/msg | 등급 | 비고 |
|---|---|---|---|
| single ROUTER_ROUTER tcp 1024 | **15,806** | A | 한 프로세스가 송·수신 양단 + I/O 스레드를 모두 포함. 이 중 **1,135은 벤치 하네스 `getenv`**(§4, G-5) |
| multi DEALER_DEALER tcp 1024 | **13,081** | A | 서버(에코) 측만. recv+send 1왕복 |
| STREAM zlink (with_stream) | **9,808** | A | 서버만 |
| STREAM zmq (with_stream) | **6,985** | A | 서버만 — zlink **1.40×** |
| multi PUBSUB tcp 1024 | 28,606 | B | idle 오염 |
| multi DEALER_ROUTER_REQREP | ≤82,918 | C | 유휴 폴링 오염, 상한 |
| single ROUTER_ROUTER_REQREP | — | 실패 | §0 |

## 2. 패턴 공통 비용 표 — 5개 셀 모두에 나타나는 Core 심볼

249개 Core 심볼이 5셀 전부에 나타난다. 그 self Ir 합(메시지당):

| | RR(single) | DD(multi) | STREAM | PUBSUB | DRQ |
|---|---|---|---|---|---|
| **공통 249심볼 합 Ir/msg** | **5,831** | **5,486** | **4,815** | 16,734 | 31,337 |
| 셀 전체 대비 | 37 % | 42 % | 49 % | (B) | (C) |

상위 공통 심볼(Ir/msg):

| 심볼 | RR | DD | STREAM | PUBSUB | DRQ |
|---|---|---|---|---|---|
| `pthread_mutex_lock` | 622 | 1,058 | 648 | 2,190 | 4,436 |
| `pthread_mutex_unlock` | 456 | 816 | 490 | 1,693 | 3,274 |
| `msg_t::close` | 747 | 789 | 149 | 1,141 | 2,836 |
| `msg_t::size` | 366 | 297 | 170 | 829 | 1,581 |
| `msg_t::init` | 269 | 259 | 60 | 249 | 1,089 |
| `msg_t::data` / `msg_t::check` | 179 | 191 | 53 | 600 | 561 |
| `pipe_t::account_inbound_frame` | 240 | 179 | 188 | 168 | 1,105 |
| `pipe_t::read` | 147 | 156 | 215 | 144 | 737 |
| `ypipe_t::read` | 153 | 162 | 196 | 151 | 768 |
| `ypipe_t::write` | 170 | 135 | 88 | 123 | 834 |
| `pipe_t::frame_accounted_bytes` | 91 | 49 | 60 | 135 | 450 |
| `pipe_t::flush_unlocked` | 63 | 14 | 94 | 47 | 328 |
| `mutex_t::lock`/`unlock` | 66 | 131 | 128 | 384 | 427 |
| `socket_public_handle_t::release` | 74 | 58 | 39 | 253 | 369 |
| `malloc`+`free` | 157 | 59 | 118 | 649 | 215 |
| `clock_gettime` | 29 | 39 | 22 | 155 | 124 |

### 2.1 S-13 layering 537 Ir/msg 는 다른 패턴에서도 같은가 — **아니다, 더 크다**

S-13이 STREAM에서 잰 pipe/ypipe layering 537 Ir/msg에 대응하는 집합
(`pipe_t::read` + `ypipe_t::read` + `ypipe_t::write` + `account_inbound_frame` + `frame_accounted_bytes` + `flush_unlocked`):

| RR | DD | STREAM |
|---|---|---|
| **864** | **695** | **841** |

즉 layering 비용은 **패턴에 무관한 공통 고정비**이며(패턴별 편차 ±20 %), STREAM에서 잰 537보다 실제로 크다.
여기에 msg_t 생애주기(`init`/`close`/`size`/`data`/`check`)가 RR 1,561 · DD 1,536 · STREAM 432 Ir/msg 로 얹힌다.

### 2.2 잠금 — 가장 큰 공통 비용

메시지당 `pthread_mutex_lock` **호출 횟수**: RR 16.98, DD 27.20, STREAM 17.34, PUBSUB 56.1, DRQ 120.4.
Ir로는 RR 1,078 / DD 1,874 / STREAM 1,138 Ir/msg(= 각 셀의 6.8 % / 14.3 % / 11.6 %).
같은 셀의 zmq는 **1.73회/msg**다(§3). `zlink::mutex_t::lock` 자체는 9.07회/msg(STREAM)뿐이므로
나머지 ~8회는 Core 밖 래퍼(part-helper 상태, asio 내부 std::mutex, 완료 상태 shared_ptr 경로)에서 온다.

## 3. STREAM zlink vs zmq — 같은 pull 모델, 메시지당 표

callgrind CCU 20 / 1024 B(서버만 계측). 컨텍스트 스위치·eventfd 는 valgrind 없이 native로 같은 구성(CCU 20, 10 s)에서
`/proc/<pid>/task/*/status`, `/proc/<pid>/io`를 샘플링해 얻었다(S-A §0 규칙: 이 카운터는 소켓 send/recv를 세지 않으므로 곧 eventfd 왕복 수다).

| 항목 | zlink | zmq | 비 |
|---|---|---|---|
| **Ir / msg** | **9,808** | **6,985** | **1.40×** |
| `pthread_mutex_lock` 호출/msg | **17.34** | **1.73** | **10.0×** |
| mailbox `send`/msg | 2.015 | 1.730 | 1.17× |
| mailbox `recv`/msg | 1.766 | 2.358 | 0.75× |
| `signaler_t::send`(eventfd write)/msg | 0.439 | 0.343 | 1.28× |
| `signaler_t::recv`(eventfd read)/msg | 0.762 | 0.343 | 2.22× |
| `epoll_wait`/msg | 0.542 | 0.688 | 0.79× |
| `epoll_ctl`/msg | 0.133 | 2.004 | 0.07× |
| TCP `recv`/msg | **2.001** | **1.001** | **2.00×** |
| TCP `send`/msg | 1.000 | 1.000 | 1.00× |
| `malloc`/`free` per msg | 1.391 / 1.567 | 2.027 / 2.148 | 0.69× / 0.73× |
| `clock_gettime`/msg | **2.025** | 0.073 | 27.7× |
| — native(비-valgrind) CCU 20 — | | | |
| 처리량 (msg/s) | 46,403 | 53,417 | 0.87× |
| vol ctxsw / msg | **0.619** | 0.505 | 1.23× |
| eventfd read(syscr)/msg | **0.854** | 0.455 | 1.88× |
| eventfd write(syscw)/msg | **0.711** | 0.455 | 1.56× |

**읽는 법.** zmq도 mailbox·command·eventfd를 쓰는 같은 pull 모델이므로 **핸드오프 자체는 격차의 원인이 아니다**
(mailbox 횟수는 오히려 비슷하고 recv는 zlink가 적다). 격차는 세 항목에 몰려 있다:
1. **잠금 10배**(17.3 vs 1.7회/msg, ~1,138 Ir/msg).
2. **TCP recv 2배**(2.0 vs 1.0회/msg) — read drain 루프 + speculative read(S-A §2와 같은 원인, 다만 이번 판은 3.0이 아니라 2.0회).
3. **eventfd 왕복 1.6–2.2배**(native 기준 write 0.71 / read 0.85 vs 각 0.455) — 이것이 vol ctxsw 1.23배로 직결된다.
반대로 zlink가 **유리한** 항목도 분명하다: `epoll_ctl` 0.13 vs 2.00, malloc/free 0.7배, mailbox recv 0.75배.

## 4. 패턴 고유 비용

| 패턴 | 고유 상위 심볼(Ir/msg) | 축소 방법(계약 §4.1 불변) | D |
|---|---|---|---|
| **ROUTER**(RR single) | `router_recv_part_impl` 342, `recv_router_message_direct` 249, `take_staged_router_recv_part` 229, `send_direct_with_retry` 229 | staged RID 사본과 `take_staged_*`의 재조회를 recv 1회당 1회로 접기(현재 recv 경로가 handle_state를 두 번 훑는다). 관측 가능한 RID/순서 계약은 불변 | — |
| **DEALER**(DD multi) | `recv_dealer_message_direct` 308, `session_base_t::push_msg_internal` 384, `reserve_decoder_frame` 370, `zmp_decoder_t::size_ready` 365, `_Rb_tree::find(transport_pair_pipes_t)` 235 | (a) 디코더 프레임 예약을 프레임당 1회로 통합, (b) `transport_pair_pipes_t` 조회를 std::map → 파이프 포인터 캐시로. 둘 다 순수 내부 자료구조 | — |
| **PUBSUB**(B) | `xpub_t::xsend` 3,040, `_Rb_tree_increment` 1,799, `zlink_publish_part` 2,197 | 구독자 목록 순회가 매 publish마다 red-black tree를 재순회한다. dist 스냅샷을 구독 변경 시에만 재빌드하도록. **B등급 수치라 이득 추정치는 재측정 후에만 신뢰** | — |
| **REQREP**(DRQ, C) | `recv_router_message_direct` 2,554, `public_router_reply_submit` 2,023, `send_public_router_reply_with_wait` 1,309, `epoll_wait` 2.93회/msg | 완료 대기 경로가 응답 1건마다 poller를 다시 돌린다(2.93 epoll_wait/msg는 다른 셀의 60배). C등급 — **먼저 측정 가능한 셀을 만들어야 한다**(G-6) | — |
| **STREAM** | `prepare_output_buffer` 185, `start_async_read` 153, `wait_for_completion_submit_admission` 144, `stream_t::xsend_routed` 121 | §3의 recv 2회/msg 축소 | — |

## 5. 하네스 오염 — 측정 신뢰도에 직결

`getenv`가 RR 셀 self Ir의 **6.96 %(1,135 Ir/msg)**, PUBSUB 9.20 %(4,532), DRQ 7.74 %(6,436)를 차지한다.
호출자를 역추적하면 **전부 벤치 하네스**다(`perf_single_one_way::run_measurement_phase`,
`recv_router_router_header_flags` 등에서 메시지당 각 1회, 합계 2.03회/msg). Core `libzlink`의 `getenv`는
전부 파일 스코프 const/캐시라 핫패스에 없다. 즉 **현재 single·multi 셀의 공개 수치에는 메시지당 ~1.1 k Ir(≈7 %)의
벤치 자체 비용이 섞여 있다**. 환경변수가 클수록(perf 러너가 PERF_* 를 많이 export 한다) `getenv` 1회가 559 Ir까지 든다.

## 6. Job 목록 — 예상 이득 순

| job | 원인 | 예상 이득 | 파일 범위 | 검증 | 스펙 |
|---|---|---|---|---|---|
| **G-1** | 공개 send/recv 경로의 mutex 획득 17–27회/msg (zmq 1.73) | 셀당 **600–1,200 Ir/msg**(4–9 %) | `core/src/api/socket/part_helper_api.cpp`, `core/src/runtime/sockets/common/socket_base*.cpp`, `core/src/runtime/core/pipe.cpp` | `ctest -R socket_base|part_helper`, perf/c single+multi 1024 B | 04-socket §4.1, 05-polling |
| **G-2** | `msg_t` 생애주기 `init/close/size/data/check` 432–1,561 Ir/msg | **300–700 Ir/msg** | `core/src/runtime/core/msg.cpp`, `core/include/.../msg.hpp` | `ctest -R msg`, hotpath_gate | 06-message |
| **G-3** | pipe/ypipe layering 695–864 Ir/msg (S-13의 537보다 크고 전 패턴 공통) | **200–400 Ir/msg** | `core/src/runtime/core/pipe.cpp/.hpp`, `ypipe.hpp` | `ctest -R pipe`, perf/c 4패턴 | 05-polling, 08-stream §5 |
| **G-4** | STREAM TCP `recv` 2.0회/msg (zmq 1.0) — read drain + speculative read | syscall 1회/msg 제거 ≈ **CPU 5–8 %** | `core/src/runtime/engine/asio/asio_engine.cpp`(read drain/spec read), `asio_stream_fastpath_policy.hpp` | `bench/with_stream` zlink vs zmq, `ctest -R stream` | 08-stream |
| **G-5** | 벤치 하네스 `getenv` 2회/msg가 공개 수치의 ~7 % 오염 | 측정 정확도(수치 자체 −7 %) | `bindings/c/perf/single/common/perf_single_one_way.hpp`, `bindings/c/perf/single/src/perf_router_router*.cpp`, `bindings/c/perf/multi/common/*` | perf/c 재측정 후 baseline 갱신 | — (bench only) |
| **G-6** | single ROUTER_ROUTER_REQREP가 callgrind에서 완료 0건 — −6 % 셀을 프로파일할 수 없다 | 이 셀의 원인 규명 가능해짐 | `bindings/c/perf/single/common/perf_single_reqrep.hpp`, `core/src/runtime/sockets/common/socket_base_completion*.cpp` | 축소 셀이 완료를 내는지 | 07-reqrep |
| **G-7** | eventfd 왕복 native 0.71 w/0.85 r per msg (zmq 0.455/0.455) → vol ctxsw 1.23× | **ctxsw 20 %**, CPU 2–4 % | `core/src/runtime/core/mailbox.cpp`, `signaler.cpp`, `socket_base_t::process_commands` | with_stream zlink vs zmq ctxsw 표 | 05-polling |
| **G-8** | REQREP 완료 대기가 응답당 `epoll_wait` 2.93회 | (C등급) G-6 뒤 재추정 | `core/src/runtime/sockets/common/socket_base_completion*.cpp`, `poller` | multi DR_REQREP | 07-reqrep |
| **G-9** | PUBSUB dist 순회가 publish마다 rb-tree 재순회(`_Rb_tree_increment` 1,799 Ir/msg) | (B등급) 재측정 뒤 추정 | `core/src/runtime/sockets/pubsub/xpub.cpp`, `dist.cpp`, `mtrie.cpp` | multi PUBSUB | 09-pubsub |

D(설계 결정) 필요 항목은 없다 — G-1~G-9 모두 관측 가능한 계약을 바꾸지 않는 내부 구현 변경이다.
다만 **G-5·G-6은 다른 job보다 먼저** 처리해야 한다: 전자는 모든 셀 수치의 7 %를,
후자는 −6 % 셀 자체의 측정 가능성을 좌우한다.
