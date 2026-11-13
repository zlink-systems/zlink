(1) 빈 queue에서 모든 frame이 준비된 complete record 한 건을 기존 whole-message 예외로 수락하도록 Core를 수정했다. 작업 위치는 /root/worktree/zlink-4118-multipart이고 branch는 fix/4118-multipart-resume이다. 산출물은 이 report.md와 change.diff다. 변경 파일은 31개이며 전체 목록은 raw/final-changed-files.txt에 있다. 최종 diff --check는 통과했고 로그는 raw/final-diff-check.log다. 최종 보관 크기는 cmfix 590 MiB와 C++ perf build 500 KiB로 25 GiB 미만이다. 크기와 최종 library digest 대조 결과는 raw/final-artifact-sizes.txt와 raw/final-artifact-audit.log에 있다.

core/src/runtime/core/pipe.hpp:47의 pipe_write_mode_t가 write_message_unlocked의 enforce_hwm_·enforce_incremental_hwm_ 인자 두 개를 대신한다. core/src/runtime/sockets/common/socket_runtime.hpp:843에서 현재 send scope가 가진 complete-record 문맥을 읽어 기존 socket dispatch와 pipe write에 전달한다. PAIR·DEALER·ROUTER·load balancer는 그 문맥을 전달하고 STREAM·PUB/SUB의 override 서명도 맞췄다. 제품 runtime에 저장되는 flag, counter, queue, lock과 TLS는 추가하지 않았다.

core/src/runtime/core/pipe_write.cpp:1227의 기존 _out_multipart_started_empty가 첫 MORE 직전 queue의 empty 자격을 고정한다. core/src/runtime/core/pipe_write.cpp:1244의 한 식이 generic final과 complete record의 MORE를 함께 판정한다. 크기를 아직 모르는 incremental 호출은 기본 mode를 유지한다. 현재 scope의 입력을 전달하므로 다른 thread에서 대기 중인 complete submit의 수가 이 자격을 바꾸지 않는다. Public single-part 호출은 MORE를 제거하고 multipart 호출은 전체 배열을 가진 complete scope를 사용한다. 호출 근거는 raw/complete-context-api-evidence.txt와 raw/complete-context-caller-audit.txt에 있다.

core/src/api/socket/socket_request_reply_runtime_io.cpp:1461의 send_completion_staged_frames_on_pipe에도 complete-record mode를 전달한다. 이 helper는 모든 reply part를 복사한 뒤 쓰며 DEALER 쪽 Application pipe도 사용한다. 두 기존 multipart write 호출에 같은 입력을 전달하여 pipe의 예외 판정을 사용한다. 외부 직접 writer를 대조한 결과 session decoder는 기본 incremental mode를 유지한다. 근거는 raw/direct-writer-audit-corrected.txt에 있다.

core/src/runtime/core/pipe_write.cpp:1212에서 payload 합계의 MAXMSGSIZE를 먼저 검사한다. HWM 예외도 기존 can_commit_bytes_with_peer_snapshot_unlocked를 사용하므로 현재 in-flight charge가 0인 경우에만 적용된다. Final commit 전의 shallow copy·rollback과 공개 C API의 모든 입력 slot 소비는 기존 경로를 사용한다. Generic HWM 거부 분기의 도달할 수 없는 MAXMSGSIZE 재분류는 제거했다.

빈 queue 예외의 자격식은 generic final 2→1곳이고 generic complete record는 0→1곳이다. 수정 후 두 경우는 core/src/runtime/core/pipe_write.cpp:1244의 같은 식을 공유한다. Single-frame 고정 true 전달은 기존 978·984행에서 현재 982·988행의 2→2곳이다. PAIR fast의 고정 false 전달은 기존 889행에서 현재 893행의 1→1곳이며 fast 자체의 예외 허용은 0→0곳이다. 중복 없이 센 자격식·고정 정책 전달 위치는 5→4곳이다. 최초 HWM 용량 검사의 false 전달 뒤에 예외 자격을 판정한다. Generic과 single-frame의 예외 허용을 결정하는 소유 함수 수는 2→2곳이다.

Fast와 generic 사이의 중복 검사 4종을 다음과 같이 검토했다. 각 입력 경계의 preflight와 fallback 검사를 유지했다.

준비 상태: core/src/runtime/core/pipe_write.cpp:859 / core/src/runtime/core/pipe_write.cpp:462, fast subset와 generic admission을 각각 유지했다.
Charge/overflow: core/src/runtime/core/pipe_write.cpp:881 / core/src/runtime/core/pipe_write.cpp:1196, complete-record 사전 합산과 frame별 누적을 각각 유지했다.
MAXMSGSIZE: core/src/runtime/core/pipe_write.cpp:891 / core/src/runtime/core/pipe_write.cpp:1212, fast snapshot preflight와 generic frame별 검사를 각각 유지했다.
HWM: core/src/runtime/core/pipe_write.cpp:893 / core/src/runtime/core/pipe_write.cpp:1233, fast subset의 거부 뒤 generic이 최종 admission을 판정한다.
Generic 내부의 중복 MAXMSGSIZE 오류 분류는 기존 main core/src/runtime/core/pipe_write.cpp:1237-1240,1258-1262에서 제거했다.
Generic final과 complete record가 공유하는 자격식: core/src/runtime/core/pipe_write.cpp:1244.
Single-frame true 전달: core/src/runtime/core/pipe_write.cpp:982, core/src/runtime/core/pipe_write.cpp:988. PAIR fast false 전달: core/src/runtime/core/pipe_write.cpp:893.

리팩토링 점검에서 불필요한 오류 재분류 1건을 발견하여 제거했고 이관한 항목은 0건이다. 저장 상태와 동기화의 판정 위치는 늘리지 않았다.

소유 계층은 Core의 pipe admission이다.
소유 spec은 core/doc/spec/core/systems/06-auto-hwm.ko.md:476이며, 최종 크기를 모르는 multipart에는 470–474행을 그대로 적용한다.
언어 대조는 공개 C API와 C++ binding을 같은 worktree Core로 실행했다. Framework runtime은 변경하지 않았다.
변경 분류는 B, 기존 Core 결함 수정이다. Spec은 변경하지 않았다.

(2) 빌드는 cpp-build-slot.sh에서 -j3, Release, -O3 -DNDEBUG, ENABLE_LTO=OFF로 실행했다. Core는 repo의 test 설정인 ZLINK_BUILD_TESTS=ON·BUILD_TESTS=ON을 사용했다. C++ contract 실행 파일에는 repo 설정의 -UNDEBUG가 적용되며 -g는 없다. Core 최종 빌드 로그는 raw/core-reply-final-build.log, C++ 빌드 로그는 raw/cpp-changed-build.log다. 설정은 core-build/CMakeCache.txt와 cpp-build/CMakeCache.txt, 실제 flag는 raw/release-flags-and-progress.txt에 있다.

최종 테스트는 measure-shared.sh 안에서 실행했다. Core unit/contract label 71/71개가 11.27초에 통과했고 integration으로만 분류된 이름상의 contract 9/9개가 20.96초에 통과했다. C++의 unit|contract 선택에는 등록된 contract 25개가 있으며 25/25개가 1.77초에 통과했다. 실패와 CTest skip은 0건이다. 로그는 raw/core-unit-contract.log, raw/core-additional-contract.log, raw/cpp-unit-contract.log이며 각 -cases.log에 개별 case 출력을 보존했다. 전체 실행과 실제 library 경로·digest는 raw/final-tests-runner.log, raw/final-cpp-runtime.log, raw/final-tested-core-sha256.txt에 있다. 이 실행들의 최소 MemAvailable은 25.70 GiB였다.

core/tests/integration/test_router_complete_record_hwm.cpp의 C API suite는 수정 전 baseline에서 9개 중 6개가 실패했고 수정본에서 9개가 모두 통과했다. 로그는 raw/core-baseline-final-regression.log와 raw/core-unit-contract-cases.log다. Auto HWM을 끄고 ROUTER→ROUTER inproc에서 HWM 16의 6 B+1 KiB, 1 KiB+4 B, 6 B+512 B+512 B가 PAUSED 때 BACKPRESSURED된 뒤 WRITABLE/SEND_ADMITTED를 받고 재제출되어 모든 frame을 수신하는지 검사했다. 같은 shape의 HWM 2048, single-frame HWM 16/2048 control, record 합계의 MAXMSGSIZE 초과/EMSGSIZE, non-empty queue의 두 번째 oversized record 대기, 거부 prefix의 rollback, 동시 두 submit 중 한 건만 admission되는 경우도 통과했다. 각 반환 결과에서 입력 slot의 소비를 확인했다.

core/tests/unittest/unittest_router_pipe_contract.cpp:910는 complete-record 예외를 pipe 수준에서 검사한다. PAIR fast subset이 거부한 뒤 generic write가 6 B+512 B+512 B를 완성하고, 둘째 oversized record는 첫 record를 읽기 전 거부되는지 검사했다. core/tests/unittest/unittest_router_pipe_contract.cpp:975는 첫 prefix 뒤 기존 record가 dequeue되어도 empty 자격이 바뀌지 않는지 검사한다. 두 테스트 모두 통과했다.

기존 incremental 검사인 test_empty_pipe_oversize_exception_applies_only_to_complete_message, test_empty_pipe_incomplete_multipart_stops_at_max_message_size, test_decoder_reservation_enforces_incremental_hwm_and_final_oversize, test_auto_hwm_blocked_ratio_counts_only_first_physical_attempt는 변경 없이 통과했다. test_drained_pipe_oversize_multipart_uses_fresh_peer_credit도 변경 없이 통과했다. Decoder의 pipe_receive.cpp와 unittest_auto_hwm_policy.cpp는 변경하지 않았다. 본문 대조는 raw/final-incremental-tests-unchanged.log에 있고 최종 실행 결과는 raw/core-unit-contract-cases.log에 있다.

core/tests/integration/test_phase3_request_reply_contract.cpp:384는 HWM 16의 빈 queue에 6 B+1 KiB reply를 쓰는 공개 C API 재현이다. Mode 전달 보완 전 1개가 실패했고 보완 후 1개가 통과했으며 전체 request/reply contract에서도 통과했다. 로그는 raw/reply-context-repro.log, raw/reply-context-fixed.log, raw/core-additional-contract-cases.log다. Reply의 입력 slot 소비와 request completion의 두 frame 내용도 검사했다.

bindings/cpp/tests/contract/test_cpp_contract_multipart_resume.cpp는 async multipart를 HWM 16에서 PAUSED receiver에 제출하고 resume 뒤 admission 완료와 모든 frame 수신을 검사한다. 세 shape를 기본·명시적 sender routing ID 각각으로 실행한 6개 경우가 모두 통과했다. 수정 전 Core에서는 pending.await_ready assertion으로 실패했다. 전후 로그는 raw/cpp-baseline-regression.log와 raw/cpp-unit-contract-cases.log다. 수정본의 남은 테스트 실패는 0건이다.

(3) HWM 범위 안에서 admitted되는 frame은 수정 전 core/src/runtime/core/pipe_write.cpp:1192의 charge 누적, core/src/runtime/core/pipe_write.cpp:1208의 MAXMSGSIZE, core/src/runtime/core/pipe_write.cpp:1223의 첫 MORE snapshot, core/src/runtime/core/pipe_write.cpp:1229의 HWM 검사와 publish/commit을 실행했다. 수정 후에는 core/src/runtime/core/pipe_write.cpp:1196, core/src/runtime/core/pipe_write.cpp:1212, core/src/runtime/core/pipe_write.cpp:1227, core/src/runtime/core/pipe_write.cpp:1233, core/src/runtime/core/pipe_write.cpp:1300에서 같은 단계가 실행된다. HWM 범위 안이면 core/src/runtime/core/pipe_write.cpp:1244의 예외 자격식은 실행하지 않는다. 전후 소스 구간은 raw/admitted-path-final-evidence.log에 있다.

기존 boolean 인자 두 개를 enum 하나로 합쳤으며 scope의 mode를 읽고 전달하는 위치는 core/src/runtime/sockets/common/socket_runtime.hpp:843와 core/src/runtime/sockets/common/socket_base_msg.cpp:356이다. 추가 whole-record scan이나 flag store, atomic, lock은 없다. Mode 전달에 따른 비용을 확인하기 위해 요청한 ROUTER 2-part throughput 비교를 실행했다.

측정은 flock -o /root/jobs/fx/cx/measure.lock 안에서 bindings/cpp/perf/single/build/cpp_perf_router_router의 같은 실행 파일로 수행했다. Inproc, 1024 B+빈 tail의 2-part, 실행당 5초, auto HWM OFF, endpoint HWM 8 MiB다. 6쌍을 AB/BA 순서로 교대했다. Baseline은 작업 시작의 제품 source인 사용자 지정 main 4ed813364f를 같은 Release/no-LTO/Tests ON 설정으로 빌드한 library다. Baseline digest는 b943abfa6107d2d656e96fb11785b215ba6e8e17f383ce10425fc616faddb926이고 수정본 digest는 610ada0eacf148235b717e492762863b1df74af10700dacfe8abceb7f17bc309다. 매 실행의 ldd와 MemAvailable을 보존했으며 최소 MemAvailable은 25.62 GiB였다.

1쌍은 main→change 순서로 실행했다. Main 1,039,625.000 record/s, 수정본 1,063,542.800 record/s, 비율 변화 +2.301%다.

2쌍은 change→main 순서로 실행했다. Main 1,007,037.000 record/s, 수정본 1,054,925.200 record/s, 비율 변화 +4.755%다.

3쌍은 main→change 순서로 실행했다. Main 1,055,795.000 record/s, 수정본 1,061,529.600 record/s, 비율 변화 +0.543%다.

4쌍은 change→main 순서로 실행했다. Main 1,062,677.000 record/s, 수정본 1,040,409.000 record/s, 비율 변화 -2.095%다.

5쌍은 main→change 순서로 실행했다. Main 1,064,050.400 record/s, 수정본 1,068,488.800 record/s, 비율 변화 +0.417%다.

6쌍은 change→main 순서로 실행했다. Main 1,051,386.800 record/s, 수정본 1,069,240.000 record/s, 비율 변화 +1.698%다.

Main과 수정본 throughput 중앙값은 1,053,590.900와 1,062,536.200 record/s로 +0.849%다. 쌍별 change/main 기하평균은 1.012484로 +1.248%이고 paired 중앙값은 1.011206로 +1.121%다. 5쌍은 상승하고 1쌍은 하락했다. 전체 결과는 raw/perf-final-exclusive.log, raw/perf-final/perf-results.json, raw/perf-final/perf-summary.json이며 각 원시 출력은 raw/perf-final/perf-pair-*-main.log와 raw/perf-final/perf-pair-*-change.log다. 설정과 실행 파일·library digest는 raw/perf-final/perf-metadata.json에 있다. 비교 범위는 위의 정상 multipart admission 조건이다. Reply 문맥 보완 전 library의 선행 결과는 raw/perf-exclusive.log와 raw/perf-*.json, raw/pre-reply-audit/에 별도로 보존했다.

(4) PR 본문의 직접 근거로 사용한 rg 출력은 454, 471, 476행이다. 요청한 rg -n "complete message|빈 queue|비어 있는 queue" core/doc/spec/core/systems/06-auto-hwm.ko.md의 전체 출력은 다음과 같으며 raw/spec-rg.txt에도 보존했다.

178:multipart 예약, 빈 queue oversize 예외, dequeue credit의 **관찰 가능한 동작**은
280:  uint64_t oversize_admission_count;            // 빈 queue oversize 예외 수락 수
442:    Note over Q: HWM이 0이 아니고 미반환+candidate가 HWM 초과면 거부<br/>(빈 queue oversize 예외는 별도)
444:    Q-->>D: complete message dequeue 시 committed 감소, writer에 credit 반환
454:   frame을 받아들이지 않는다. 공개 스펙의 빈 queue oversize 예외는 별도로 적용한다.
457:6. Queue가 complete message를 dequeue하면 그 frame들의 committed 값을 줄이고 writer에
471:전에 멈춘다. 다만 비어 있는 queue에서 시작한 multipart는 마지막 frame이 HWM을 넘더라도
476:비어 있는 queue에는 전체 charge를 admission 시점에 아는 complete message 한 건을 HWM보다 크더라도 받아들일 수 있다. 이 예외는 두 message에 동시에 적용하지 않으며 `ZLINK_OPT_MAXMSGSIZE` 검사를 건너뛰지 않는다.
480:Core HWM charge의 종료 경계는 complete message를 queue에서 dequeue해 binding에 넘기는
635:- complete message를 recv하면 Core queue charge가 종료되고 sender가 다시 보낼 수 있다. Application이 payload를 계속 보유해도 snapshot의 `application_accounted_bytes`는 0이다.
