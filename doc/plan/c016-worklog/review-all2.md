# review-ALL-2 — 0.17.3 트랙 2 누적 patch 차단 검증

**판정: 채택 보류. 신규 차단 결함 1건(B-ALL2-1).** PAIR multipart 일괄 송신의 사전 검증과 실제 쓰기 사이에 I/O thread가 peer 최대 메시지 크기를 바꾸면 `zlink_assert(written)`으로 종료할 수 있다. 트랙 1의 receive 소유권 전환 반례는 단일 lifecycle turn으로 해소됐지만, 이것만으로 pipe의 foreign writer까지 같은 불변식 안에 들어오지는 않는다.

## 범위와 증거

- 대상: `/home/hep7hep7/project/zlink-work/all`, branch `wip/0.17.3-all2`, HEAD `1a79625d3db197431dd08876bca9af483b403ed2` (`wip(core): ALL-2 cumulative patch (0.17.3 track 2) — not gated`).
- 비교: `git show --stat HEAD`와 `git diff HEAD~1 HEAD`. 63개 파일, +5,211/−4,748. `origin/main` 대비 비교로 트랙 1 변경을 섞지 않았다.
- 입력: ALL-1/ALL-2 보고서, review-st1/st2, ST-3 보고서, WS-1 분석, synchronization11, ZMP §8/§9, Socket 공통 §2/§6/`disconnect_rid`. ALL-2 보고서는 작업 중 파일이 사라진 상태여서 메인 저장소의 `git show HEAD:doc/plan/c016-worklog/core-rf-ALL-2-report.md`로 읽었다. 파일을 복원하지 않았다.
- **정적 검토만 수행했다.** 아래 재현 순서는 코드에서 도출한 실행 순서이며 실행한 repro가 아니다. 소스·스펙·테스트 수정, 빌드·테스트·벤치 실행, commit은 하지 않았다. 기존 보고서의 테스트 결과와 이번 검토 결과를 구분한다.
- 아래 코드 경로는 별도 표시가 없으면 대상 worktree 기준이다. `common/`은 `core/src/runtime/sockets/common/`, `core/`는 `core/src/runtime/core/`의 약칭이다. 보고서 경로는 메인 저장소 `doc/plan/c016-worklog/` 기준이다.
- 판정의 **해소**는 해당 반례를 정적으로 닫았다는 뜻이다. 모든 스케줄의 실행 검증을 뜻하지 않는다. B는 채택 차단, W는 잔여 위험·검증 한계, S는 사양 표현·설명 정리다.

## 1. 항목별 판정

| 요청 항목 | 판정 | 근거와 결론 |
|---|---|---|
| 1-A receive·readiness·command·completion·attach의 공통 turn | 해소 | `common/socket_lifecycle_runtime.cpp:58`, `common/socket_runtime.hpp:352`, `common/socket_base_lifecycle.cpp:421`, `common/socket_base_api.cpp:478`, `:1047`, `:1624`. 실제 scheduler/route/receive 변경은 같은 lifecycle bit로 직렬화한다. 아래 전이 표 참조. |
| 1-B mutex fallback 지연 진입 | 해소 | fallback 상태와 mutex 선택 자체가 제거됐다. 지연된 진입자도 `socket_lifecycle_runtime.cpp:415`의 같은 CAS를 통과해야 한다. |
| 1-C async→available | 해소 | async는 executor 예약 상태이고 receive 배타 방식이 아니다. turn 이후 async 재확인 `socket_base_lifecycle.cpp:431`, 표식 guard `:425`가 unlock보다 먼저 종료된다. |
| 1-D progress epoch·CV 재진입 | 해소 | `socket_runtime.hpp:320`, `socket_base_lifecycle.cpp:1580`. epoch·waiter SC 연산과 plain `progress_sync`로 등록/알림을 연결한다. |
| 1-E blocking recv·종료·기아 | 부분 | 일반 recv의 조건 실패 후 turn 해제는 확인. command batch는 EAGAIN까지 무제한이고 turn 대기 자체에는 종료 취소가 없다. endpoint 종료의 기존 foreign turn/wait도 남는다. W-ALL2-1/2. |
| 2-A socket `_out_sync` 제거와 foreign scalar | 미해소 | C2 writer 직렬화와 atomic 발행은 대부분 맞지만, max의 per-record 불변식이 깨진다. `core/pipe_write.cpp:832`, `:866`, `:874`, `:1102`, `:1139`. **B-ALL2-1.** |
| 2-B reply generation·cold transport gate | 해소 | `core/pipe_write.cpp:791`, `core/pipe_transport.cpp:729`, `core/session_base_pipe_io.cpp:11`, `core/src/api/socket/socket_request_reply_runtime_io.cpp:1434`. 기존 stamp와 record 단위 stale 폐기로 retirement 뒤 late publication을 처리한다. |
| 3 route shard snapshot·pipe 수명 | 해소 | `core/src/runtime/sockets/stream/stream.cpp:145`, `:169`, `:196`, `:243`, `stream.hpp:89`. immutable map과 shared entry가 pipe ref를 유지하며 raw lookup 결과 사용은 turn 안이다. |
| 4-A Beast 8-byte mask·1,120 조합 | 해소 | `core/external/boost/boost/beast/websocket/detail/mask.ipp:47`, `core/tests/unittest/unittest_ws_transport_config.cpp:66`. alignment·endian·tail·연속 chunk 회전의 정적 근거 확인. |
| 4-B 128 KiB batch·connection 메모리 | 부분 | §9의 현재 bytes에 대한 bounded batch는 유지한다. 고정 메모리 증가는 client +176 KiB, server +112 KiB. high-CCU RSS 수용성 미확인. W-ALL2-3. |
| 4-C executor·connection 수명 | 해소 | `core/src/runtime/transports/ws/ws_transport_common_internal.hpp:26`, `:112`, `ws_transport.cpp:41`, `core/src/runtime/transports/tls/wss_transport.cpp:44`. owner io_context와 callback의 connection `shared_ptr`를 유지한다. |
| 4-D `ws_roundtrip_gate.py` 판정 | 부분 | 필수 셀·완료·중복·양수·유한값 검사와 Q 공식은 맞는다. 복수 파일에서 비교 셀의 같은 측정 조건을 보증하지 못한다. W-ALL2-4. |
| 5-A `_slot_sync` plain mutex | 해소 | `core/ctx.cpp:232`, `:275`, `core/ctx.hpp:175`. socket 생성의 registry 구간 뒤에 후속 schedule을 실행하고 파생 constructor의 재진입 호출을 제거했다. |
| 5-B mailbox post `noexcept` | 해소 | `core/mailbox.cpp:65`, `:348`, `:354`, `:366`. 기존 queue allocation fatal 정책을 post에도 적용한다. 예외 후 정상 복귀/재시도하는 계약을 새로 만들지 않는다. |
| 5-C monitor stop 이동 | 해소 | `core/ctx_termination.cpp:82`, `common/socket_base.cpp:345`, `common/socket_base_lifecycle.cpp:1377`, `common/socket_base_monitor.cpp:839`. registry 밖·stop command turn 밖에서 monitor를 정지한다. |
| 5-D lb observer·async consumer | 해소 | `core/src/runtime/sockets/dealer/dealer.cpp:532`, `router/router_admission.cpp:483`, `common/socket_base_lifecycle.cpp:1492`. 관측·drain/detach/reschedule이 기존 turn을 사용한다. |
| 5-E pipe 분할의 무동작변경 | 부분 | 분할 시점의 196/196 body 비교는 ALL-1 보고서 `:30`, `:62`의 증거다. 최종 patch에는 실제 의미 변경도 있으므로 최종 63파일 전체의 무동작변경 증거로 확대할 수 없다. W-ALL2-5. |
| 6 D-ALL-1 테스트 | 해소 | `core/tests/unittest/unittest_phase3_request_reply_owners.cpp:1017`, `:1031`; `router/router.cpp:474`. route map 부재를 관측한 다음 NOT_FOUND/ENOENT를 검사한다. |
| 7 WIN-1 공존 | 해소 | `core/ctx_termination.cpp:122`의 EAGAIN 재대기 유지. `random.cpp`는 ALL-2 diff에 없고 부모의 RtlGenRandom 경로 유지. Windows 실행 검증은 수행하지 않았다. |
| 8 hot path·성능 근거 | 부분 | CAS 1+RMW 1은 turn 자체 비용이다. recv에는 별도 `recv_ticks` RMW가 있다. 8.3861은 pthread lock 측정값이며 dev/Release 비교 조건이 다르다. W-ALL2-6. |
| 9 사양 대조 | 부분 | 주된 단일 turn 설계는 §3.1/§3.3에 맞는다. §2/§5의 per-record 불변식 위반은 B-ALL2-1, §3.4/§6 기존 위반은 W-ALL2-2. 문장 초안은 S-ALL2-1. |
| 10 RAII·예외·dead code·이식성 | 부분 | 새 receive scope의 RAII와 portable include는 확인. 이전 설계 주석이 남으며 Windows build 및 일부 종료 순서의 실행 검증은 없다. S-ALL2-2, W-ALL2-1/2/5. |

## 2. 코드에서 재구성한 lifecycle turn

turn은 `public_api_state`의 bit 62이고 close·admission·multipart/completion bit와 같은 word에 있다(`common/socket_lifecycle_runtime.cpp:22`). receive 전용 available/public/command/async word는 더 이상 없다. 획득 CAS와 해제 `fetch_and`는 다른 bit를 보존한다(`:415`, `:437`). 현재 thread의 내부 재진입은 TLS 소유자 확인으로 같은 turn을 빌리며, 빌린 scope는 이를 해제하지 않는다(`:388`, `socket_runtime.hpp:312`, `:352`).

| 출발 → 도착 | 실행 주체와 적용 범위 | 코드 근거 |
|---|---|---|
| turn 없음 → 소유 | public recv의 실제 `xrecv` 1회, readiness의 `xhas_in` | `socket_base_msg.cpp:53`, `:671`; `socket_base_api.cpp:1047`; acquire `socket_lifecycle_runtime.cpp:58` |
| 소유 → 같은 소유 | command/control 안에서 receive/attach/completion 호출 | TLS 소유자면 `acquire_turn()`이 false 반환. entry destructor가 caller turn을 해제하지 않음. `socket_lifecycle_runtime.cpp:58`, `socket_runtime.hpp:355` |
| entry 소유 → record scope 소유 | 첫 part 성공 후 record header/body 및 continuation 처리 | `socket_base_msg.cpp:62`, `:73`; `socket_runtime.hpp:375`, `:432`, `:445` |
| 소유 → 없음 | 성공한 단일 수신 반환, 실패한 시도, RAII unwind | entry/record destructor 및 `socket_lifecycle_runtime.cpp:437` |
| 없음 → command 소유 → 없음 | mailbox batch 전체, deferred termination, readiness 알림 | `socket_base_lifecycle.cpp:421`, `:506`, `:523`, `:531`; 표식 guard가 turn guard보다 늦게 생성되어 먼저 파괴됨 `:425` |
| async inactive/pending/active/quiesce 변경 | executor 소유권만 변경. 배타 장치는 계속 같은 turn | `socket_base_lifecycle.cpp:1023`, `:1492`, `:1522`, `:1554`, `:1568` |
| 수신 실패 → waiter 등록 → 없음 → CV 대기 | turn 안에서 waiter 증가 후 scope 종료. plain mutex를 잡고 epoch 재검사 | `socket_base_lifecycle.cpp:1587`, `:1590`, `:1597` |
| 대기 종료 → 소유 → 재검사 | recv loop가 다시 receive entry 획득. timeout도 마지막 검사 후 결정 | `socket_base_msg.cpp:820`, `:830`, `:843` |
| admission 있음 → close 거절 | close는 turn 양도를 기다리는 연산이 아님 | Socket README `:57`; lifecycle state의 admission/closing bit 보존 |

실제 mutation 범위를 함께 확인했다. endpoint bind/connect의 바깥 turn 아래 직접 attach가 실행되고, `xattach_pipe`에도 같은 entry가 있다(`socket_base_endpoint.cpp:183`, `:223`, `:697`; `socket_base_api.cpp:478`). completion drain도 같은 entry로 count-1 DATA와 충돌하지 않는다(`socket_base_api.cpp:1624`). `has_in`의 buffered public part 조기 반환은 helper가 이미 보유한 part를 조회하며 fq를 건드리지 않는다. STREAM writable 등록의 route 존재 검사도 readiness scope 뒤로 들어갔다(`socket_send_complete.cpp:194`).

설계 비교도 같은 소유권 기준으로 재검토했다. receive owner word·fallback·handoff를 보완하는 안보다 기존 lifecycle turn 재사용은 독립 배타 장치를 3→1로 줄인다. pipe 쪽은 mutex를 남기는 안보다 turn/C3 분리가 비용을 줄이지만, max처럼 commit과 결합된 값은 독립 C3라는 전제가 틀리다. route는 turn 아래 mutable map도 가능하지만 C1 immutable snapshot은 entry lifetime을 한 소유자에 묶는다는 근거가 있다. WS는 frame마다 write하는 안보다 현재 bytes의 bounded batch가 합성 연산 횟수를 줄이며, 그 대가인 고정 메모리 증가는 별도 검증해야 한다.

### 트랙 1 반례별 재현 순서 대조

| 기존 순서 | ALL-2에서의 같은 순서 | 판정 |
|---|---|---|
| C가 command mode를 소유 → P가 mutex fallback을 결정하고 정지 → C가 available 발행 → R이 lease 취득 → P가 뒤늦게 mutex 진입 | P와 R 모두 동일 bit를 CAS한다. R이 먼저 성공하면 P는 상태어만 읽고 기다린다. 별도 mutex 진입으로 fq에 접근할 갈래가 없다. | 해소 |
| P가 async mutex 수신 중 정지 → A가 async→available 저장 → R이 lock-free 수신 | async detach가 receive 권한을 발행하지 않는다. P가 turn을 쥐면 R은 CAS에 실패한다. A의 drain/detach도 같은 turn 경계를 사용한다. | 해소 |
| public command 후보가 async=false를 관측 → installer가 async를 설치 → 후보가 뒤늦게 command 시작 | 후보는 turn 획득 뒤 `async_mailbox_owns_commands()`를 다시 검사한다(`socket_base_lifecycle.cpp:431`). 필요한 control scope는 자기 turn 안에서 drain한다. | 해소 |
| receiver가 이전 epoch를 기억 → publisher가 waiter 0을 보고 생략 → receiver가 잠듦 | waiter 증가와 publisher의 epoch 증가/등록 관측, waiter의 epoch 재관측이 SC 순서다. publisher가 등록을 못 봤으면 이후 waiter가 바뀐 epoch를 본다. 등록을 봤으면 publisher가 progress mutex를 통해 CV 등록 뒤 알림을 보낸다. | 해소 |
| POLLIN thread의 `xhas_in`과 유일 DATA receiver의 `xrecv`가 동시에 fq 변경 | 양쪽이 `socket_receive_entry_scope_t`를 획득한다. buffered helper 반환은 이 반례의 fq 접근이 아니다. | 해소 |
| bind/connect direct attach가 async receive와 동시에 scheduler/route 변경 | direct attach caller의 turn과 `xattach_pipe`의 entry가 배타를 유지한다. 단순 attach 이름만 보고 monitor·pair mutex로 대신 보호한다고 판단하지 않았다. | 해소 |
| WS/WSS callback 대기 중 root close·context 정리 → callback이 이전 stream 사용 | callback의 정확한 connection generation `shared_ptr`가 stream을 보유한다. close는 root를 move하고 cancel/shutdown/close한다. 테스트의 io_context는 socket보다 오래 유지되고 소유 client thread가 정리한다. | 해소(이 수명 반례) |
| ctx registry를 쥔 채 monitor worker 정지 → worker가 registry/다른 socket 필요 | mailbox ref snapshot 후 registry를 놓고 모든 monitor를 먼저 멈춘 뒤 socket stop을 보낸다(`ctx_termination.cpp:82`). | 해소(이 역순 경계) |
| receive recursive mutex 재진입 깊이 2 → CV wait가 mutex를 완전히 놓지 못함 | receive progress CV의 mutex는 plain `mutex_t`; waiter 등록의 turn scope는 CV 전에 끝난다. 이중 receive mutex 깊이가 없다. | 해소 |

TLS/context 수명 반례가 해소됐다는 결론을 **모든 종료 대기가 취소 가능하다**는 뜻으로 확대하면 안 된다. 다음 두 위험은 남아 있다.

## 3. B-ALL2-1 — PAIR complete-record의 max 검증이 쓰기 전에 무효화됨

**신규 runtime 차단, 미해소. 소유 계층은 Core pipe admission이며 synchronization11 §2(특히 62–65행), §3.2, §5와 Socket README §2의 record atomic admission에 해당한다.**

`core/pipe_write.cpp:832`는 `_max_message_bytes`를 한 번 읽고, `:866`에서 전체 payload를 검사한다. 승인 후 각 part를 `write_message_unlocked(..., false, false, NULL)`로 쓰며 성공을 단정한다(`:871–875`). 그런데 그 함수는 `:1117`에서 max를 다시 읽고 `:1139–1147`에서 새 상한을 넘으면 false를 반환한다. HWM 검사 플래그가 false여도 이 max 검사는 생략되지 않는다.

동시에 `core/session_base.cpp:150–154`는 I/O handshake에서 socket 쪽 pipe의 max를 직접 바꾼다. 호출은 `core/src/runtime/engine/asio/asio_zmp_engine.cpp:591`, setter는 `core/pipe_write.cpp:1102–1107`의 release store다. socket turn을 획득하지 않는다. PAIR는 기본 `immediate=0`(`core/options.cpp:122`)에서 handshake 전 socket pipe가 attach될 수 있다(`common/socket_base_endpoint.cpp:650–708`, `core/session_base.cpp:363–374`). 일반 application queue는 `_registry_accounting=false`이므로 빠른 경로의 제외 조건에도 걸리지 않는다(`core/pipe.cpp:132–146`). public 경로는 `common/socket_send_complete.cpp:261–299` → `core/src/runtime/sockets/pair/pair.cpp:117–128`이다.

| 순서 | application thread: PAIR FINAL 제출 | I/O thread: peer READY |
|---|---|---|
| 1 | 크기 64+64의 두 part, 충분한 HWM, 현재 max=0으로 complete-record 사전 검증 통과 | handshake 진행 중 |
| 2 | 첫 part 쓰기까지 진행하거나, 사전 검증 직후 정지 | peer max=96을 파싱하여 같은 socket-end pipe에 release store |
| 3 | 이후 part 쓰기가 acquire load로 max=96을 읽음. 누적 payload=128이므로 false 반환 | socket turn이 필요 없으므로 위 store가 가능 |
| 4 | `pipe_write.cpp:874`의 `zlink_assert(written)` 실패 | — |

부모에서는 `HEAD~1:core/src/runtime/core/pipe.cpp`의 `try_write_complete_record_and_flush`(`:2579` 부근)와 `set_max_message_bytes`(`:3707` 부근)가 모두 같은 `_out_sync`를 잡았다. 따라서 whole-record 검증/쓰기 사이에 setter가 들어오는 순서는 이번 제거로 새로 가능해졌다. `core/src/runtime/utils/err.hpp:94–100`의 `zlink_assert`는 Release에서도 활성인 abort 매크로다. atomic release/acquire는 data race를 제거하지만 **이미 성공한 사전 검증의 유효성**을 보장하지 않는다. TSan warning 0으로 배제할 수 없는 논리 race다.

수정 완료 조건은 기존 record admission 소유자가 정책 관측과 record commit의 일관성을 보장하고, 동시 peer limit 발행이 정상 오류 또는 기존 계약에 맞는 일관된 admission 결과가 되도록 하는 것이다. assertion 완화, caller retry, budget 증가로 닫을 수 없다. 검증은 handshake의 max 발행을 사전 검사와 첫/마지막 part 사이에 고정하고 정상 반환·rollback·part ownership을 검사해야 한다. 기존 PAIR xsend gate hook을 그대로 켜면 일괄 경로를 우회한다(`pair.cpp:122–126`)는 점도 주의해야 한다.

## 4. C-2b / C-2d의 나머지 검토

| 상태·결정 | writer/reader와 보호 | 판정 |
|---|---|---|
| socket-end queue·multipart·active/generation 묶음 | socket turn의 실행자; session-end는 I/O thread. ordinary write/read/flush에 있던 중복 writer lock 제거 | owner 통합 자체는 맞음. foreign max와 record 사전검증의 결합만 별도 B-ALL2-1 |
| HWM·peer max | `pipe.hpp:790`, `:829` atomic; HWM `pipe_transport.cpp:549–585`, max `pipe_write.cpp:1102` release/acquire | scalar 발행은 맞음. scalar라고 per-record 불변식까지 독립인 것은 아님 |
| cold context 회계 관측 | `pipe_write.cpp:1407–1423` total/provisional release, `:1282–1319` acquire 및 available 범위 clamp | foreign reader가 socket writer의 plain incomplete 상태를 읽던 경계를 published snapshot으로 통합. 두 값의 동시 원자 snapshot이라고 해석하면 안 됨 |
| peer 수명 링크·transport generation | `pipe.cpp:370–404`, `pipe_transport.cpp:729–735`의 cold gate와 retained ref 유지 | hot writer lock 제거와 별개인 양쪽 공유 수명 보호 유지 |
| request correlation 예약/반환 | `pipe_receive.cpp:608–660`, `:668–704`는 여전히 같은 `_out_sync`. timeout scheduler도 반환 writer | lock 제거 대상이 아님. 실제 두 writer가 있으므로 §4 근거가 있는 남은 lock |
| reply generation 검사 | record admission에서 connection ID 확인, 모든 part에 같은 stamp, session pull에서 stale prefix부터 FINAL까지 폐기 | `socket_request_reply_runtime_io.cpp:1434–1476`, `pipe_write.cpp:791–823`, `session_base_pipe_io.cpp:11–32`. 종료와 write를 직렬화하던 helper 제거를 기존 stamp 폐기 규칙으로 설명 가능 |
| reply 검증 범위 | `unittest_zmp_engine_controls.cpp:242–312`는 stale single, stale prefix/fresh final, 이후 fresh record를 확인 | discard 규칙 검증은 있음. 실제 두 thread의 retirement/write interleaving을 결정적으로 실행한 테스트는 아님 |

route shard는 `shared_ptr<const map<...shared_ptr<pipe_t>...>>`이고, 최초 entry 생성에서 pipe lifetime ref를 한 번 취득한다(`stream.hpp:89–113`, `stream.cpp:169–193`). snapshot 복사는 shared entry를 복사하며 마지막 shared entry 해제 시 custom deleter가 pipe ref를 반환한다. 종료는 map에서 제거한 새 snapshot을 발행하고 retired snapshot을 정리한다(`:196–232`, `:243–276`). 따라서 map 제거만으로 아직 보유된 entry의 pipe가 즉시 소멸하지 않는다.

다만 lookup은 snapshot을 local shared_ptr로 복사하지 않고 raw `pipe_t *`를 반환한다(`stream.cpp:145–166`). 이것의 안전 근거는 **lookup과 사용·교체가 같은 socket turn 안**이라는 조건이다. `route_shard_for()`의 turn assertion과 send/routed/writable 호출자를 확인했고, writable wait 등록도 scope 안으로 옮겼다. 향후 C1 snapshot을 이유로 이 lookup만 turn 밖으로 꺼내면 현재 수명 증명이 깨진다. 현 patch에서 그런 production caller는 찾지 못했다.

## 5. WS/WSS

### mask와 carrier 계약

8-byte XOR는 native 32-bit key를 64-bit 양 절반에 반복하고 `memcpy`로 읽고 쓴다(`mask.ipp:47–71`). 비정렬 load/store나 strict-aliasing 위반이 없고, 같은 native endian으로 읽기/쓰기를 하므로 byte 순서도 원래 4-byte mask 반복과 같다. 8/4-byte chunk는 key 주기를 바꾸지 않으며 tail 0–3 byte만큼 회전하는 결과는 전체 길이 modulo 4와 일치한다.

`unittest_ws_transport_config.cpp:66–151`의 길이 0..31 및 65535..65537 × 시작 alignment 8 × key rotation 4는 정확히 1,120조합이다. scalar reference, 양끝 sentinel, 두 번 적용한 복원, 분할 chunk 사이 회전까지 검사한다. 이번 변경의 분기와 64 KiB 경계를 정적으로 검증하기에 충분한 구성이다. 실제 big-endian/Windows에서 실행했다는 증거는 아니다.

ZMP `core/doc/spec/core/protocol/01-zmp.ko.md` §8의 binary carrier byte-stream 규칙과 §9의 현재 확보한 bytes에 대한 bounded batch 규칙에 대한 동작 변경은 찾지 못했다. WS message 경계를 새 ZMP record 경계로 해석하거나 미래 입력이 찰 때까지 기다리는 로직은 추가되지 않았다. 128 KiB는 유한한 현재 batch의 상한이며, 그 크기 자체는 §9 위반이 아니다.

### W-ALL2-3 — connection당 고정 메모리 증가와 WS 잔여 목표

`ws_batch_policy.hpp:13–15`의 128 KiB가 non-STREAM encoder의 즉시 `malloc` 크기에 연결된다(`core/src/runtime/protocol/encoder.hpp:30–39`). 기존 16 KiB 대비 **endpoint당 +112 KiB**다. Beast client masking scratch도 기본 64→128 KiB로 증가한다(`ws_transport_common_internal.hpp:41–45`, `:98–103`; `core/external/boost/boost/beast/websocket/impl/stream_impl.hpp:233–243`).

| 단위 | 증가량 |
|---|---:|
| client endpoint | 176 KiB |
| server endpoint | 112 KiB |
| client+server 한 연결쌍 | 288 KiB |
| 1,000 연결쌍 | 약 281.25 MiB |

encoder 수치는 할당 크기 증가이며 RSS는 실제 사용·allocator/page 동작에 따라 달라진다. scratch는 해당 client write buffer가 준비될 때 발생한다. WS-1의 connection 수 100/1,000/10,000 메모리 수용성과 큰 payload 할당·지연 목표는 완료됐다고 판단할 수 없다. ALL-1이 B 전체를 부분 완료로 남긴 것은 타당하다.

### executor·수명

`ws_transport_common_internal.hpp:26–30`의 concrete executor는 `open()`에 전달되는 소유 `io_context`로 socket을 구성한다. 공통 read/write callback은 전달받은 generation의 `shared_ptr`를 capture한다(`:112–312`). WS/WSS close는 root를 move하고 해당 stream을 정리하므로 새 root와 이전 callback의 generation을 섞지 않는다(`ws_transport.cpp:61`, `wss_transport.cpp:70`). 포인터만 장기 저장하는 새 우회 lifetime은 보이지 않는다.

### W-ALL2-4 — 복수 report의 비교 조건은 gate가 보증하지 않음

`bindings/c/perf/ws_roundtrip_gate.py:39–101`는 각 report의 complete/expected/actual/RESULT count, skip/fail/unsupported, duplicate, finite positive 값을 검사한다. `:133–148`의 Q 공식과 두 transport 각각 `Q64/Q1 >= 0.80` 판정도 요구와 맞는다.

그러나 `load_cells()`는 서로 다른 파일의 셀을 그대로 합치며 source 정보는 중복 오류에만 쓴다. CLI는 복수 report를 허용하고(`:154–160`), 테스트 `bindings/c/perf/tests/test_ws_roundtrip_gate.py:117–123`는 1 KiB RR TCP와 WS/WSS가 서로 다른 파일에 있는 임의 분할도 허용한다. 같은 host/load/run인지 확인하지 않는다.

정적 계산 예: 테스트 CELLS를 앞 4개/나머지 8개로 나누면 기본 WS Q1=0.9, Q64=1, 비율=1.111…이다. 나머지 report 전체 값을 2배 하면 Q1만 1.8이 되어 비율=0.555…로 바뀐다. RR WS 64 KiB를 35→20으로 바꾼 실제 저하 예에서는 원래 비율 0.634921(FAIL)이 나머지 report 전체를 0.5배 한 입력에서는 1.269841(PASS)이 된다. 이는 실행 결과가 아니라 코드 공식에 대입한 값이다.

이 경고는 **서로 다른 측정 조건을 섞은 입력에 대한 보호 부재**다. 같은 측정의 올바른 split 파일이나 ALL-1의 단일 complete report 결과를 곧바로 무효화하는 신규 runtime 차단으로 세지 않았다. 여러 report를 공식 수용 증거로 쓸 때 비교 셀의 provenance를 함께 확인해야 한다.

## 6. D/E·D-ALL-1·WIN-1

`_slot_sync`의 재진입 제거는 socket constructor에서 후속 context schedule을 호출하던 원인을 제거한 변경이다. registry 안에서는 socket/slot을 생성·발행하고, schedule은 밖에서 한다(`core/ctx.cpp:232–275`). PAIR/DEALER/ROUTER/STREAM/PUB/SUB/XPUB/XSUB constructor의 같은 호출 제거를 확인했다. 단순히 recursive mutex 타입만 바꾸지는 않았다.

mailbox send/post는 반환 실패를 복구하는 공개 API가 아니라 command publication 내부 경계다. queue allocation은 기존 `alloc_assert` fatal 정책(`core/yqueue.hpp:44`, `:92`)이며, post의 allocation 예외도 `noexcept`로 fail-stop 처리한다(`core/mailbox.cpp:366–376`). 정상 복귀하면서 `_scheduled`나 lifetime pin만 남는 경로는 제거된다. **`std::terminate`에서 항상 stack unwind되어 lock이 풀린다고 설명해서는 안 된다.** 보장하는 것은 프로세스가 복구 불가능한 예약 상태로 계속 실행되지 않는다는 점이다. fork/SIGABRT 테스트는 플랫폼 guard가 있는 Unix 검증이며 Windows 실행 증거를 대신하지 않는다.

monitor 정지는 `process_stop()` turn 안에서 `stop()` control 경계로 이동했다(`common/socket_base.cpp:345–358`, `common/socket_base_lifecycle.cpp:1377–1386`). ctx는 registry에서 mailbox ref를 보유한 socket snapshot을 만들고 registry를 놓은 다음 **모든 source monitor를 먼저 정지하고 socket stop을 보낸다**(`core/ctx_termination.cpp:82–115`). monitor detach는 operation mutex로 교체와 직렬화하고 짧은 monitor mutex 밖에서 task drain·foreign socket 작업을 한다(`common/socket_base_monitor.cpp:839–897`). snapshot의 pin 반환과 예외 cleanup도 있다. 이 변경으로 제거한 역순 경계는 타당하지만 W-ALL2-2의 endpoint 경계까지 해소한 것은 아니다.

lb의 test-only peer-weight 관측자는 production writer와 같은 turn을 사용한다(`dealer.cpp:532`, `router_admission.cpp:483`). 제거한 `lb::send`는 `sendpipe`의 전달 wrapper였으며 test caller가 기존 소유 함수를 직접 사용한다. async callback의 completion drain → detach → reschedule은 모두 같은 lifecycle turn/기존 mailbox 소비자 경계로 수렴했다(`socket_base_lifecycle.cpp:1492–1572`). 별도 completion mutex와 turn의 역순 교착을 제거한다. callback의 바깥 deferred termination 호출도 실제 `xsocket_msg_pipe_terminated` 직전에 내부 turn을 획득한다(`common/socket_base_dispatch.cpp:70–75`).

추가로 결과 mapper의 EDEADLK→CLOSE_BUSY(`core/src/api/core/close_result_internal.hpp:18`)와 EPROTONOSUPPORT→CONNECT_NOT_SUPPORTED(`core/src/api/message/connect_result_internal.hpp:14`)는 기존 enum으로 내부 errno를 정규화하며 signature를 바꾸지 않는다. HWM integration fixture는 bind/connect 전에 SNDBUF/RCVBUF를 4096으로 고정한다(`core/tests/integration/test_router_mandatory_hwm.cpp:17–27`). 기존 메시지 수 assertion·HWM·대기를 완화하지 않고 OS buffer가 backpressure 관측을 가리는 변수를 제한한 변경이다. 다만 기본 OS-autotuned buffer 구성의 실행 증거를 대신하지는 않는다.

### D-ALL-1: 해소

첫 disconnect는 route의 비동기 종료를 시작하므로 곧바로 같은 호출이 NOT_FOUND일 필요가 없다(Socket README `:909–911`). 최종 test는 `pair.pump()`와 `router.get_peer_state()`를 반복하고 EHOSTUNREACH를 확인한 뒤 두 번째 공개 disconnect 결과를 검사한다(`unittest_phase3_request_reply_owners.cpp:1017–1034`). 이 probe의 EHOSTUNREACH는 단순히 writable=false가 아니라 **route map에 key가 없음**을 뜻한다(`router.cpp:474–485`). fixture에는 그 사이 같은 RID를 재연결하는 동작이 없다.

따라서 비동기 제거의 완료 조건을 관측하는 결정적인 predicate다. timeout 범위 내 실제 진행 여부는 여전히 scheduler에 달려 있지만, 한 번 pump하고 시간상 완료됐다고 추정하던 오류는 제거됐다. NOT_FOUND와 ENOENT, pending counters의 유지도 검사한다. 없는 대상의 공개 계약을 약화한 assertion 변경으로 보지 않는다.

### WIN-1: 해소(정적)

main의 `f5d7cccde2`에 도입된 `random.cpp`의 `SystemFunction036`/RtlGenRandom Windows 분기는 이번 diff에서 수정하지 않는다. `ctx_termination.cpp:122–139`의 term mailbox `EAGAIN` 재대기, EINTR 반환, done 확인도 보존돼 있다. ALL-1 D-1의 변경은 앞의 registry snapshot/monitor stop 구간(`:82–115`)이므로 두 변경의 의미상 덮어쓰기는 없다. 신규 pipe 분할 및 WS helper include에서 unguarded POSIX 전용 API는 발견하지 못했다. 실제 Windows compile/run은 이번 읽기 전용 범위 밖이다.

## 7. 잔여 W 항목과 hot path

### W-ALL2-1 — 무제한 command batch와 종료 중 turn 대기

`common/socket_base_lifecycle.cpp:506–527`는 queue가 EAGAIN을 반환할 때까지 turn을 유지한다. command producer가 지속적으로 채우면 public recv/send/readiness는 turn을 얻지 못한다. `_ctx_terminated` 확인도 batch 뒤 `:547`이고, `lock_public_api_sync()`의 CAS/backoff loop(`socket_lifecycle_runtime.cpp:415–434`)에는 종료 확인이나 취소 반환이 없다.

재현 순서는 C가 turn 획득 → 생산자가 C의 dequeue보다 빠르게 command 공급 → R이 recv turn CAS 재시도 → ctx stop 발행 → C가 아직 batch를 끝내지 못하는 순서다. 일반 blocking recv의 CV는 turn을 놓지만 **turn 획득을 기다리는 상태**에는 같은 해제/취소 규칙이 적용되지 않는다. 유한 batch가 끝나면 진행하므로 즉시 교착이라고 단정하지 않는다. synchronization11 `:35–41`은 FIFO 공정성까지 보장하지 않지만, ALL-2가 public recv 기아나 종료 지연을 닫았다는 근거도 없다. 새 정책·budget 추가로 임의 해결할 사안이 아니라 command 소유자의 진행 경계를 확인해야 한다.

### W-ALL2-2 — endpoint 종료의 기존 foreign turn/대기 위반

`common/socket_base_endpoint.cpp:1127–1140`는 자기 turn을 쥐고 `term_endpoint_internal()`을 부른다. inproc 종료는 `:964–981`에서 상대 socket의 `ensure_async_command_processing()`을 직접 호출한다. 이 함수는 async가 이미 active여도 먼저 상대 turn을 획득한다(`socket_base_lifecycle.cpp:1033`, `:1043`).

구체적인 순서는 같은 inproc endpoint의 binder A와 connector B(DEALER/ROUTER)가 동시에 endpoint 종료 → A/B가 자기 turn 소유 → 각각 상대 socket에 ensure 호출 → A는 B turn, B는 A turn을 기다림이다. binder는 `socket_base_endpoint.cpp:1055`, connector는 `:1078` → `socket_endpoint_runtime.cpp:101–123`으로 같은 helper에 도달한다. close는 admitted API 때문에 EBUSY이고 turn lock loop는 ctx 종료로 빠져나오지 않는다. 이 경로는 **부모에도 존재하므로 신규 patch 회귀로 세지 않는다.** 다만 전체 §3.1/§6 준수 주장은 성립하지 않는다.

같은 함수의 inproc ack 대기는 바깥 turn을 유지한 채 `process_commands(10)`을 부른다(`socket_base_endpoint.cpp:1064–1071`). 내부 `api_owner`가 caller turn을 빌린 경우 자신의 scope를 끝내도 바깥 turn은 남으므로 `socket_base_lifecycle.cpp:561`의 mailbox 대기는 §3.4의 무소유 대기가 아니다. network bound endpoint도 `:1112` → `core/object.cpp:489–501`의 future wait를 turn 아래서 수행한다. 이 경계에서 같은 I/O thread가 `core/session_base.cpp:247` → `socket_base_lifecycle.cpp:1309`의 socket owner progress turn을 필요로 하면 순환 대기가 가능하다. ctx registry/monitor 수정과 이 경계를 구분해서 관리해야 한다.

### W-ALL2-5 — 분할·플랫폼·회귀 검증의 적용 범위

ALL-1의 196/196 qualified function body SHA 일치는 **분할 직전과 직후**의 기계적 이동 증거다. 이후 `_out_sync`·회계·generation 의미 변경과 ALL-2 rebase를 포함한 최종 patch가 전부 무동작변경이라는 뜻은 아니다. CMake의 새 3개 cpp 등록(`core/CMakeLists.txt:866`)과 정의 이동은 확인했지만 이번 리뷰에서 새로운 body 비교 도구나 바이너리를 실행하지 않았다.

### W-ALL2-6 — 원자 연산 수와 성능 판정 한계

| 측정/경로 | 정확히 말할 수 있는 것 | 말할 수 없는 것 |
|---|---|---|
| uncontended receive turn 1회 | acquire CAS 1 + release `fetch_and` RMW 1, turn mutex 0. `socket_lifecycle_runtime.cpp:415`, `:437` | public recv 전체 원자 연산이 2개라는 주장 |
| successful physical recv의 command tick | `socket_command_runtime.cpp:21–24`의 `fetch_add(acq_rel)` 1회가 turn 앞에 추가된다(`socket_base_msg.cpp:663`). 따라서 이 두 범위만 합쳐도 CAS 1 + 별도 RMW 2 | helper lifetime/admission·pipe ledger·queue exchange·통계까지 포함한 전체 비용 |
| uncontended public send의 lifecycle scope | `enter_public_send():165`가 admission과 turn을 CAS 1회로 합치고 `leave_public_send():193`가 RMW 1회로 뺌 | send 전체가 lock 0이라는 주장. queue notification, correlation 등의 조건부 경계는 별도 |
| nested internal scope | 현재 thread가 이미 turn을 소유하면 추가 turn CAS/RMW 0 | wait 뒤 재획득, command 주기 poll, multipart 각 API의 비용까지 0이라는 주장 |
| stream_tcp lock/msg | ALL-1 raw 측정의 pthread lock 537,372 / message 64,079 = 8.3861. CCU20, 1024B, 10초, io4 | CAS turn 횟수나 message마다 고정 8.39개의 소스 lock이 있다는 의미 |
| turn 함수 계측 | ALL-1 `lock_public_api_sync` 3.015 call/msg, `xhas_in` 0.289, `pipe::check_read` 0.146 | pthread lock/msg에 turn 호출 수를 그대로 더한 동일 metric |

`recv_ticks`는 public receiver와 command/완료 경로가 turn 밖에서 관측·reset할 수 있어 공유 발행 근거가 있다. progress epoch/waiter도 waiter와 publisher 두 주체의 놓치지 않는 등록을 위한 C3이며, outbound total/provisional은 context observer 때문에 필요하다. 반면 max와 record commit의 결합은 §4의 단순 독립 reader 사례로 정당화할 수 없다(B-ALL2-1). HWM 재계획의 기존 relaxed 필드들은 새 release/acquire max 증명과 별개이며, §6의 문장을 모든 기존 필드가 이미 만족한다고 쓰면 과장이다.

ALL-1 보고서 `:87–90`, `:138–157`의 성능 수치는 다음 한계를 포함해 수용해야 한다.

- stream lock 기준 G1은 dev, 현재는 Release다. 재진입 검출 mutex 등 build 조건 차이가 있으므로 제거한 lock의 인과 효과와 총 처리량을 동일 조건 비교로 확정하지 않는다.
- 요청된 hotpath 5개 셀은 상한 +5% 해석에서 통과했으나 PAIR +3.55%, ROUTER-ROUTER TCP +1.99% 등은 감소가 아니다. 개선 셀을 symmetric gate가 FAIL로 표기한 결과와 사용자의 상한 기준을 구분한다.
- 최종 C multi RR 1024B는 203.540kops, 이전 phase2g 242.5k 대비 **−16.07%**다. 앞선 ALL-1 실행 303.326k와 변동이 커서 회귀 부재를 확정하지 못한다.
- WS/WSS Q64/Q1은 1.851217/2.585461로 비율 gate를 통과했다. 하지만 TCP RR 64 KiB가 36.157→16.098로 하락한 분모 영향이 크고 WS는 13.912→14.132였다. 절대 WS 왕복 성능·지연 개선을 같은 결론으로 취급하면 안 된다.
- WS-1의 같은 host/load 5-run median, connection RSS 수용성은 제출된 각 구현 1회 측정으로 충족되지 않는다. ALL-2 rebase 뒤 새 성능 측정도 없다.

## 8. S 항목 — 사양 문장 초안과 남은 설명

### S-ALL2-1 — synchronization11 문장 대조

보호된 스펙 파일은 수정하지 않았다. 아래는 승인 시 사용할 수 있는 명료화 초안이며, 위반을 허용하도록 계약을 바꾸자는 제안이 아니다.

| 절·현재 근거 | 구현과의 관계 | 문장 초안/처리 |
|---|---|---|
| §3.1 `:75–95` command와 public이 같은 turn | 구현과 부합. readiness/completion/whole-record가 표에 명시되지 않아 범위를 오독할 수 있음 | “socket의 scheduler·route·receive 상태를 조회하거나 바꾸는 readiness, completion drain, attach/termination도 같은 turn을 사용한다. 하나의 수신 record를 구성하는 내부 처리 동안 소유권을 유지하며, 이미 소유한 내부 호출은 caller의 turn을 빌린다.” |
| §3.3 `:133–152` mailbox owner 전환 | ALL-2의 async 상태는 실행 예약이며 별도 receive mode가 아님 | “mailbox의 비동기 실행자 설치·해제는 command 소비 주체를 정한다. 설치·해제가 socket C2 상태의 배타 방식을 바꾸지 않으며, 실제 command 적용은 같은 socket turn 안에서 수행한다.” |
| §3.4 `:159–176` 대기 중 turn 없음 | 일반 recv/CV는 맞음. 빌린 바깥 turn이 있는 endpoint 대기는 위반 | “대기 직전에는 가장 바깥 소유 scope도 turn을 해제해야 한다. 내부 함수가 빌린 scope를 끝낸 것만으로는 turn 해제가 아니다.” W-ALL2-2는 문장 변경으로 해소되지 않음 |
| §5 `:211–215` 같은 불변식 분할 금지·wait 금지 | B-ALL2-1과 W-ALL2-2에 이미 충분한 규칙 | 새 예외 불필요. “record 사전검증에 사용한 정책과 그 검증에 의존한 commit은 같은 유효성 경계에서 처리한다”를 §2의 예로 명료화 가능 |
| §6 `:233–235` 양쪽 SC fence라고 기술 | 구현은 explicit fence 대신 해당 변수의 SC RMW/load로 순서를 구성 | “등록과 알림 발행 양쪽은 필요한 store→load 순서를 보장하는 seq_cst 연산 또는 동등하게 증명된 fence 조합을 사용한다.” 단순 release/acquire만으로 충분하다는 뜻이 아님 |
| §6 `:229–232` foreign socket turn 금지 | endpoint의 기존 직접 ensure 호출은 위반 | 문장을 완화하지 않고 W-ALL2-2의 owner command 경계에서 해결할 항목 |
| ZMP §8/§9 | byte stream carrier와 현재 bytes bounded batch 유지 | 계약 수정 필요 없음. 128 KiB의 메모리 수용성은 별도 성능/메모리 검증 |
| Socket README §2/§6/`disconnect_rid` | turn은 close fail-fast·single consumer·completion 계약을 대체하지 않음. route 부재 검사는 기존 계약 그대로 | 공개 반환 계약 변경 필요 없음. B-ALL2-1은 기존 정상 오류/atomic admission에 맞춰 구현을 고칠 대상 |

### S-ALL2-2 — 이전 설계 주석 잔존

- `common/socket_lifecycle_runtime.cpp:32–37`: blocking recv가 API 전체 동안 sync를 소유한다는 설명은 현재 시도별 acquire/release와 맞지 않는다.
- `core/pipe_transport.cpp:53–61`: command/public receive가 별도 동기화 domain이라는 설명은 단일 turn 전제와 맞지 않는다. `:73`의 relaxed 설명도 실제 acquire와 맞춰야 한다.
- `core/src/runtime/sockets/pair/pair.cpp:95–99`: “under the pipe lock” 설명은 제거한 hot writer lock을 가리킨다.
- `core/ctx.cpp:219–220`: registry lock 안 monitor teardown을 전제한 설명은 snapshot 후 외부 stop으로 바뀐 순서와 맞지 않는다.
- `common/socket_base.hpp:1025`의 이전 receive sync 관련 설명은 새 runtime 선언과 함께 정리할 대상이다.

주석 정리는 요청 범위 밖 소스 수정이므로 수행하지 않았다. 새 RAII의 정상 반환·오류 반환·예외 unwind는 확인했으나, mailbox handler의 모든 기존 예외 경계를 새 no-throw 계약으로 증명한 것은 아니다.

## 9. 검증 증거와 채택 조건

| 증거 주체 | 실행/결과 | 이번 판정에서의 한계 |
|---|---|---|
| 이번 review-ALL-2 | Git diff·소스·보고서·기존 로그 읽기와 정적 순서 대조만 수행 | 새 실행 테스트 0. B-ALL2-1은 정적 도달 가능한 반례 |
| ALL-2 보고서 `:57–60` | dev 210/210; 관련 232/232; 신규 60/60; owners 20/20 | 테스트가 B-ALL2-1의 handshake/part interleaving을 고정하지 않음 |
| ALL-2 보고서 `:62–68` | TSan **209/210**, runtime warning 0. `test_stream_packet_progress/test_shutdown_during_drain` 실패 | 262,153개의 1-byte WS fragment를 3초 안에 적재하는 fixture 전제 실패로 보고됨. ALL-1의 210/210과 혼용 금지. 이번 리뷰에서 원인 실행 재확인 안 함 |
| ALL-2 보고서 `:64` | ASan 변경 13/13 | lifetime 관련 증거지만 모든 concurrent ordering 증명은 아님 |
| ALL-1 보고서 | hotpath·lock/msg·WS 비율·분할 hash | rebase 후 성능 실측/최종 patch 전체의 무동작변경 증거가 아님 |

WS 독립 검토의 mask·executor·메모리 근거와 추가 PAIR 반증 시도는 감독 리뷰에서 해당 소스를 다시 열어 확인한 뒤 채택했다. 복수 report 문제를 신규 B로 올리자는 후보는 단일 report의 올바른 판정까지 무효화하지 않으므로 W-ALL2-4로 분류했다.

채택 전에는 B-ALL2-1을 pipe admission 소유 경계에서 수정하고, 빠른 경로를 실제 통과하는 결정적 회귀 검증이 필요하다. W 항목을 새 runtime 차단으로 부풀리지는 않았지만, “종료·기아까지 모두 해소”, “B 전체 완료”, “최종 TSan 전체 통과”, “recv 전체 CAS 1+RMW 1” 같은 표현은 현재 증거로 사용할 수 없다.

소유 계층: Core socket turn, pipe admission 및 transport/session lifetime. Framework runtime 변경 없음.

스펙 조항: synchronization11 §2·§3.1·§3.2·§3.3·§3.4·§4·§5·§6, ZMP §8/§9, Socket README §2·§6·`disconnect_rid`.

교차언어 대조: 변경은 공통 Core ABI 아래이며 언어별 Framework 보상 경로는 없다. ALL-2가 보고한 C/C++/Go/Rust header mirror 12/12는 입력 증거이고, 이번 리뷰에서 binding 실행은 하지 않았다.

변경 분류: 의도는 A(기존 synchronization 계약 적응)+B(기존 결함 수정), D-ALL-1은 A. 이번 신규 발견 B-ALL2-1은 patch가 유발한 Core 논리 race이며 계약 변경(D)으로 해소할 대상이 아니다.

수정 전/후 규칙 수: receive/command 배타 장치 3종 → lifecycle turn 1종, fallback 진입·재검증·해제 3개 → 0개. 단, max 정책을 독립 C3로 간주한 부분은 record 불변식을 보존하지 못해 이 단순화 주장을 완성하지 못한다.

차단 항목 수 / 채택 가능 여부: 1 / 불가(B-ALL2-1 해소 후 재검토)
