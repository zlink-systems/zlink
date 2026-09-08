# ALL-1 — 누적 Core 수정·검증

A·C·D·E 구현을 마쳤고 B의 mask·batch·executor 수정과 비율 회귀 검증을 완료했다. B의 공통 큰 payload 할당 비용·왕복 지연·RSS는 남아 있어 B 전체는 부분 완료다. 최종 TSan210/210·경고0, 관련348/348·신규120/120·lost-wake80/80이다. ASan은 기존 disconnect assertion/계약 충돌 때문에12/13이며, 최종 C multi RR1024B는 이전 기준보다16.07% 낮다.

- 기준: main `25355fcfc5` 이후 독립 구현, detached worktree `/home/hep7hep7/project/zlink-work/all`.
- 시작: 2026-09-07 23:01:57 UTC. 종료: 2026-09-08 03:14:33 UTC. 상한8시간 이내(약4시간12분).
- 변경은 미커밋 상태다. 다른 worktree는 읽기만 했다. stash·commit·spec 변경 없음.
- 로그·계측: `/home/hep7hep7/project/zlink-work/all-artifacts/`.
- 아래 source 경로는 worktree 기준이다. `runtime/`는 `core/src/runtime/`다.

## 항목별 결과

| 항목 | 상태 | 변경·근거 | 규칙 수 전/후 | 검증 |
|---|---|---|---|---|
| A-1 public receive·async·command 소유권 | 완료 | `runtime/sockets/common/socket_runtime.hpp:280`, `socket_lifecycle_runtime.cpp:58`: 기존 lifecycle turn 하나로 통합. 별도 receive owner word·fallback mutex·command owner mutex를 제거 | receive/command exclusion 3종→1종 | STREAM/DEALER/ROUTER 신규 회귀 20회 완료, 최종 전체 TSan warning0 |
| A-2 command 전용 상태·fallback 재검증·async 종료 | 완료 | `socket_base_lifecycle.cpp:431`: command batch 전체가 같은 turn을 소유하고 command_drain_active로 command dispatch를 표시. fallback 자체가 없어 재검증·available 전환의 이중 규칙도 없음. command 표식 guard도 turn 내부에서 파괴되어 다음 owner의 표식을 덮지 않음 | fallback 진입/재검증/종료 규칙 3→0 | reconnect 정책 TSan 20회 통과 |
| A-3 atomic progress·CV·notify | 완료 | `socket_runtime.hpp:320`, `socket_base_lifecycle.cpp:1586`: epoch·waiters seq_cst 교차 발행, 등록은 turn 안·대기는 밖. receive/submit/async/monitor CV는 plain mutex | progress publisher1 유지, CV recursive 종류4→0 | 최종 lost-wake80/80 통과 |
| A-4 has_in·control attach·record 경계 | 완료 | `socket_base_api.cpp:261,1017`, `socket_base_msg.cpp:53`, `stream.cpp:745`: endpoint의 직접 attach와 xattach, readiness, packet header/body를 같은 turn에 포함 | receive·command·public 배타 장치3→1 | 관련 116 target×3 통과 |
| A-5 회귀 수명·동시 close | 완료 | `core/tests/integration/test_stream_concurrent_pull_send.cpp:169,505`: client별 Asio context/socket과 취소·drain을 같은 thread에서 소유. 모든 thread join 뒤 close. STREAM bounded/unbounded + DEALER/ROUTER | client별 실행/종료 소유자1 유지, 외부 동시 close 경로1→0 | dev 20회·ASan 통과 |
| B-1 원인 특정 | 완료 | WS-1 분석의 callgrind 근거 직접 확인. 64 KiB mask client27.89%/server48.19%, 호출당 wide XOR Ir 약78% 감소 | 원인 가설→mask 위치 확정 | `core-rf-WS-1-analysis.md` |
| B-2 mask·비율 회귀 | 완료 | `core/external/boost/boost/beast/websocket/detail/mask.ipp:46`: alias/alignment-safe 8-byte XOR. `bindings/c/perf/ws_roundtrip_gate.py`: 기존 report parser로 WS·WSS 각각 Q64/Q1≥0.80 | 별도 codec/option 0→0; parser 소유자1 유지 | mask 1,120개 입력 조합, Python23/23; 최종 WS1.851217/WSS2.585461≥0.80; TCP 대조 변동 한계 포함 |
| B-3 batch·executor 비용 | 완료 | `runtime/transports/ws/ws_batch_policy.hpp:13`, `ws_transport_common_internal.hpp:26,41`: encoder와 client masking scratch의 기본 상한128KiB로 통합. 실제 소유 io_context executor 타입을 WS/WSS 내부 socket에 유지 | 독립 기본 상한2→1, executor type erasure 경계1→0 | Release12셀 완료·비율 통과. 절대 처리량·지연·RSS 한계는 아래에 명시 |
| B-4 남은 큰 payload 할당·지연 | 부분 | `runtime/protocol/zmp_decoder.cpp:275,286`, `decoder_allocators.cpp:196,348`, `decoder.hpp:93`: allocator callback/refcount는 재사용 가능하지만 현재 read 입력을 소비하기 전 buffer 교체는 수명 위반. 기존 API의 직접 치환을 기각 | 새 cache·deferred-growth 상태0 유지 | mprotect steady stack·allocator 제어 진단, 코드 직접 재검증. 새 입력 보존 상태/별도 storage 구현 안 함 |
| C-2a socket turn CAS | 완료 | command마다 receive.sync 제거. completion drain/owner gate도 turn으로 합쳐 `_completion_owner_sync` 제거. WRITABLE route 확인·wait 등록·재확인에 기존 scope 하나 재사용 | socket exclusion 3종→1종 | 전체 dev 실패 원인 수정·개별 통과, lock/msg8.386 |
| C-2b socket pipe _out_sync | 완료 | `runtime/core/pipe_write.cpp:160,806`: socket-end writer는 C2 turn, session-end writer는 I/O thread. foreign HWM/max 값은 C3 atomic, context 회계는 기존 published snapshot 재사용 | hot _out_sync1→0; cold transport gate1 유지 | 최종 TSan warning0·hotpath 요청5/5 통과 |
| C-2d route shard snapshot | 완료 | `runtime/sockets/stream/stream.hpp:89`, `stream.cpp:146`: 불변 map snapshot과 단일 pipe lifetime 참조. lookup은 turn 아래, shard mutex 없음 | shard별 hot mutex64→0 | STREAM 회귀·ASan 통과, lock/msg8.386 |
| D-1 _slot_sync recursive+CV | 완료 | `runtime/core/ctx.hpp`, `ctx.cpp:232`, `ctx_termination.cpp:51`: plain registry mutex, 기존 mailbox ref snapshot 뒤 registry 밖 monitor/stop. 생성자8곳의 재진입 schedule을 ctx publish 후1곳으로 통합 | 생성 후 schedule 소유자8→1 | ctx focused·ASan 통과 |
| D-2 mailbox::send 예외 | 완료 | `runtime/core/mailbox.cpp:65,354`: RAII 정상 unlock, void enqueue의 기존 fatal allocation 정책을 post에도 noexcept로 통일. 별도 복구 owner/state/재시도 없음 | enqueue/post 예외 정책2→1 | one-shot post bad_alloc child가 SIGABRT인지 검증, dev20회·ASan 통과 |
| D-3 receive_once_guarded race | 완료 | A-1/A-4와 동일 수정 | 별도 보상 규칙0 | A 검증 공유 |
| D-4 pipe 파일 분할 | 완료 | `pipe.cpp`, 신규 `pipe_receive.cpp`, `pipe_write.cpp`, `pipe_transport.cpp`, `pipe_internal.hpp`; CMake 등록 | 분할 자체 동작 규칙 추가0; body196/196 동일 | 분할 직전 qualified body196개 SHA 비교196/196 통과; 이후 reply C2 변경은 별도 검증 |
| D-5 WS/WSS twin | 완료 | `runtime/transports/ws/ws_transport_common_internal.hpp`: binary/no-fragment/options/fd/read/write/errno 공통화. WSS의 TLS/SNI/handshake는 TLS 모듈 유지 | 공통 transport 규칙8×2→8 | WS/WSS integration·unit·ASan 통과 |
| D-6 lb::sendpipe 중복 | 완료 | `runtime/sockets/internal/lb.cpp:444`: 사용하지 않는 send wrapper 제거. 기존 unit10개 호출을 sendpipe로 통일, 기대값 불변 | 같은 의미 진입점2→1 | router pipe unit·ASan 통과 |
| D-7 R7 #4/#7 | 완료 | `runtime/core/socket_poller.cpp:101,110`, `core/src/api/core/close_result_internal.hpp:18`, `core/src/api/message/connect_result_internal.hpp:15`: poller socket/fd 등록은 lifetime pin·index·rollback이 달라 유지. errno mapper는 enum별 계약 owner 유지; close EDEADLK→BUSY, connect EPROTONOSUPPORT→NOT_SUPPORTED 누락 수정 | mapper 소유자1 유지, 누락2→0 | result enum unit·ASan 통과 |
| D-8 R11-B | 완료 | `runtime/utils/ip_fdpair.cpp:287`: 지원하지 않는 OpenVMS/VxWorks 분기103행 제거. 지원 플랫폼 경로는 유지 | 미지원 플랫폼 분기2→0 | dev build·전체 suite |
| E-1 monitor/ctx lock-order 3 target | 완료 | `socket_base_monitor.cpp:839`: 기존 operation_sync로 교체 직렬화, monitor.sync 밖에서 worker·foreign socket 종료. stop_monitor를 process_stop command turn에서 stop control 경계로 이동 | monitor mutex 아래 foreign wait1→0 | 전체 TSan의 세 target 통과 |
| E-2 lb peer-weight | 완료 | `runtime/sockets/dealer/dealer.cpp:533`, `runtime/sockets/router/router_admission.cpp:483`: DEALER/ROUTER test-only 관측자가 기존 socket turn을 재사용. production LB writer의 C2 소유권 유지 | 보호 없는 관측1→0 | flow unit dev·TSan focused 통과 |
| E-3 async mailbox consumer | 완료 | detach/reschedule/activate를 동일 lifecycle turn 아래로. completion owner mutex 역순 교착도 같은 turn으로 제거 | mailbox consumer exclusion2종→1종 | reconnect TSan20/20; 최종 전체 TSan warning0 |

## 설계 비교와 선택

- A/C: ST의 별도 receive owner protocol을 보강하는 안과 lifecycle turn으로 합치는 안을 비교했다. 후자는 command·receive·completion이 같은 상태를 만지는 모든 경로에 규칙 하나를 적용하고 mutex fallback 상태 자체를 없앤다. 스펙11 §3.1/§3.4/§5/§6에 따라 후자를 채택했다. 최종 guard 리뷰는 독립 agent의 의견을 감독이 선언/파괴 순서와 대기·reconnect 호출 경로에서 직접 재검증해 수용했다.
- B: mask만 수정한 안은 WS/WSS 크기 비율이 각각0.275/0.567로 실패했다. encoder16KiB와 client masking scratch64KiB의 독립 기본 상한을128KiB 한 곳으로 합친 안은0.735/1.153이었다. 최종안은 이미 connection을 소유하는 io_context의 구체 executor 타입까지 유지하여0.842/1.178로 통과했다. ZMP §9는 현재 준비된 byte의 bounded batch를 규정하며 고정 크기를 규정하지 않는다. 기다리는 시간·재시도·공개 옵션은 추가하지 않았고, 기존 환경변수 override는 유지했다. RSS 증가와 절대 처리량 변동을 감수하는 선택이며, 아래 수치를 함께 검토해야 한다.
- C2b: cold writer 때문에 hot writer mutex를 유지하는 안과 실제 공유 scalar만 발행하는 안을 비교했다. HWM/max·회계만 C3로 발행하고 C2 writer에는 turn을 재사용했다(스펙11 §3.2/§4). Reply의 single/multipart generation lock도 제거했다. 기존 connection ID의 acquire 관측으로 admission을 선형화하고, 동시 retirement 뒤 늦게 발행된 part는 기존 stamp와 session의 record 단위 폐기가 처리한다(`session_base_pipe_io.cpp:11-32`, ZMP §4의 이전 ID/generation 폐기, Socket 공통 §6 completion). 새 handshake/state/CAS는 없다. ID 초기화 전용 helper도 기존 setter에 통합했다.
- D: poller/enum mapper를 macro/template로 합치는 안은 lifetime·rollback·반환 enum의 차이를 새 분기로 옮기므로 기각했다. 의미가 같은 WS/WSS 동작과 lb wrapper만 통합했다.
- 최종 A 리뷰: command_drain_active guard가 turn 바깥에서 파괴되면 이전 command owner가 다음 owner의 true 표식을 false로 덮을 수 있었다. api_owner 뒤에 guard를 두어 모든 return에서 표식부터 해제한다. 별도 atomic counter를 추가하는 안 대신 기존 scope를 바로잡았고 수동/RAII 해제2경로를 RAII1경로로 줄였다(`socket_base_lifecycle.cpp:238,423`).
- E: completion mutex의 잠금 순서만 보완하는 안 대신 중복 mutex를 제거했다. gdb에서 확인한 turn→mutex / mutex→turn 교착이 사라졌고 기존 reconnect 전체 케이스가 TSan20회 통과했다.

## 검증 표

| 검증 | 결과 | 로그 |
|---|---|---|
| dev build | 통합·reply generation·ctx/probe·WS 성공, 최종 command guard138/138 성공 | `dev-command-claim-final-build.log` |
| 전체 ctest -E hotpath_gate 1회 | 206/210,263.32s. 실패4개는 아래에 원인과 수정 기록 | `dev-full-ctest.log` |
| 관련 regex until-fail:3 | 최종 command guard 포함116×3=348/348,411.82s | `dev-command-claim-related-repeat3.log` |
| 신규·변경 until-fail:20 | 최종6target×20=120/120,46.63s. command fairness 선택 case dev20/20,0.10s·ASan20/20,0.56s. owners 전체 반복은 D-ALL-1 실패 이력 유지 | `dev-command-claim-new-repeat20.log`, `dev-command-claim-fairness-repeat20.log`, `asan-command-claim-fairness-repeat20.log` |
| 최종 WS·신규 until-fail:20 | 5target×20=100/100,134.33s | `dev-ws-final-repeat20.log` |
| lost-wake until-fail:20 | 최종4×20=80/80,633.36s | `dev-command-claim-lost-wake-repeat20.log` |
| TSan 전체 | 최종210/210,367.38s; TSan warning0·suppression0. packet4.14s·owners1.72s 통과 | `tsan-command-claim-final-full-ctest.log` |
| ASan 신규·변경 | WS 단계13/13,12.24s. 최종 command guard 단계12/13,27.02s; 실패는 D-ALL-1이고 assertion 탈출 뒤68,454B/3alloc leak 보고. 변경 fairness case20/20,leak0 | `asan-ws-final-13-tests.log`, `asan-command-claim-final-13-tests.log`, `asan-command-claim-fairness-repeat20.log` |
| Python perf gate | WS/WSS23/23 | `ws-wss-gate-python-tests.log` |
| 공개 인터페이스 | core/include·libzlink.vers diff0,4언어×8 raw header32/32 동일(상위3종×4=12포함) | `public-interface-check.json` |
| pipe 분할 | qualified body196/196 SHA 일치 | `pipe-split-body-compare.txt` |

전체 dev에서 발생한 4개 실패의 원인은 다음과 같다. 기대값을 낮추지 않았다.

1. helper_ownership / stream_send_blocking_wakeup: WRITABLE 등록의 route 존재 검사가 turn 밖이었다. 기존 readiness scope를 앞당겨 같은 turn으로 통합. 수정 후2개 모두 dev·TSan 통과.
2. ctx_lifecycle: before_gate 테스트 hook이 새 turn 뒤에 있어 대기 진입을 관측하지 못했다. hook만 turn 앞에 배치; 기대값 불변. 수정 후 dev·ASan 통과.
3. phase3_request_reply_owners: budget barrier가 turn을 잡은 채 같은 turn의 command pump를 기다렸다. 기존 contention/command probe로 command가 적용 전 차단됨을 확인한 뒤 barrier를 해제한다. 기존64개·ID·NOT_CONNECTED·replacement 기대값은 그대로다. 새 관측은 모든 명령이 turn을 소유했는지를 검증한다. 단일 busy snapshot으로 실행 순서를 강제하지 않는다. 수정 후 해당 fairness case20회와 ASan 통과; 전체 unit 반복에서 별개의 D-ALL-1 충돌이 나타났다.
4. 최종 ctx 반복에서 기존 batch drain counter의 false negative가 드러났다. counter 증가 후 같은 batch에 들어온 request_completion을 처리해도 값은 증가하지 않는다. gdb의 ypipe 상태는 빈 queue였다. 기존 command probe로 실제 request_completion 처리를 관측하도록 fixture만 수정했다. trigger·3초·assertion 불변, 수정 후20/20 통과.

TSan 최초 전체 실행의 추가 발견은 다음과 같다.

1. `options.rcvtimeo` setter와 completion pull getter 사이 race: timeout snapshot을 기존 turn 아래로 옮겼다. 같은 구조의 send getter와 receive retry snapshot도 동일 owner를 재사용한다. DONTWAIT는 timeout0을 직접 사용하므로 성공 hot path에 추가 snapshot/lock이 없다. `unittest_phase3_request_reply_owners` TSan focused1/1,1.72s 통과.
2. `test_router_mandatory_hwm`: `<10`/`<20`은 메시지 수 assertion이다(MP-3 보고서의 ms 표기는 소스와 다르다). OS-autotuned buffer가 이미 pipe에서 빠진 수십 개의64KiB record를 흡수했다. 기존 backpressure fixture와 같은 `SNDBUF/RCVBUF=4096`을 bind/connect 전에 적용했다. HWM·assertion·1초 대기는 불변이다. 수정 후 TSan5/5,10.26s 통과. 이전 mp2 binary의 isolated 실패(12/43, agent 관측)와 compiler 부하 중 root 재실행 통과도 기록했다.
3. `test_stream_packet_progress/test_shutdown_during_drain`: shutdown 시작 전262,153개1-byte WS frame을3초 안에 적재하는 fixture 전제에서 실패. ST-1은 source 원복 대조로 같은 실패를 확인했고 ST-3도 같은 서명이다. 앞선 TSan은 warning 없이 해당 대기 assertion만 실패했고, 최종 전체 TSan은4.14s로 통과했다. 최종 dev·ASan도 통과했다. timeout·기대값·입력 조건을 완화하지 않았다.

기존 debt target의 최종 TSan 시간은 reject_duplicate6.53s, reject_disconnected_without_app_recv1.43s, same_socket_reconnect_policy3.87s, flow_state_socket2.67s, ctx_lifecycle0.18s다.

TSan은 CMake ENABLE_TSAN의 기존 비계측 옵션을 사용하지 않았다. ENABLE_TSAN=OFF와 명시적 `-fsanitize=thread -fno-omit-frame-pointer`로 Core와 모든 test를 계측했다. `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`, `setarch x86_64 -R` 사용. suppression 파일·no_sanitize 추가 없음.

## 성능 표

| 측정 | 기준 | 결과 |
|---|---|---|
| Release lib | LTO ON, JOBS=4 | 통합·WS executor 성공; 최종 command guard lib·hotpath 증분5/5 성공 |
| hotpath 5셀 | repository reference,±5%; 개선은 표기 | DD −4.09%, DR_REQREP −5.57%, PAIR +3.55%, RR_TCP +1.99%, STREAM_TCP −0.44%; 요청 판정5/5 통과 |
| stream_tcp lock/msg | 15.1→목표약8.3 | 8.3861(537,372 locks/64,079 messages); CCU20·1024B·10s·io4. 기준 G1 dev, 현재 Release 차이 명시 |
| with_stream | zlink/asio/zmq,64/1024/65536B,CCU1000,runs1 | 9/9,mismatch0; zlink294.245/265.737/35.589kops, asio370.206/339.645/43.295, zmq338.673/315.096/28.408; 시작load0.574,ninja0 |
| C multi1024B3셀 | DD/DR_SENDSEND/RR_SENDSEND | 최종990.372/296.542/203.540kops; 기존 phase2g905.1/273.9/242.5 대비+9.42/+8.27/−16.07%. RR 하락과 실행 변동은 아래에 기록 |
| WS·WSS 크기 비율 회귀 | Q=(WebSocket왕복/TCP왕복)/(WebSocket단방향/TCP단방향),각 Q64/Q1≥0.80 | 최종 WS1.851217/WSS2.585461 PASS; 12cells·60/60 RESULT,85.57s,skip/fail/unsupported0. TCP 왕복64KiB 하락의 분모 영향도 명시 |

## D — 계약 변경 필요 항목

| 항목 | 스펙 조항 | 필요한 계약 변경 | 판정 |
|---|---|---|---|
| D-ALL-1: 기존 repeat disconnect 기대값 | Socket 공통 `zlink_disconnect_rid`, `README.ko.md:909-921` | 기존 unit `unittest_phase3_request_reply_owners.cpp:1013-1015`는 두 번째 호출도 OK를 요구하나, 비동기 종료가 route를 제거하면 계약상 NOT_FOUND다. 항상 OK를 만들려면 없는 대상의 idempotent 성공 계약이 필요 | runtime·기존 assertion 모두 변경 안 함. 테스트의 기대값 개정은 감독관 별도 판단 필요 |

## WS 성능·원인 근거와 측정 한계

같은 실행의 TCP를 기준으로 Q(size)=(WS 또는 WSS 왕복/TCP 왕복)/(WS 또는 WSS 단방향/TCP 단방향)를 계산한다. 절대 처리량 기준을 새로 만들지 않고 Q64/Q1≥0.80으로 payload 크기에 따른 추가 손실을 검증한다. gate의 임계값·reference는 측정 중 변경하지 않았다. 각 재측정은 아래 실제 코드 변경 뒤1회이며 같은 코드를 통과할 때까지 반복하지 않았다.

| 구현 | WS Q64/Q1 | WSS Q64/Q1 | WS64KiB 왕복 kops/s | WSS64KiB 왕복 kops/s |
|---|---:|---:|---:|---:|
| 8-byte mask | 0.274792 FAIL | 0.566500 FAIL | 6.258 | 8.312 |
| + batch/scratch 기본128KiB | 0.735192 FAIL | 1.153293 PASS | 19.977 | 9.216 |
| + 구체 io_context executor | 0.842130 PASS | 1.177982 PASS | 13.912 | 9.641 |
| + command claim scope 종료 보완, 최종 | 1.851217 PASS | 2.585461 PASS | 14.132 | 9.146 |

최종 셀은 다음과 같다. 처리량 단위는 kmsg/s 또는 kops/s이며 latency는 report의 ms 값이다.

| 패턴·transport | 1KiB 처리량 | 64KiB 처리량 | 1KiB latency ms | 64KiB latency ms |
|---|---:|---:|---:|---:|
| DD tcp | 990.372 | 81.367 | 1.237 | 17.319 |
| DD ws | 1088.816 | 56.291 | 0.747 | 28.744 |
| DD wss | 890.741 | 26.917 | 2.086 | 70.165 |
| DR_SENDSEND tcp | 296.542 | 16.098 | 0.823 | 102.460 |
| DR_SENDSEND ws | 223.467 | 14.132 | 4.907 | 441.622 |
| DR_SENDSEND wss | 177.169 | 9.146 | 526.066 | 419.634 |

- 최종 report: `final-perf/multi-ws/multi/report/perf_c_multi_linux_20260908_120750_ALL-1-final.txt`, gate `final-perf/ws-ratio-gate.log`. WS64KiB 왕복14.132k는 직전13.912k와 가깝지만 TCP 대조는36.157k→16.098k로 낮아졌다. 따라서 최종 Q 상승에는 분모 하락이 크게 작용했고 WS 자체의 추가 향상으로 해석하지 않는다. 구체 executor의 안정적인 절대 성능 향상을 증명하지도 않는다. 최종 multi 시작load2.117, RR 시작load3.671을 그대로 남긴다(직전 with_stream·multi 부하가 포함된1분 평균).
- callgrind의 mask-only64KiB·4client 진단에서 server mask26.110M/117.259M Ir(22.27%), client의 이름 없는 libc 심볼181.511M/284.714M(63.75%)를 확인했다(`ws-residual-{client,server}.callgrind`). 심볼 이름을 memcpy로 단정하지 않았다.
- 부하 중 `mprotect` stack은 libc malloc→`msg_t::init_size`→`zmp_decoder_t::size_ready`였다(`allocation-mprotect-steady-stack.log`). allocator의 heap 반환 영향을 분리한 별도 진단에서만 `MALLOC_TRIM_THRESHOLD_=1073741824`를 사용했고, WS64KiB 왕복25.471kops/s·latency437.625ms였다. 이 환경변수나 allocator 정책은 patch·최종 성능 실행에 넣지 않았다.
- RSS는 batch128KiB 단계의 외부 sampler로 관측했다. WS64KiB server 최대1747MiB, WSS1KiB server1878MiB였고 TCP64KiB server146MiB였다(`ws-batch-rss-samples.jsonl`). 큐에 쌓이거나 allocator가 보유한 payload가 포함되므로 전부 batch 변화의 결과라고 단정할 수 없다. 고정 buffer 기본값 자체의 증가도 encoder112KiB와 client masking scratch64KiB로 연결당 최대176KiB이며, 연결1000개의 client 쪽에는 약172MiB 추가 가능성이 있다. 최종 executor 단계의 별도 RSS 대조는 하지 않았다.
- 큰 message를 기존 shared allocator에 바로 옮기는 대안도 검토했다. 단일 생성자는 `_max_size`를 초기 read target에 고정하고 `set_allocation_size`는 그 값으로 clamp한다(`decoder_allocators.cpp:196-205,348-352`). 그 제한만 풀어도 `size_ready`에서 현재 buffer를 교체하면 `decoder.hpp:93-111`이 계속 읽는 같은 transport chunk와 admission retry의 입력 포인터 수명이 끊긴다. 독립 리뷰의 이 근거를 감독이 직접 확인해 즉시 치환안을 기각했다. 기존 refcount callback 자체의 cross-thread close 소유권은 재사용할 수 있지만 입력을 보존하는 allocation 순서·상태 설계가 더 필요하다. 새 cache·deferred-growth 상태나 별도 temporary storage를 이번 제약 아래 추가하지 않았다. 내부 constructor plumbing 자체를 공개 API 변경으로 간주한 것은 아니다. 현재 메모리 스펙이8KiB 크기를 고정한다는 근거도 없으므로 이 미구현은 D(spec gap)가 아니라 제약 아래 남긴 성능 위험이다.

## hotpath·readiness·with_stream 보충 수치

| hotpath 셀 | reference Ir/msg | 측정 Ir/msg | 변화 |
|---|---:|---:|---:|
| DEALER_DEALER inproc | 3230.922 | 3098.85265 | −4.09% |
| DEALER_ROUTER reqrep inproc | 16455.383 | 15538.1606 | −5.57% 개선 |
| PAIR inproc | 2348.457 | 2431.88465 | +3.55% |
| ROUTER_ROUTER tcp | 2972.5318 | 3031.7857 | +1.99% |
| STREAM tcp | 13969.806 | 13908.6942 | −0.44% |

기존 gate 도구는 양방향±5%로 판단하여 reqrep의5.57% 개선도 FAIL로 표시했다. reference나 도구 판정을 바꾸지 않았으며, 사용자의 “개선은 표기만” 기준으로 회귀 상한+5%는5/5 통과다. 최종 코드 로그는 `final-perf/hotpath.log`, 원본은 `final-perf/hotpath-callgrind/`다. 최초 측정 뒤 WS와 command guard의 실제 코드 변경이 있어 마지막 코드로 한 번 재측정했다. 최초 수치도 `hotpath-final-gate.log`에 보존했다.

STREAM 계측에서 실제 pthread mutex537,372회/64,079message=8.3861회였다. `lock_public_api_sync`는 CAS turn 함수로3.015호출/msg이며 pthread mutex 횟수가 아니다. `xhas_in`0.289호출/msg·`pipe::check_read`0.146호출/msg를 관측했다(`final-perf/stream-readiness-calls.log`). C multi1024B3셀 처리량은 위 표에 기록했으며 해당 실행에서 별도 함수 프로파일링은 하지 않았다. has_in 내부 비용과 TCP 네트워크·스케줄링 비용을 처리량만으로 분리할 수 없다.

with_stream은 최종 시작load0.5737·ninja0에서 PERF_LOCK을 잡고1회 실행했다(103.75s). Phase0 zlink268.9/243.0/30.4kops 대비294.245/265.737/35.589kops로 각각+9.43/+9.36/+17.07%다. zlink/asio 비율은0.795/0.782/0.822이다. 서로 다른 실행의 기준 비교와 runs1의 변동 한계가 있다. 최종 로그 `final-perf/with-stream.log`, 결과 `final-perf/with-stream/comparison.md`·`summary.json`·`metrics.csv`.

C multi1024B의 최종 RR은203.540kops/s·0.947ms로 phase2g242.5k 대비16.07% 낮다. 초기 ALL-1 실행은303.326k였으므로 실행 간 변동도 크다. 이 하락을 숨기거나 hotpath Ir 통과로 대체하지 않는다. DD/DR은 각각990.372/296.542k로 phase2g 대비+9.42/+8.27%다. 최종 RR 로그 `final-perf/multi-rr.log`, 원본 report `final-perf/multi-rr/multi/report/perf_c_multi_linux_20260908_120917_ALL-1-final.txt`.

## 누적 patch

- 파일: `/home/hep7hep7/project/zlink-work/all-artifacts/ALL-1-cumulative.patch`.
- 기준 HEAD: `25355fcfc59afb998fd3d04f3d38f2a71ee4ff11`.
- 64파일,5581행 추가·4419행 삭제,503225bytes. 신규8파일 포함.
- SHA-256: `b569fdad0c65e1010d73647fd025d3569d7063e59c869876cd55517a85b6dac0`.
- 실제 index를 건드리지 않는 임시 index에 기준 HEAD를 읽어 `git apply --cached --check` 통과. `git diff --check` 통과. 미커밋이며 다른 트랙과 merge/rebase하지 않았다.

## 남은 위험

1. 반복 disconnect의 기존 assertion과 공개 계약 충돌(D-ALL-1)이 남았다. runtime이나 기존 assertion을 바꾸지 않았다. 최종 ASan에서도 이 assertion이 실패했고 이후68,454B/3allocation leak이 보고됐다. PTY로 같은 case를 until-fail:20 실행하여5번째에 `Expected 0 Was 605`와 같은 leak을 함께 확인했다(`asan-disconnect-contract-pty-until-fail20.log`). Unity의 assertion 탈출은 C++ local destructor를 실행하지 않으며, 실패 케이스가2개 socket을 남겨 강제 정리한 경고도 기록됐다. 실패 없는 이전13-target 실행과 해당 case의 첫4회에는 leak 보고가 없었다.
2. TSan에서 shutdown 전262K개의 작은 WS frame을3초에 적재하는 기존 fixture 전제가 앞선 실행에서 실패했다. WS 최종 구현의 전체 TSan은210/210,407.27s로 통과했고 이 target은4.34s였다. 간헐 적재 시간 위험 이력은 남긴다. timeout·입력·assertion을 완화하지 않았다. command 표식 scope 보완 뒤 최종 전체 실행도210/210,367.38s로 통과했다.
3. WS/WSS의 크기 비율은 통과했으나 수백 ms의 왕복 지연과 높은 RSS는 해결됐다고 판단하지 않는다. 최종 코드의 이 성능·메모리 tradeoff는 감독관 리뷰 대상이다.
4. 최종 C multi RR1024B는 phase2g 대비16.07% 낮다. 같은 코드 단계들 사이에서도 큰 변동이 관측되어 처리량 회귀 부재를 확정하지 않는다. G1 lock/msg 기준은 dev, 현재 측정은 Release라는 차이가 있다. 성능은 각 구현1회 측정이며 장기간 변동 폭까지 증명하지 않는다.

소유 계층: Core socket lifecycle turn이 C2를, connection I/O thread가 session 끝을, transport가 WS framing·masking·executor를 소유한다. context registry와 발행 scalar는 각각 기존 registry lock·C3 atomic이 소유한다.

스펙 조항: synchronization11 §2(C1/C2/C3)·§3.1–3.5·§4·§5·§6, ZMP §4(old generation 폐기)·§9(bounded WebSocket batch), Socket 공통 §2·§6·disconnect_rid. atomic 추가는 타 thread 관측자와 waiter publication에 필요한 C3 발행으로 한정했다. 공개 signature·반환 계약·monitor event·completion record·poll level을 바꾸지 않았다.

교차언어 대조: Framework runtime 변경 없음. 모든 binding은 같은 Core ABI를 사용하며 C/C++/Go/Rust raw header32/32 동일, 요구한12개 mirror 포함. 언어별 우회는 추가하지 않았다.

변경 분류: A(기존 synchronization 계약 적응)+B(기존 결함 수정). C 우회 구현0, D 계약 충돌1건은 미구현으로 위 표에 남겼다.
