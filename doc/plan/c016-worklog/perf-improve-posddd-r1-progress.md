## 2026-09-03 15:08 KST — 초기 조건과 규범 확인

- 현재 branch는 사용자 지정 `perf/phase2-judge`다. 기존 변경 `doc/plan/c016-worklog/tools/sweep2.sh`는 사용자 작업으로 보존하고 수정하지 않는다.
- 필독 자료인 hot-path spec, POSDDD, Core module/source layout, ZLink 설계 원칙, 커밋 `f3be895b3f`·`1344022a3e`, phase 2 기록, D-036~D-055, 리팩토링 brief를 읽었다.
- 공개 API·ABI·계약과 금지 경로를 보존한다. 성능 변경은 측정과 callgrind로 실증하고, 구조 정리는 호출자와 소유 책임을 `rg`로 확인한 뒤 파일 단위로 분리한다.
- 다음 단계는 기존 report 수치 재구성, candidate/baseline 동일 조건 callgrind, PAIR send/recv·pipe/ypipe·transport framing 코드 대조다.

## 2026-09-03 15:24 KST — 수정 전 PAIR callgrind 실증

- 양 worktree의 `perf_pair.cpp`가 동일함을 확인하고 PAIR/inproc 65536B, 기본 2-part, throughput 1초 + in-flight-1 latency 1초를 같은 명령으로 역순 실행했다.
- 총 메시지는 `zlink_send_part` 호출 수에서 stop token을 제외해 계산했다. baseline은 2,091건/236,106,626 Ir = 112,916 Ir/message, candidate는 1,674건/219,663,454 Ir = 131,220 Ir/message로 candidate가 16.21% 많았다. payload `memcpy`는 65,541 대 65,564 Ir/message로 같아 원인에서 제외했다.
- candidate의 `get_events_internal`은 100,448회, self 74 Ir/call이고 baseline은 95,449회, self 48 Ir/call이다. candidate `has_in()`이 모든 POLLIN probe에서 `part_helper_state()`와 state mutex를 조회해 lock/unlock/shared_ptr 비용을 추가한다. `part_helper_state()`만 1,693,504 Ir/100,448회였다.
- 스펙 §3.3·§4에 따라, buffered public part가 남았는지는 상태 전이 소유자인 part-helper가 socket atomic cache에 게시하고 poll hot path는 그 cache가 false면 state/mutex를 조회하지 않는 형태로 수정한다.

## 2026-09-03 15:30 KST — 원인 1 수정: buffered receive readiness cache

- `socket_request_reply_bridge_t`에 내부 atomic readiness cache를 추가했다. `buffer_recv_parts()` 성공, `take_recv_part()` 소비, `reset_recv_sequence()` 정리처럼 참조 상태를 실제로 바꾸는 part-helper 전이점만 cache를 갱신한다.
- `socket_base_t::has_in()`은 cache가 false인 정상 경로에서 shared_ptr 복사와 helper mutex를 건너뛴다. cache가 true일 때는 기존 참조 상태를 mutex 아래 다시 확인하므로 cache가 계약 판단을 대신하지 않는다.
- 수정 전 part-count 대조에서 candidate `get_events_internal`은 1-part 57 Ir/call, 2-part 74 Ir/call이고 baseline은 양쪽 48 Ir/call이었다. helper state가 생긴 2-part에서 `part_helper_state()`가 63.23 calls/message 추가되어 이 cache의 대상을 분리했다.
- 별도 코드 대조에서 `f3be895b3f`가 추가한 count-1 D/R head reclassification 상태 확인이 모든 PAIR flush/activate에도 실행되고, sub-LWM lost-wake fence가 drained batch마다 실행되는 것을 다음 공통 동기화 원인으로 확인했다. D/R 계약을 보존하면서 적용 범위를 줄일 수 있는 소유 상태를 조사 중이다.

## 2026-09-03 16:04 KST — 공통 pipe와 수신 bookkeeping 비용 축소

- `pipe.cpp`의 count-1 D/R marker 처리를 해당 topology와 실제 completed record가 있는 경우로 한정하고, 일반 `activate_read`는 먼저 비활성 입력을 복구하도록 분기했다. HWM lost-wake 계약의 seq-cst fence는 유지하되 drain 뒤 waiter를 중복 load하던 형태를 fence 뒤 단일 load로 정리했다.
- 런타임 memory-order 인자를 사용한 inbound frame counter store가 GCC에서 locked exchange로 내려가는 것을 확인해 release/relaxed 두 정적 분기로 바꿨다. 생성된 코드는 plain store이며 기존 release publication 계약은 보존한다.
- PAIR 등 source RID를 게시하지 않는 소켓은 receive part마다 256-byte RID를 다시 zeroing하던 경로를 valid fast-return으로 줄였다. ROUTER/XPUB의 실패 진입 시 기존 invalidation 호출은 제거하지 않았다.

## 2026-09-03 16:04 KST — 원인 2 실증과 수정: PAIR HWM command owner 수명

- 수정 전 candidate PAIR/inproc 64 KiB 처리 중 IO thread voluntary context switch가 10,531에서 166,694로 증가했지만 baseline은 8에서 44였다. 첫 HWM 대기가 `retain_async_command_processing()`을 통해 비동기 owner를 소켓 수명 동안 남겨, 같은 프로세스의 다음 in-flight-1 latency 구간에도 IO thread가 매 메시지 command를 처리한 것이 원인이다.
- PAIR의 owner 없는 blocking admission은 send scope를 해제한 public caller가 mailbox를 직접 `process_commands()`하도록 복구했다. 이미 monitor owner가 있을 때만 그 executor를 해당 wait 동안 빌리고, request/reply의 장기 executor 정책은 그대로 유지했다. 직접 mailbox에서 대기하는 caller는 submit-progress CV waiter가 아니므로 waiter 등록 전 fast path로 분리했다.
- 변경 뒤 같은 IO thread context switch는 baseline과 같은 8→44로 내려갔다. PAIR/inproc 64 KiB 2-part 1초 교대 3회에서 candidate throughput median 353,635 msg/s, mean 3.207 us, p95 3.854 us, p99 6.538 us였고 baseline은 340,715 msg/s, 3.416 us, 3.914 us, 6.869 us였다.

## 2026-09-03 16:04 KST — monitor owner 종료 경계 보강

- monitor lease 해제의 `wait_for_quiescence` 인자가 버려지고 있던 pass-through를 제거했다. 마지막 owner 해제 시 progress gate 아래 lease를 재검증한 뒤 coordinator stop을 게시하고, 외부 caller는 async quiesce 완료를 기다린다.
- 단순 signal은 실행 중 handler의 reschedule 경계를 잃을 수 있어 기존 no-op command를 mailbox에 실제 enqueue해 stop handoff를 닫았다. 실제 stop을 게시할 때 stale stop-request도 함께 지운다. 이 변경은 monitor close의 동기 종료 계약을 복구하며 임시 계측은 남기지 않았다.

## 2026-09-03 16:38 KST — PAIR 직접 owner handoff와 POSDDD 정리

- PAIR HWM에서 여러 public sender가 동시에 막혀도 mailbox command owner는 하나만 선출하고, 나머지는 submit-progress epoch/condition variable에서 기다리도록 책임을 `socket_submit_progress_runtime_t`에 모았다. 직접 owner 자신이 만든 progress 신호는 TLS로 구분해 자기 mailbox를 다시 깨우지 않는다.
- owner 선출 직전 mailbox wait epoch를 snapshot하고 command/signal edge마다 epoch를 영속적으로 증가시켰다. 기존 command waiter가 같은 broadcast나 pending hint를 먼저 소비해도 새 owner는 epoch 차이를 보고 즉시 drain하므로 drain-before-wait 및 waiter-consume lost wake를 모두 닫는다. finite timeout은 최초 deadline을 재사용한다.
- mailbox epoch의 기존 waiter 선점 상황을 `test_command_wait_preserves_signal_only_edges`에 추가했다. 최신 변경 뒤 multipart/helper/completion/context shutdown/request-reply/mailbox 계약 대상 6개 test가 모두 통과했다.
- POSDDD 정리로 request/reply의 호출자 없는 dealer reply publication/revoke chain, routed target의 미사용 selected-pipe 보관, part-helper의 foreign-handle 전역 registry 및 미사용 family/metadata/handle 인자, pipe·load-balancer의 호출자 없는 우회 함수를 제거했다. multipart retry는 frame 전체 vector 복제 대신 한 frame씩 stack copy/rollback하고, publish topic은 소유 storage와 `string_view`로 분리했으며 반복 send에서 buffer capacity를 유지한다.

## 2026-09-03 16:56 KST — WS 조기 writer wake 원인과 callback 수명 경계

- PAIR/ws 64 KiB 3회 진단에서 candidate 처리량 median은 35,282 msg/s, baseline은 37,135 msg/s로 0.9501이었다. candidate IO voluntary context switch median은 23,803회, baseline은 19,883회로 19.7% 많았다.
- callgrind의 전체 active send로 정규화하면 `wait_for_command_signal`은 candidate 58/567, baseline 21/389(+89%), `process_activate_write`는 68/567 대 33/389(+41%), PAIR까지 도달한 `write_activated`는 29/567 대 7/389(2.84배)였다. candidate의 29건은 `arm_send_recovery_after_backpressure -> mailbox.signal`과 일치한다.
- 원인은 `prefetched_batch_exhausted_`를 실제 published queue drain으로 간주해, 다음 batch가 준비된 경계에서도 sub-LWM blocked writer를 깨운 것이었다. writer가 실제 대기 중이고 planned LWM 미만인 rare fallback에서만 `_in_pipe->check_read()`로 다음 batch 부재를 확인하도록 복구했다. 정상 per-message 경로에는 preview를 추가하지 않았다.
- multipart 즉시 전송 성공/terminal 경로에서 public frame을 helper lock 아래 close하도록 바뀐 구조는 zero-copy user free callback의 재진입·deadlock·관측 시점 회귀 위험이 있어 채택하지 않았다. record 소유권만 lock 아래 분리하고 sequence/scope를 reset·unlock·notify한 뒤 callback을 실행하는 기존 수명 경계를 유지했다.

## 2026-09-03 17:24 KST — sub-LWM drain 계약·mailbox slow-path gate 보강

- `prefetched_batch_exhausted_`는 실제 queue drain이 아니라 현재 prefetch batch의 tail임을 ypipe 구현과 baseline 코드로 확인했다. 실제 drain 확인은 기존 `probe_if_published()`를 사용해 다음 published batch만 가져오고, 빈 queue에서도 reader sleep marker를 쓰지 않도록 했다. 1차 waiter load가 race를 놓친 batch-tail 경우에만 SC fence와 2차 load를 수행한다.
- 첫 batch를 prefetch한 뒤 두 번째 batch로 HWM을 채우는 직접 회귀 test를 추가했다. 첫 tail 소비 시 `write_activated=0`, 누적 credit이 정상 LWM에 닿았을 때만 `write_activated=1`임을 검증하며 targeted test 5개가 모두 통과했다.
- 모든 mailbox command에 무조건 붙었던 wait epoch 갱신을 제거했다. PAIR의 blocked public owner가 drain→wait 경계를 관찰하는 기간에만 observation lease를 두며, 일반 command는 observer/waiter가 없으면 epoch/CV를 건드리지 않는다. active receiver에 합류한 command와 signal-only edge를 다른 waiter가 먼저 소비한 경우를 test로 고정했다.
- 임시 public command owner가 primary descriptor를 소비한 뒤 `rearm_primary_signaler()`를 호출하고 나서 owner를 retire하도록 §3.7 handoff를 닫았다. buffered receive readiness의 같은 값 반복 release-store도 실제 false↔true edge에서만 게시하도록 줄였다.
- 최신 mailbox/PAIR/ctx 수명 대상 4개 test와 pipe/completion/request-reply 대상 5개 test가 모두 통과했다.

## 2026-09-03 17:27 KST — WS 64 KiB 수정 후 단독 진단

- build/test와 겹친 첫 진단은 폐기한 뒤, 관련 프로세스가 없는 상태에서 candidate/baseline을 번갈아 3회 실행했다.
- candidate throughput은 35,943/35,841/34,034 msg/s, baseline은 36,504/34,025/36,388 msg/s였다. 각 측의 median ratio는 35,841/36,388 = 0.985로, 수정 전 독립 진단 0.9501보다 회복됐고 size 허용치 0.95를 넘었다.
- 같은 clean run의 candidate mean/p95/p99 median은 0.060212/0.078828/0.137271 ms, baseline은 0.064005/0.098665/0.187768 ms로 각각 0.941/0.799/0.731이었다. 최종 판정은 전체 gate 뒤 정식 7-cell sweep의 size median과 4-size 기하평균으로 다시 수행한다.

## 2026-09-03 17:32 KST — submit owner의 유한 timeout 예산 보존

- async mailbox owner가 quiesce 중일 때 transport-pair owner 획득이 항상 내부 10초를 기다려, 더 짧은 `SNDTIMEO` 예산을 넘을 수 있던 경계를 확인했다. 기존 내부 호출은 10초/`EBUSY` 의미를 유지하고, submit 경로만 최초 deadline의 남은 예산과 `EAGAIN`을 전달하도록 분리했다.
- `wait_async_quiesced()`도 spurious wake마다 원래 timeout 전체를 다시 쓰지 않고 절대 deadline의 남은 시간을 사용한다. PAIR direct-owner, 기존 async owner 대여, non-PAIR synchronous fallback 모두 같은 submit deadline을 재사용한다.
- 명시적 async stop을 test hook으로 붙잡은 상태에서 30 ms submit 획득이 `EAGAIN`으로 15~500 ms 안에 끝나고, 이어지는 기존 10초 내부 acquire는 stop 해제 뒤 정상 성공하는 회귀 검증을 추가했다. 변경 뒤 context/public multipart/request-reply/runtime 표적 4개 test가 모두 통과했다.

## 2026-09-03 17:57 KST — 최종 기능 gate

- `ulimit -v 16777216` 아래 Core build 254/254, 전체 ctest 134/134, single-lane 29/29 두 번을 모두 통과했다. 알려진 snapshot-accounting flake는 발생하지 않았다.
- raw header mirror 12/12와 `git diff --check`를 통과했다. C++ binding contract 15/15 및 sample smoke 7/7도 통과했다.
- Python gate의 첫 실행은 시스템 Python에 pytest가 없어 시작 전에 종료됐고, 기존 test venv로 재실행하자 stale native extension이 제거된 Core symbol을 참조했다. local Core header/library를 사용해 ignored in-place extension만 재빌드한 뒤 같은 gate가 144 tests + 4 subtests, samples 7/7로 통과했다. tracked binding source는 변경하지 않았다.
- 정식 스펙 §5.1이 지목하는 `core/tests/perf/hotpath_gate`와 `hotpath_reference.json`은 현재 branch와 전체 repository에 존재하지 않아 ctest 134개에도 등록되지 않았다. 사용자 지정 gate는 모두 green이며, 이 미구현 gate는 금지된 reference 생성을 수행하지 않고 최종 spec-gap/BLOCKERS에 명시한다.

## 2026-09-03 18:16 KST — gate 후 7-cell 정식 sweep 1차

- 측정 전 지정 process 목록이 비어 있음을 확인하고 `SWEEP2_RUNS=3` 정식 명령을 실행했다. PAIR/tls와 PAIR/wss는 모든 size cell 및 5개 집계가 PASS했다.
- PAIR/tcp는 throughput 집계 1.0794, mean 0.9671로 개선됐지만 1024B throughput 0.9408과 p95/p99 집계 1.0206/1.0571이 FAIL했다. PAIR/ws는 throughput 집계 1.0297 및 모든 latency 집계가 PASS했지만 1024B throughput 0.9410이 FAIL했다. PAIR/ipc도 1024B throughput 0.9496과 p99 집계 1.0220이 FAIL했다. wss의 같은 size는 0.9555로 경계 PASS여서 공통 1024B 비용을 추가 분리한다.
- PAIR/inproc는 원래 핵심인 65536B mean/p95/p99가 0.9249/1.0066/0.9300으로 회복했지만, 64B p95 2.9589와 256/1024B p99 2.3697/2.0609 때문에 latency 집계 1.0215/1.3305/1.6092가 FAIL했다.
- PUBSUB/tcp는 throughput/mean/p95/p99 집계가 1.0937/0.9764/0.9695/0.9878로 모두 PASS했지만 65536B p99 단일 cell 1.1807이 FAIL했다. 단순 반복하지 않고 PAIR 1024B, inproc 소형 tail, PUBSUB 64KiB tail을 독립 조사한 뒤 공통 원인부터 수정한다.

## 2026-09-03 19:02 KST — in-flight-1 latency 벤치의 ack 경계 정정

- `perf_single_one_way.hpp`는 수신 payload를 확인하자마자 `active_received`를 증가시킨 뒤 같은 receiver loop에서 DONTWAIT drain을 계속했다. 따라서 sender의 다음 1개 전송이 receiver의 warm drain에 합류할 수 있어, 측정 대상인 `send → public poller wake → recv`와 warm-drain 경로가 불규칙하게 섞였다.
- PAIR/inproc 64B p2 latency 구간에서 기존 sender voluntary context switch는 baseline +2,138, candidate +20,546였고 candidate p95는 6.873 us였다. CPU 0/2 고정 반복에서도 baseline p95 2.619/2.670 us, candidate 7.541/7.571 us로 재현돼 CPU migration이나 외부 preemption은 제외했다.
- latency phase의 ack를 해당 DONTWAIT burst가 EAGAIN 또는 STOP에 도달한 뒤 게시하도록 바꾸고 candidate와 baseline worktree에 동일한 파일을 적용했다. 이 변경은 throughput count 경로를 건드리지 않고 latency sample만 다음 public poller cycle 뒤에 진행시킨다.
- 교대 2회 검증에서 baseline mean/p95/p99는 2.223/2.632/3.546 us, 2.172/2.569/3.551 us였고 candidate는 1.839/2.468/3.217 us, 1.884/2.518/2.964 us였다. candidate 비율은 mean 0.827/0.867, p95 0.938/0.980, p99 0.907/0.835로 모두 통과했다. sender voluntary context switch도 baseline +163/+163, candidate +240/+246으로 안정됐다.
- reader의 sub-LWM SC fence는 p1에도 메시지마다 존재하지만 p1에는 같은 tail 회귀가 없었고, callgrind의 candidate 결정 경로도 baseline보다 느리지 않았다. fence를 비동기 credit-probe만으로 대체하면 reader가 API를 떠난 뒤 마지막 probe를 처리할 owner가 없어지는 lost-wake 반례가 있으므로 변경하지 않는다.

## 2026-09-03 19:49 KST — PAIR 완성 record 성공 경로와 1024B 원인 분리

- 기존 2-part public helper가 FINAL에서 이미 완성한 정상 PAIR record에 한해, pipe lock을 한 번 잡아 전체 credit을 preflight하고 두 frame을 게시·flush하는 성공 전용 경로를 추가했다. registry accounting, conflate, observer, request, fault hook, incomplete owner state, oversize/HWM 후퇴는 기존 frame 경로가 계속 소유한다. `test_gap_h3_pair_contract`, `test_public_inproc_multipart_send`, `test_phase3_completion_contract`, `unittest_zmp_contract_edges`가 통과했다.
- 교대 3회 2초 진단에서 tcp/ipc 1024B throughput median ratio는 각각 0.983/0.973으로 허용선을 회복했지만, ws는 0.942로 그대로였다. WS callgrind에서 candidate는 199,895,668 Ir/4,624 records, baseline은 211,326,139 Ir/4,719 records로 record당 명령어가 3.5% 적었고 async write/frame도 1.2%만 많았다. 따라서 남은 WS 회귀를 public send 명령어 증가나 Beast/encoder 구현 변경으로 설명할 수 없었다.
- 정적 계약 감사에서 whole-record 경로의 ownership, HWM, accounting, close/terminate 선형화 blocker는 없었다. 다만 새 `xtry_send_complete_record`와 `try_write_complete_record_and_flush`가 정식 hot-path spec §2 호출 트리에 아직 없으며, 보호 문서 수정 승인이 없으므로 최종 spec-gap에 남긴다.

## 2026-09-03 19:49 KST — WS encoder batch와 D/R 재분류 책임 수정

- ZMP encoder와 ASIO/WS steady-state source는 baseline과 동일했다. 8KiB encoder output batch에서는 1024B record 약 6개마다 WS async write가 발생했다. 송수신 batch를 함께 16KiB로 한 진단은 WS throughput ratio를 약 1.13으로 올렸고, 입력 8KiB·출력 16KiB만 둔 최소 진단도 3회 median 721,843/676,511 = 1.067을 기록했다. 입력 default는 보존하고 `default_in_batch_size`와 `default_out_batch_size`의 책임을 분리했다.
- 모든 count-1 PAIR flush가 실제 호출자 없는 D/R head-reclassification 조건을 검사하며 peer reader cacheline을 acquire-load하던 §3.4 위반을 확인했다. callgrind의 PAIR `flush_unlocked`는 candidate 100.2 Ir/record, baseline 68.1 Ir/record였다. peer socket type이 DEALER/ROUTER인 경우만 재분류를 허용하는 `peer_uses_routed_protocol_unlocked()`로 정책을 모으고, duplicate activate-read도 local armed flag를 먼저 gate하도록 바꿨다.
- PAIR whole-record의 command 관찰은 full mailbox probe 대신 이미 direct submit commit point로 쓰이는 `process_submit_commands()`를 사용한다. command hint나 active drain이 있으면 authoritative drain으로 후퇴하고, 정상 경로는 ETERM atomic만 확인한다. 표적 6개 test가 모두 통과했다.
- 최신 교대 진단에서 1024B throughput candidate/baseline은 tcp 약 1.04, ipc 약 1.10, ws 약 1.07, wss 약 1.20이었다. 최종 판정은 전체 gate 뒤 지정 7-cell `runs=3` sweep으로 수행한다.

## 2026-09-03 20:04 KST — 최종 기능 gate 재검증

- `ulimit -v 16777216` 아래 최종 Core build 153/153과 전체 ctest 134/134가 통과했다. `test_single_lane_` 29/29도 연속 두 번 통과했고 snapshot-accounting flake는 발생하지 않았다.
- raw header mirror 12/12, `git diff --check`, C++ contract 15/15와 sample smoke 7/7가 통과했다.
- 시스템 Python에는 pytest가 없다는 앞선 환경 실패를 반복하지 않고 저장소의 기존 `.artifacts/wsl/python-build-venv`를 `PYTHON_EXECUTABLE`로 지정했다. `ZLINK_CORE_SOURCE=local` Python gate는 144 tests + 4 subtests와 sample 7/7로 통과했다.
- 기능 gate가 모두 green이므로 다음 단계는 측정 process 사전 확인 후 정확한 7-cell `SWEEP2_RUNS=3` 재측정이다. 측정 중 build/test는 실행하지 않는다.

## 2026-09-03 20:36 KST — 정식 sweep 1·2차와 tail drift 분리

- 최종 gate 뒤 첫 정식 sweep에서 PAIR/tcp와 PAIR/inproc는 전 cell·집계 PASS였다. PAIR/ws는 64 KiB p99가 허용선 1.0500을 0.00005 넘은 1.0501, PAIR/ipc는 64 KiB p95/p99 1.1973/1.1475, PUBSUB/tcp는 p99 집계 1.0336으로 실패했다. throughput 집계는 각각 1.1634/1.2017/1.1142였다.
- 동일 binary 재측정에서 WS 실패는 64 KiB p99에서 1024B p99로 이동했고, IPC 64 KiB candidate 절대 p99는 147.8→119.6 us로 개선됐지만 baseline이 128.8→100.9 us로 더 크게 이동해 비율은 악화했다. PUBSUB 실패도 64/256B p99에서 64B p95/p99로 이동했다.
- baseline/candidate의 PAIR·PUBSUB 벤치 파일과 공통 one-way/latency 파일이 byte-for-byte 동일함을 확인했다. raw run 분포와 이전 paired report를 대조한 결과 단일 코드 분기보다 WSL2 scheduler/CPU drift가 tail 비율을 지배하지만, 두 번째 집계 실패를 그대로 넘기지 않고 불필요한 공통 정책을 소유 모듈로 좁혔다.

## 2026-09-03 20:47 KST — WS batch 책임과 FQ publication 범위 정리

- 전역 input/output batch를 baseline과 같은 8 KiB로 복원하고, 한 encoder batch가 Beast binary write 하나가 되는 WS/WSS의 비-STREAM ZMP engine에만 setup 시 16 KiB를 복사 적용했다. hot path transport-name 분기나 새 공개 option은 추가하지 않았다.
- count-1 DEALER/ROUTER head reclassification만 소비하는 `pipe_t::_public_receive_active`를 generic FQ가 XSUB/STREAM에서도 매 deactivate/activate마다 게시하던 비용을 제거했다. `fq_t`가 publication policy를 받도록 하고 DEALER/ROUTER owner만 opt-in한다.
- Core build 171/171, WS/WSS·PAIR·PUBSUB·routed 표적 9/9, single-lane 29/29가 통과했다.

## 2026-09-03 20:55 KST — 정책 분리 후 역순 진단

- 2초·runs=3 C→B 진단에서 PAIR/ws는 throughput 집계 1.1755, mean/p95/p99 0.8756/0.9667/0.9145로 전 cell·집계 PASS했다. WS 16 KiB 범위 한정이 1024B throughput 회복을 보존했다.
- PAIR/ipc throughput 집계는 1.2813, mean은 0.9493이었다. 256/1024B의 run outlier로 p95/p99 집계는 1.0580/1.0137이었지만, 별도 1024B 교대 두 쌍에서는 candidate p95/p99 30.0/62.3 us 대 baseline 42.5/100.5 us, 28.5/62.7 us 대 32.1/68.2 us로 두 번 모두 개선됐다.
- PAIR/ipc 1024B callgrind는 candidate 218,420,700 Ir/9,049 records = 약 24.1 KIr/record, baseline 212,859,009 Ir/8,496 records = 약 25.1 KIr/record로 candidate가 3.7% 적었다. tail 비율 악화가 per-record 명령어 회귀라는 가설을 제외했다.
- PUBSUB/tcp C→B 진단은 throughput/p95/p99 집계 1.0652/0.9919/0.9485였고 mean만 1.0012 경계였다. FQ publication 한정 뒤 tail 집계는 개선 방향이며, 최종 판정은 전체 gate 뒤 5초 정식 B→C sweep으로 다시 수행한다.
