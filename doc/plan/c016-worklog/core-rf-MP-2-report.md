# MP-2 — 동시 multipart 제출 지원 구현 보고서

> 기준 commit: `3a7db427bce2`  
> 구현 worktree: `/home/hep7hep7/project/zlink-work/mp2` (detached)  
> 구현 기준: `core-rf-MP-1-design.md` A안, `decisions.ko.md` D-B197·D-B198  
> 작업 시각: 2026-09-07 16:02–17:56 KST

## 결과

PAIR·DEALER·ROUTER의 SEND·REQUEST·REPLY multipart 조립 상태를 socket-wide 단일 owner에서 caller별 sequence slot으로 옮겼다. 각 caller의 `MORE`는 논리 buffer에만 보관되고, `FINAL`이 그 caller의 완성 record를 기존 complete-record admission으로 제출한다. 다른 caller의 미완성 record는 single `FINAL`, 다른 family, 다른 target의 독립 제출을 막지 않는다.

| 항목 | 결과 |
|---|---|
| 구현안 | MP-1 A안 그대로 적용 |
| 변경 분류 | **A — 감독자가 확정한 계약 적응** |
| 공개 인터페이스 | 변경 없음. `git diff --stat -- core/include core/src/libzlink.vers` 출력 없음 |
| ABI·enum·errno·option | 변경 없음 |
| 적용 socket/family | PAIR·DEALER·ROUTER의 SEND·REQUEST·REPLY |
| 제외 범위 | STREAM은 FINAL-only 유지. PUB·XPUB physical incremental marker 유지 |
| 소스·test diff | 24개 파일, +1,512/−509 |
| worktree 상태 | 미커밋 변경으로 보존. commit·stash·branch 변경 없음 |
| 스펙 문서 | 생성·수정·삭제 없음 |

## 소유권과 규칙 수

| 구분 | 현재 구현의 소유자 |
|---|---|
| API 호출 사이의 논리 part와 family/target/flags 불변 조건 | `part_helper`의 caller별 sequence slot |
| REPLY token checkout과 중복 거절 | 기존 ROUTER reply token registry |
| 완성 record의 HWM·route·rollback·원자 admission | 기존 socket complete-record submit과 pipe |
| FLOW/WEIGHT 지연 | pipe에 실제 incomplete multipart가 존재하는 구간 |
| close의 미완성 payload 폐기 | socket이 소유하는 `part_helper` state |

규칙 수는 설계 메모의 계산 기준으로 **수정 전 6 → 수정 후 3**이다. 수정 전의 단일 slot 독점, marker 상호 배제, suspend/resume, complete 진입 배제, FINAL marker 인계, public control lease를 caller별 sequence 정리, FINAL physical admission/rollback, pipe physical control 경계로 줄였다.

## 변경 파일과 함수

| 파일 묶음 | 주요 함수·자료구조 | 변경 내용 |
|---|---|---|
| `core/src/api/socket/part_helper_internal.hpp` | `send_caller_identity_t`, `send_sequence_store_t`, `handle_state_t` | `weak_ptr` thread-lifetime identity를 key로 사용하는 caller별 sequence 저장소 추가. payload나 raw socket을 TLS에 두지 않음 |
| `core/src/api/socket/part_helper_api.cpp` | `current_send_caller_identity`, `prepare_send_step_state_locked`, `take_buffered_send_record_locked`, `abort_send_step` | 첫 MORE에서 slot 생성, 같은 caller의 continuation만 조회, FINAL·오류에서 slot 분리, 만료 identity의 sequence를 helper mutex 밖에서 폐기 |
| `core/src/api/socket/part_helper_state.cpp` | `cleanup_socket` | close에서 모든 caller slot을 helper mutex 아래 분리하고 payload와 REPLY context를 mutex 밖에서 해제 |
| `core/src/api/socket/socket_message_send_api.cpp` | `submit_completion_aware_part`, `zlink_send_part`, `zlink_send_part_rid` | SEND의 MORE를 논리 staging으로 처리. DONTWAIT FINAL은 complete scope 한 쌍을 helper 진입부터 physical submit까지 재사용 |
| `core/src/api/socket/socket_request_reply_submit_api.cpp` | `submit_buffered_request_step`, `public_router_reply_submit`, `router_reply_sequence_context_t` | REQUEST caller별 buffer·ID 연속성 구현. REPLY checkout context를 caller slot에 보관하고 FINAL 성공 시 commit, 폐기 시 restore |
| `core/src/api/socket/socket_request_reply_{api,dispatch,internal,runtime_io}.cpp/.hpp` | `stage_request_payload_part`, `commit_router_reply_target` | socket-wide `public_router_reply_active/owner/token/target/checkout_token` 제거. registry가 token checkout의 단일 기준으로 남음 |
| `core/src/runtime/sockets/common/socket_base.hpp`, `socket_base_msg.cpp`, `socket_send_submit.cpp` | `begin_public_api_scope`, `request_admission_submit_scoped`, `try_request_admission_submit_fast` | MORE용 generic lifecycle scope와 DONTWAIT REQUEST FINAL의 외부 complete scope 재사용 경로 추가 |
| `core/tests/CMakeLists.txt` 및 integration/unit test 9개 | 아래 계약 test | caller 동시성, 수명, control, 기존 expectation을 확정 계약에 맞게 검증 |

## 수명과 lock 순서

| 경로 | 적용 순서와 해제 위치 |
|---|---|
| MORE | generic public lifecycle 진입 → helper mutex → caller buffer로 move → helper mutex 해제 → generic lifecycle 종료. scope는 한 쌍이며 physical sync는 얻지 않음 |
| blocking FINAL | generic lifecycle 진입 → helper mutex → 완성 buffer 분리·slot 제거 → helper mutex 해제 → generic lifecycle 종료 → 기존 blocking complete submit |
| DONTWAIT FINAL | complete lifecycle 진입과 physical sync 획득 → helper mutex → 완성 buffer 분리·slot 제거 → helper mutex 해제 → 같은 complete scope에서 physical submit → scope 종료 |
| 오류·close | helper mutex 아래 buffer와 REPLY context의 소유권만 분리하고, `zlink_msg_close`·registry restore·pipe lifetime ref 해제는 helper mutex와 physical sync 밖에서 수행 |

Caller identity에는 thread마다 한 번 생성되는 `shared_ptr` control block만 TLS에 둔다. Socket의 map은 weak ownership을 비교하므로 OS thread ID나 메모리 주소가 재사용되어도 새 thread가 이전 prefix를 이어받지 않는다. Slot은 첫 MORE 성공에서 생기고 FINAL 성공, family/target/flags 불일치나 다른 실패에 따른 sequence 폐기, socket close에서 제거된다. Thread 종료를 기다리지 않으며, 종료된 caller의 미완성 sequence는 다음 helper 접근 또는 close가 회수한다.

## socket별 동작

| 대상 | 구현 결과 |
|---|---|
| PAIR SEND | caller별 2-part 조립과 독립 single FINAL 지원 |
| DEALER SEND | caller별 조립 후 기존 DEALER pipe 선택과 complete admission 사용 |
| ROUTER SEND | caller별 RID 복사본을 보존하고 FINAL에서 기존 route 선택·admission 사용 |
| REQUEST | request sequence·pending cookie를 caller slot spec에 보존. DONTWAIT FINAL은 complete scope를 중첩하지 않음 |
| REPLY | 첫 MORE에서 token registry checkout. context를 caller slot이 보관하며 중복 token checkout은 registry가 거절 |
| STREAM | 기존 FINAL-only 검증과 제출 경로 유지 |
| PUB·XPUB | 기존 socket-wide physical incremental `send` slot, marker, suspend/resume, control boundary 유지 |

`socket_base_flow_state.cpp:73`, `socket_base_dispatch.cpp:529`, `socket_send_complete.cpp:422` 부근의 deferred-control 처리는 PUB·XPUB와 실제 incomplete pipe라는 다른 소유 이유 때문에 유지했다. PAIR·DEALER·ROUTER의 public staging은 `public_multipart_send_active`를 설정하지 않으므로 이 분기들의 지연 조건이 되지 않는다. 실제 pipe write가 시작된 뒤의 incomplete boundary는 기존 pipe가 계속 담당한다.

## 공개 계약 test

| 요구 | test와 검증 결과 |
|---|---|
| DEALER→ROUTER 4 thread × 2-part × 100 | `test_dealer_four_callers_stage_two_parts_independently`: 400 record의 caller·sequence·part를 전수 확인, 혼합·중복·누락 없음 |
| PAIR 동일 | `test_pair_four_callers_stage_two_parts_independently`: 4×100 전수 확인 |
| ROUTER `send_part_rid` 동일 | `test_router_four_callers_stage_two_parts_independently`: 같은 RID에 4×100 전수 확인 |
| REQUEST와 REPLY 동시 | `test_request_and_reply_four_callers_complete_independently`: REQUEST 4 caller×20, REPLY 4 caller×20; 80개 고유 completion과 2-part 무혼합 확인 |
| 다른 caller single FINAL | `test_other_caller_final_consumes_input_and_keeps_staged_sequence`: single record와 기존 2-part record가 각각 독립 수신 |
| 다른 caller family | `test_other_caller_different_family_is_independent`: SEND prefix 중 다른 caller의 REQUEST FINAL 성공 후 두 record 독립 수신 |
| close·thread identity | `test_pair_close_aborts_suspended_multipart_without_exposing_prefix`, `test_thread_lifetime_identity_does_not_continue_abandoned_sequence`: close OK/errno 0, prefix 미노출, 새 thread FINAL은 single |
| 같은 caller 불일치 | `test_wrong_send_helper_aborts_open_sequence`, `test_target_change_aborts_open_routed_sequence`, `test_same_thread_failure_aborts_non_publish_sequence`: family·RID·`NONE→DONTWAIT` flags 변경이 EINVAL, 해당 prefix 폐기 |
| FLOW/WEIGHT | `test_sl_controls_progress_during_public_staging`: public prefix가 열린 동안 PAUSED, WEIGHT, RUNNING 전달 확인 |
| HWM | 기존 `test_dontwait_hwm_is_immediate_atomic_and_pending_options_do_not_apply` 유지: 가득 찬 pipe에서 MORE OK, FINAL EAGAIN·WRITABLE token |

## 기존 test expectation 변경

| test/영역 | 이전 expectation | 확정 계약에 따른 expectation | 이유 |
|---|---|---|---|
| `test_helper_ownership` 다른 caller FINAL | EINVAL·기존 prefix 유지 | 독립 single FINAL 성공·기존 prefix 유지 | 다른 caller의 열린 sequence는 독립 |
| `unittest_complete_record_admission` | 열린 public multipart가 complete record와 다른 staging을 거절 | logical staging과 single/complete record 동시 허용 | 배제 경계가 public MORE가 아니라 physical admission |
| single-lane control boundary | public staging 동안 FLOW/WEIGHT 지연·coalesce | public staging 중 전달, physical incomplete 경계만 지연 | D-B198 D3 |
| REPLY 두 번째 token | socket-wide owner 때문에 EBUSY | token registry 조회 결과로 거절 | 중복 checkout 판정의 단일 소유자는 registry |
| REPLY OOM failpoint | FINAL continuation에서 checkout key 할당 실패 | 첫 MORE checkout에서 실패 | checkout 수명이 caller sequence 시작과 일치 |
| helper 내부 state assertion | `handle_state_t::send` 단일 slot | 현재 caller의 `send_sequence_state_t` | 공개 동작이 아닌 내부 소유 구조 변경 |

Expectation은 오류를 없애기 위해 완화하지 않았다. D-B198이 확정한 caller 독립성·physical control 경계·registry 단일 소유권을 직접 관찰하도록 바꾸었다.

## 검증 결과

| 검증 | 명령 범위 | 결과 |
|---|---|---|
| dev 전체 build | `cmake --build core/build-dev --parallel 4` | 성공 |
| dev 전체 ctest 1회 | 208개 전체 | 206/208. 당시 REPLY OOM expectation 1개는 계약 위치에 맞게 수정 후 통과. `hotpath_gate`는 RelWithDebInfo 수치를 Release reference와 비교하여 실패 |
| 최신 dev 비성능 전체 | `ctest ... -E '^hotpath_gate$'` | 206/207. 기존 `unittest_ctx_lifecycle` transport-owner timing assertion 1개가 병렬 실패했으나 즉시 단독 재실행 1/1 통과 |
| 관련 suite 반복 | 정규식 `part|multipart|send|request|reply|router|dealer|pair|flow|hwm|close|wake`, 80개 target, `until-fail:5` | 전부 통과(400회), 646.52초 |
| 마지막 flags test 포함 반복 | `test_helper_ownership`, `test_phase3_request_reply_contract`, `until-fail:5` | 10/10 통과 |
| lost-wake | 관련 wake 4개 target, `until-fail:20` | 80/80 통과, 633.00초 |
| ASan+LSan | `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1`, `test_helper_interleave` 6 case | 6/6 통과, sanitizer·leak 오류 없음 |
| Valgrind Memcheck | close/leak test 기동 | 호스트의 stripped `ld-linux`에서 `strcmp` 필수 redirection symbol을 찾지 못해 시작 불가. 요구한 대안 중 ASan+LSan으로 판정 |
| TSan build | GCC `-fsanitize=thread`, memory/atomic 계측을 끄는 저장소 기본 TSan option은 사용하지 않음, `--parallel 2` | 572 target build 성공. `libzlink.so`의 `__tsan_read*` 참조로 계측 확인 |
| TSan 신규·직접 변경 test | caller/close/REQUEST/REPLY/control state 관련 8개 target | 8/8 통과. 마지막 helper 변경 관련 3개도 재빌드 후 3/3 통과 |
| TSan 관련 suite | 같은 관련 정규식 80개, 1회 | 75/80 통과. 남은 항목은 아래 표 |
| diff 검사 | `git diff --check` | 통과 |
| 공개 API 검사 | `git diff --stat -- core/include core/src/libzlink.vers` | 출력 없음 |

TSan 실행은 ASLR의 `unexpected memory mapping`을 피하려고 `setarch ... -R`을 사용했다. 임시 suppression에는 기존 custom lock-free 경로 `receive_once_guarded`와 `ypipe::check_read`만 넣었으며 MP-2 함수나 caller-slot 경로를 숨기지 않았다.

| TSan 관련 suite의 남은 실패 | 관찰 결과 | MP-2 판정 |
|---|---|---|
| `test_router_reject_duplicate` | monitor/ctx/async mailbox의 기존 lock-order-inversion 보고 | 변경 파일 밖의 기존 runtime 잠금순서 |
| `test_router_reject_disconnected_without_app_recv` | 같은 monitor/ctx lock-order 계열 | 변경 파일 밖의 기존 runtime 잠금순서 |
| `test_router_same_socket_reconnect_policy` | 같은 monitor/ctx lock-order 계열 | 변경 파일 밖의 기존 runtime 잠금순서 |
| `test_router_mandatory_hwm` | TSan에서 10ms 제한을 43ms에 완료하여 timing assertion 실패 | 비계측 dev suite에서는 통과 |
| `unittest_flow_state_socket` | 기존 flow-state 동기화 보고 | 신규 control staging test와 MP-2 신규 8개는 통과; 별도 TSan debt로 남김 |

## hotpath gate

Release LTO library와 `hotpath_bench`를 `JOBS=4`로 빌드했다. 2026-09-07 17:40 KST의 시작 load average는 `3.31, 2.54, 2.55`였고, 지정된 `PERF_LOCK`을 얻은 상태에서 5셀을 한 번 실행했다. Reference는 갱신하지 않았다.

| cell | reference Ir/msg | measured Ir/msg | ratio | 변화율 | 판정 |
|---|---:|---:|---:|---:|---|
| `dealer_dealer_inproc` | 3230.922 | 3265.197 | 1.0106 | +1.06% | PASS |
| `dealer_router_reqrep_inproc` | 18663.506 | 18562.833 | 0.9946 | −0.54% | PASS |
| `pair_inproc` | 2348.457 | 2341.381 | 0.9970 | −0.30% | PASS |
| `router_router_tcp` | 2972.532 | 2924.961 | 0.9840 | −1.60% | PASS |
| `stream_tcp` | 14623.471 | 14234.082 | 0.9734 | −2.66% | PASS |

모든 셀이 ±5% 안이다. 이 값으로 성능 개선을 주장하지 않는다. `with_stream`과 `bindings/c/perf`는 실행하지 않았다.

## 스펙 초안과 구현 차이

| 항목 | §9 초안·추가 계약 | 구현 차이 |
|---|---|---|
| thread별 독립 sequence | 같은 record는 같은 thread, 다른 thread는 독립 | 없음 |
| slot 수명 | 첫 MORE 성공에서 생성, FINAL 성공·폐기·close에서 제거, thread 종료를 기다리지 않음 | 없음. 종료된 weak identity는 다음 helper 접근 또는 close에서 회수 |
| HWM | public staging은 pipe HWM 밖, physical frame 규칙과 total-known 예외는 유지 | 없음 |
| control | public staging 중 진행, physical incomplete record에서만 지연 | 없음 |
| payload 실패 계약 | FINAL 실패 후 payload 미보관, token/target/context만 기존 계약에 따라 유지 | 없음 |
| 공개 계약 | API·ABI·errno mapper 불변 | 없음 |

## 교차언어 대조

| 구현 | 대조 결과 |
|---|---|
| C++ | `zlink_send_part*`, `zlink_request_part`, `zlink_reply_part`를 Core에 직접 위임 |
| .NET | `NativeMethods.Socket.cs`가 같은 Core symbol을 호출 |
| Java | `Native.java`의 downcall이 같은 Core symbol을 호출 |
| Rust | `ffi.rs`와 messaging operation이 같은 Core symbol을 호출 |
| Go | native adapter가 같은 Core symbol을 호출하며 multipart 제출 구간을 `LockOSThread`로 고정 |
| Node | native addon이 같은 Core symbol을 호출 |

언어별 Framework runtime에 같은 owner·map·retry 규칙을 추가하지 않았다. 모든 binding이 Core의 caller-slot 계약을 공유하므로 한 언어만 별도 변경할 구조적 이유가 없다.

## 남은 위험

| 위험 | 현재 완화와 후속 판단 |
|---|---|
| 동시 미완성 record 메모리 | pipe HWM은 logical staging의 byte 상한이 아니다. 새 option·hidden cap·timer를 추가하지 않았으며 close/실패에서 전량 회수한다 |
| caller slot 비용 | 첫 MORE에서 map node와 sequence가 할당되고 조회는 O(log k)다. single hotpath에는 slot을 생성하지 않으며 5셀 gate가 모두 통과했다 |
| 종료된 thread의 abandoned prefix | 다음 helper 접근이 만료 weak key를 회수하고 socket close가 전량 회수한다. thread ID 재사용으로 이어지지 않음을 공개 test로 검증했다 |
| TSan 전체 green 미달 | 신규·직접 변경 target은 green이다. monitor/ctx lock-order, flow-state, TSan timing 한계는 별도 기존 runtime debt로 남는다 |
| dev 전체 병렬 flake | `unittest_ctx_lifecycle` 한 assertion이 병렬 1회 실패하고 단독 통과했다. MP-2 변경 경로와 겹치지 않으며 남은 실패로 기록한다 |

## 필수 완료 분류

| 항목 | 판정 |
|---|---|
| 소유 계층 | Core `part_helper`가 caller별 조립·폐기를, request/reply registry가 token checkout을, socket/pipe가 physical admission·control 경계를 소유 |
| spec 근거 | MP-1 §1·§3.1·§3.2·§4.1–§4.3·§6.2·§7과 감독자 확정 §9, D-B197·D-B198 |
| 교차언어 | C++/.NET/Java/Rust/Go/Node가 동일 Core C symbol을 사용. Go의 OS-thread 고정도 대조했으며 Framework 보상 구현 없음 |
| 변경 분류 | **A — 확정 계약 적응** |

