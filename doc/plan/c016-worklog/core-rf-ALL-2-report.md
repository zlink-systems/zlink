# Core RF ALL-2 결과

## 결론

ALL-1 누적 변경을 현재 `origin/main` `14ec63e2c45068baa56735ae0390e8307984fe02` 위에 옮겼다. 충돌한 receive 소유권 코드는 트랙 1의 별도 `receive_owner` word·fallback·handoff protocol 대신 ALL-1의 단일 lifecycle turn 설계로 정리했다. 트랙 1이 지적한 배타·progress·종료 경계는 아래 표처럼 단일 turn 또는 기존 수명 소유자에서 닫힌다. D-ALL-1은 runtime을 바꾸지 않고 비동기 route 제거 완료를 관측한 뒤 공개 계약의 `NOT_FOUND`/`ENOENT`를 검사하도록 고쳤다.

변경은 detached worktree `/home/hep7hep7/project/zlink-work/all`에 미커밋 상태로 남겼다. stash·commit·스펙 수정·성능 측정은 하지 않았다.

## 보존과 재적용

- ALL-1 시작 기준: `25355fcfc5`.
- 현재 기준: fetch 뒤 detached `origin/main` `14ec63e2c45068baa56735ae0390e8307984fe02`.
- 사전 tracked patch: `/tmp/zlink-all2.Vu3NrK/all1-before-all2.patch`, SHA-256 `266fcf5b187b0b0540bd05eddeaa58d731da878d020b14bc6a1a18315e172270`.
- 사전 untracked 목록: `/tmp/zlink-all2.Vu3NrK/all1-untracked-files.txt`, SHA-256 `344411bd9d4b29bd82b7066e0b1cc3acee36aa743357a3c11b1022ca49752e7b`. 당시 8개 파일 원본도 `/tmp/zlink-all2.Vu3NrK/untracked-snapshot/`에 보존했다.
- tracked patch는 `git apply --3way`로 적용했다. 당시 신규 8개 중 `test_stream_concurrent_pull_send.cpp`는 현재 main에서 이미 tracked이므로 원본 내용으로 교체했고, 나머지 7개는 신규 파일로 복원했다.

충돌은 다음 4개였다.

| 파일 | 해결 |
|---|---|
| `core/src/runtime/sockets/common/socket_runtime.hpp` | 트랙 1의 확장 `receive_owner` word, `enter_receive_exclusion`, `lease_handoff_sync`를 제거하고 receive/readiness/command가 기존 lifecycle turn 하나를 쓰는 ALL-1 runtime을 채택했다. `progress_epoch`와 `waiters`의 atomic 발행은 유지했다. |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp` | command batch 전체를 public API turn 안에서 처리하고 async owner를 turn 획득 뒤 재확인하는 ALL-1 경로를 채택했다. track 1의 owner transition·handoff 코드는 남기지 않았다. |
| `core/src/runtime/sockets/common/socket_base_msg.cpp` | public receive와 whole-record 범위를 같은 lifecycle turn으로 묶는 ALL-1 RAII를 채택했다. mutex fallback 분기는 제거했다. |
| `core/src/runtime/sockets/common/socket_base_api.cpp` | `has_in`, completion drain, attach/xattach와 receive-state mutation이 같은 turn을 쓰는 ALL-1 구현을 채택했다. |

트랙 1 테스트의 핵심 workload는 ALL-1 버전에 포함했다. STREAM bounded `100×40`, HWM `4096`, payload `64`와 unbounded `40×40`, HWM `0`, payload `64`가 각각 `test_stream_concurrent_pull_send.cpp:507,514`에 있고, DEALER/ROUTER 동시 pull/send도 유지된다. client별 `io_context`와 socket을 client thread가 소유하고 같은 thread에서 취소·정리하므로 기존 cross-thread close와 context 수명 결함도 제거된다.

## 트랙 1 결함과 ALL-1 설계 대조

| review-st1/st2 결함 | ALL-1에서 닫히는 방식 | 근거 |
|---|---|---|
| mutex fallback 진입자 재검증 | 별도 receive owner 상태와 fallback 선택 자체를 제거했다. public receive, readiness, command가 동일 lifecycle turn을 획득하므로 지연된 fallback entrant가 새 lock-free owner와 겹칠 상태 전환이 없다. | `socket_runtime.hpp:280-282,312-315,352-372`, `socket_lifecycle_runtime.cpp:58-70` |
| async→available 전환 배타 | async 설치는 command drainer만 바꾸며 socket-state 배타 방식은 바꾸지 않는다. command는 turn 획득 후 async owner를 다시 확인하고, `command_drain_active` 표식도 turn을 놓기 전에 해제한다. 따라서 async 값을 지우며 이미 진입한 fallback을 추적하는 별도 해제 규칙이 없다. | `socket_base_lifecycle.cpp:389-437`, `socket_runtime.hpp:280-282` |
| `progress_epoch` data race | epoch와 waiter를 atomic으로 두고 등록·발행을 seq_cst로 교차시킨다. publisher는 `publish_receive_progress()` 하나이고, waiter 등록은 turn 안에서 한 뒤 turn을 놓고 plain mutex CV에서 기다린다. | `socket_runtime.hpp:318-332`, `socket_base_lifecycle.cpp:1575-1605` |
| `has_in` 편입 | buffered public part가 아닌 실제 `xhas_in()` readiness 접근은 같은 `socket_receive_entry_scope_t`를 획득한다. | `socket_base_api.cpp:1029-1048` |
| control attach | endpoint의 direct attach가 호출하는 실제 scheduler/route mutation인 `xattach_pipe()`와 termination 대칭 경로를 같은 receive entry turn에 넣었다. pair table·monitor lock은 각자 소유 상태만 보호한다. | `socket_base_api.cpp:442-494`; 호출 경계 `socket_base_endpoint.cpp:650-704` |
| TLS/context 종료와 테스트 수명 | WS/WSS callback은 정확한 connection generation의 `shared_ptr`를 유지하고 close는 root를 move한 뒤 cancel/shutdown/close한다. context 종료는 registry 안에서 mailbox ref snapshot만 만들고 registry lock을 놓은 뒤 monitor/socket을 정지한다. 회귀 테스트도 socket 취소·파괴를 소유 client thread에서 수행한다. | `wss_transport.cpp:70-107`, `ws_transport.cpp:61-100`, `ctx_termination.cpp:82-115`, `test_stream_concurrent_pull_send.cpp:169-265` |
| CV 비재진입 | receive progress CV는 lifecycle turn의 recursive lock을 재사용하지 않고 별도 plain `mutex_t progress_sync`를 쓴다. turn 안에서 waiter를 등록한 뒤 turn을 해제하고 CV에 진입하므로 command publisher가 재진입 없이 진행한다. | `socket_runtime.hpp:329-332`, `socket_base_lifecycle.cpp:1583-1605` |

설계 대안은 (1) 트랙 1의 owner word·fallback·handoff protocol을 main 위에서 계속 확장하는 안과 (2) socket receive/readiness/command를 기존 lifecycle turn 하나로 합치는 안을 비교했다. 후자는 같은 socket state에 소유자 하나와 배타 규칙 하나만 남기며 전환 상태와 fallback 규칙을 없애므로 채택했다.

수정 전/후 규칙 수: receive/command exclusion 3종 → lifecycle turn 1종, fallback 진입·재검증·해제 규칙 3개 → 0개, progress publisher 1개 → 1개.

## D-ALL-1

`unittest_phase3_request_reply_owners.cpp:1013-1034`를 공개 `disconnect_rid` 계약에 맞췄다. 첫 disconnect 직후 한 번만 `pair.pump()`하는 초안은 focused 실행에서 route command를 아직 처리하지 못해 두 번째 호출이 `OK`인 사례가 재현됐다. 최종 fixture는 기존 `kWaitMilliseconds` 안에서 `pair.pump()`를 진행하고 내부 route probe가 `EHOSTUNREACH`를 반환해 제거 완료를 직접 확인한 다음, 두 번째 공개 호출이 `ZLINK_CONNECT_NOT_FOUND`와 `ENOENT`를 반환하는지 검사한다. reconnect, timeout 증가, assertion 완화, runtime 변경은 없다.

Socket 공통 계약 `core/doc/spec/core/socket/README.ko.md:909-921`의 없는 `disconnect_rid` 결과를 그대로 검증하는 테스트 계약 적응이다.

## 검증

모든 빌드 전 `pgrep -c -x ninja`가 0이고 available memory가 10.7GB 이상임을 확인했으며 `JOBS=4`, foreground로 실행했다.

| 검증 | 결과 | 로그 |
|---|---|---|
| dev build | 성공. 최종 D 수정 뒤 incremental rebuild도 성공 | terminal; 기존 `unittest_monitor_ready_drain` GCC `stringop-overflow` 경고 1건 |
| 전체 dev `ctest -E hotpath_gate` 1회 | 210/210, 249.23초 | `all2-dev-full-ctest.log` |
| 관련 regex `until-fail:2` | 116 target × 2 = 232/232, 264.77초 | `all2-dev-related-repeat2.log` |
| 신규 6 target `until-fail:10` | 60/60, 32.51초 | `all2-dev-new-repeat10.log` |
| D owners `until-fail:20` | 20/20, 13.03초 | terminal |
| TSan build | 성공. 기존 GCC `atomic_thread_fence`의 compile-time `-Wtsan` 경고는 관측했으며 runtime report와 구분했다. | `all2-tsan-build.log` |
| TSan 전체 `ctest -E hotpath_gate` 1회 | 209/210, 430.68초. `WARNING: ThreadSanitizer` 0, summary 0, data-race/lock-order/thread-leak 0 | `all2-tsan-full-ctest.log` |
| TSan 첫 실패 분리 | `test_stream_packet_progress/test_shutdown_during_drain` 단독도 `transport did not queue all fragments`로 실패. 262,153개 1-byte WS fragment를 3초 안에 적재한다는 기존 fixture 경계이며 sanitizer 보고는 없다. 전체 suite는 재실행하지 않았다. | terminal |
| ASan 신규·변경 13 target | 13/13, 12.42초. leak/error report 없음 | `all2-asan-changed-13.log` |
| 공개 interface | `core/include`, `core/src/libzlink.vers` diff 0; C/C++/Go/Rust 요구 header mirror 12/12 | final static check |
| patch 정적 검사 | `git diff --check HEAD` 통과 | final static check |

남은 실패는 TSan의 기존 `test_shutdown_during_drain` fixture 시간 경계 1건이다. timeout·budget·입력·기대값은 바꾸지 않았다. dev 전체와 ASan 변경 대상에서는 같은 target이 통과했고, TSan runtime warning은 없다.

## 최종 누적 patch

- 파일: `/home/hep7hep7/project/zlink-work/all-artifacts/ALL-2-cumulative.patch`
- 생성: 신규 7개에 `git add -N`을 적용한 뒤 `git diff HEAD`.
- 크기: 507,426 bytes.
- SHA-256: `0514d34c567b7533a33f4d498bf708c7a68328e358625fda809d69d6af981b14`.
- stat: 63 files changed, 5,211 insertions(+), 4,748 deletions(-).

소유 계층: Core socket lifecycle turn이 socket receive/readiness/command C2 상태를 소유하고, transport connection generation이 WS/WSS 비동기 수명을, context registry가 socket 종료 snapshot을 소유한다.

스펙 조항: synchronization11 §2(C1/C2/C3)·§3.1–3.5·§4·§5·§6, Socket 공통 §2·§6·`disconnect_rid`(`README.ko.md:909-921`). 공개 signature·반환 계약·monitor event·completion record는 바꾸지 않았다.

교차언어 대조: Framework runtime 변경은 없고 모든 binding은 같은 Core ABI를 사용한다. C/C++/Go/Rust 요구 raw header mirror 12/12가 동일하며 언어별 우회는 추가하지 않았다.

변경 분류: ALL-1 누적 runtime은 A(기존 synchronization 계약 적응)+B(기존 결함 수정), D-ALL-1은 A(공개 반환 계약에 대한 테스트 적응)다. C 우회와 D spec gap 구현은 없다.
