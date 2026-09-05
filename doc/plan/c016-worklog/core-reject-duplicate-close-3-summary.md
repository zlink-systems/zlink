# Core D-090 계약 검증 — round 3

2026-09-05. 승인된 다섯 계약 기대값을 D-090에 맞췄다. D-088의 **inproc 단일 fixture
20/20**, 실제 거부 분기를 거친 **TCP 20/20**, 기존 immediate reconnect 본문의
**inproc·TCP 각각 20/20**을 확인했다. 전체 CTest 원시 결과는 **170/171 PASS**이며
유일한 실패는 dev tree에서 n/a로 분류하는 `hotpath_gate`다.

**요청한 모든 gate가 green인 상태는 아니다.** 별도로 실행한
`test_close_completion_poller_release`는 현재 worktree에 없는 D-089 수정에 의존하며
close 정지로 실패했다. 아래 BLOCKERS에 재현과 소유 위치를 기록했다.

작업 위치는 `/home/hep7/project/zlink-core-a`, detached HEAD는
`fadfac1c4e02c33d1a8eb19f39457e14e5a61b7d`다. Round-2에서 물려받은 tracked diff
**17/17개를 byte 동일하게 보존**했다. 이번 round의 source 변경은 아래 assertion 파일과
기존 `test_router_reject_duplicate.cpp` 확장뿐이다. Runtime을 추가로 수정하지 않았다.
Commit·branch 변경은 하지 않았으며 diff와 이 보고서는 worktree에 미커밋 상태로 남겼다.

D-090과 개정 completion 표, round-1/2 보고서는 이 worktree에 없거나 이전 내용이므로
`/home/hep7/project/zlink/`의 해당 문서만 읽었다. Spec, `bindings/`, `framework/`,
main의 `core/build-dev`와 `zlink-core-b`에는 쓰지 않았다.

## Assertion 변경과 spec 근거

아래 source 위치는 `zlink-core-a`의 최종 line이다. 계약 근거는 main의
`core/doc/spec/core/socket/README.ko.md` §6 completion 표와 감독 결정 D-090이다.

Pending REQUEST는 submit 시점 pair가 종료되면 원인 불문 즉시
`NOT_CONNECTED(109)`/`EHOSTUNREACH`로 정확히 한 번 종결된다. 기존 소유 helper도
`core/src/api/socket/socket_request_reply_dispatch.cpp:462-463`에서
`from_errno(EHOSTUNREACH)`를 사용한다. Public REQUEST completion에는 별도 errno field가
없으므로 테스트는 `request_result`를 검사한다. `TIMED_OUT`은 admit된 request의 pair가
계속 유지되지만 reply가 오지 않는 경우에만 해당한다.

| 파일:line | 승인된 변경 | Spec 조항 |
|---|---|---|
| `core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:509` | 거부된 pipe의 REQUEST 기대값을 `TIMED_OUT(101)`에서 `NOT_CONNECTED(109)`로 변경 | §6의 submit 시점 transport pair 종료 행: REJECT로 닫혀도 원인 불문 즉시 한 번 종결 |
| `core/tests/integration/test_phase3_request_reply_contract.cpp:1050`, `:1085`, `:1089` | 원래 `:1086/:1102`의 detach 후 무응답·timeout 기대를 제거하고, reconnect 전에 payload 없는 `NOT_CONNECTED`를 소비. 이름을 `test_admitted_request_completes_not_connected_on_physical_detach_without_replay_after_same_rid_reconnect`로 변경. Reconnect 뒤 DATA marker를 통한 replay 부재와 추가 completion 부재 검사는 유지 | §4 submit 시점 pair pinning; §6 pair 종료 행. Reply를 받을 수 없는 pending은 원래 reply timeout을 기다리지 않음 |
| `core/tests/integration/test_phase3_request_reply_contract.cpp:2815` | 원래 `:2818`의 responder physical close 후 pinned pending 기대값을 `NOT_FOUND(102)`에서 `NOT_CONNECTED(109)`로 변경 | §6 pair 종료 행과 명시적 endpoint/logical RID 제거 행의 구분 |
| `core/tests/integration/test_zmp_metadata.cpp:2066` | Invalid completion kind로 pipe가 닫힐 때 `TIMED_OUT` 대신 `NOT_CONNECTED`를 기대 | §6 pair 종료 행: 종료 원인과 무관하게 pending을 종결 |
| `core/tests/integration/test_dealer_router_single_lane_contract.cpp:3664` | 이전 pair의 REQUEST completion을 기존 `assert_request_completion()`으로 한 번 소비하여 ID·`NOT_CONNECTED`·payload count 0·NULL을 검사. 기존 100ms 무응답 검사와 새 pair의 정상 completion 검사는 유지 | §6 pair 종료 행의 exactly-once 종결 및 payload 없는 completion 표현 |

명시적 endpoint/logical RID 제거의 REQUEST 계약은 `NOT_FOUND(102)`다. 기존
`test_router_explicit_logical_rid_removal_invalidates_reply_token`도 수정하지 않았으며,
그 테스트의 reply 제출 기대값인 `ZLINK_SUBMIT_NOT_FOUND`/`ENOENT`는 통과했다.
Pair가 유지되지만 reply가 오지 않는 기존 `TIMED_OUT` 검사도 그대로 통과했다.
지정된 assertion 파일에서 이 표 밖의 fixture·timeout·budget·검사 조건은 변경하지 않았다.

- 소유 계층: REQUEST terminal과 WRITABLE token은 Core socket, RID admission은 Core ROUTER, 종료·재연결은 Core pipe/session이 소유한다.
- Spec 조항: socket README §4 REJECT·pair pinning, §6 completion 표의 pair 종료·명시적 제거·reply timeout 행; D-090.
- 교차언어 대조: 동일 candidate Core로 C++와 Python의 기존 completion 계약·sample을 검증했다. Binding·Framework runtime 변경은 없다.
- 변경 분류: **A 계약 적응**. 이번 round는 승인된 assertion 정렬과 공개 API fixture 확장이다.

**수정 전/후 규칙 수: runtime의 pair 종료 1규칙 → 1규칙. Round-2의 원인별 3규칙 →
pair 종료 1규칙 수정을 보존하고, 그 규칙과 충돌하던 테스트 기대값을 정렬했다.**

## Inproc 단일 fixture

`core/tests/integration/test_router_reject_duplicate.cpp:401`의 기존 거부 pending fixture를
확장했다. `IO_THREADS=0`과 하나의 public poller로 duplicate admission보다 REQUEST
submit이 먼저 일어나도록 한다. Initial request/reply가 끝난 기존 peer의 receive flow를
PAUSED로 설정하고, 서버가 그 RID에 제출한 DONTWAIT SEND의 WRITABLE 대기 토큰을 확보한다.

그 뒤 다음 순서를 한 fixture에서 모두 검사한다.

1. Duplicate pipe의 pending REQUEST가 100ms 이내 `NOT_CONNECTED`로 한 번 완료된다.
2. 기존 peer가 endpoint를 disconnect하고 close한다.
3. 서버의 기존 RID 대기 토큰이 `WRITABLE`/`SEND_ADMITTED`로 한 번 회복된다. ID·RID·user context·errno 0을 검사한다.
4. 자동으로 다시 연결한 duplicate에서 새 REQUEST를 한 번 제출하여 정상 reply와 `REQUEST_OK`를 받는다.
5. 원래 1000ms request timeout을 넘겨도 추가 completion이 없다.

Connector의 READY/WRITABLE을 관측하는 대안은 remote ROUTER의 admission을 증명하지
못한다. 선택한 서버 RID 토큰은 기존 peer가 PAUSED인 동안 회복될 수 없고, REJECT는
기존 pipe가 종료되기 전 새 RID owner를 수용하지 않는다. 따라서 이 토큰의 회복과
후속 REQUEST 성공으로 필요한 경계를 확인한다. 별도 poller·수동 connect 반복·내부
symbol·failpoint·sleep을 추가하지 않았다. 서버의 logical RID를 명시적으로 제거하지도 않는다.

## 검증 결과

로그 공통 위치는 `/home/hep7/project/zlink-core-a/core/build-dev/reject-3-validation/`이다.
Core 빌드는 `ulimit -v 16777216` 아래 `nice -n 10 env JOBS=4 scripts/build-core.sh dev`로
성공했다. 이후 최종 fixture target도 다시 빌드했다. `RelWithDebInfo`, LTO OFF다.

| 검사 | 결과 | 로그 |
|---|---|---|
| 수정한 계약 관련 CTest | **4/4 PASS** | `assertion-tests.log` |
| `test_router_reject_duplicate` ×20 | **20/20 실행, 140/140 case PASS** | `contract-20-final.log`, `contract-final-01.log`부터 `-20.log` |
| Inproc 단일 fixture | **20/20 PASS**, 매회 거부 pending → old 종료 → 다음 admission → REQUEST OK; completion 대기 0ms | 위 반복 로그의 `rejected_pending transport=inproc` 20개 |
| TCP reconnect counterpart | **20/20 PASS**, 매회 실제 거부 분기와 후속 REQUEST OK; completion 대기 0ms | 위 반복 로그의 `rejected_request transport=tcp` 20개 |
| 원래 immediate reconnect 본문 | **inproc 20/20, TCP 20/20 PASS** | `immediate-inproc-20.log`, `immediate-tcp-20.log` |
| 지정 회귀 CTest | **26/26 PASS**: reciprocal handover 1, disconnect progress 24, fixed-RID context term 1 | `targeted-regression.log` |
| 전체 `ctest --test-dir core/build-dev --output-on-failure -j2` | **170/171 PASS**, 222.67초. 기능 170/170, dev hotpath 1 FAIL/n/a | `full-ctest.log` |
| `test_router_reciprocal_handover_lanes` | **PASS**, 전체 gate에서 26.14초 | `full-ctest.log` |
| `test_disconnect_progress_*` | **24/24 PASS**, 전체 gate에도 포함 | `full-ctest.log` |
| `test_ctx_term_fixed_rid_handover` | **PASS**, 전체 gate에서 0.78초 | `full-ctest.log` |
| `test_close_completion_poller_release` | **FAIL**, monitor case iteration 0에서 close 정지, exit 134 | `close-regression.log`; 아래 BLOCKERS |
| §5 `^test_single_lane_ -j2` ×2 | **29/29 PASS / 29/29 PASS**, 20.51초 / 20.29초 | `single-lane-1.log`, `single-lane-2.log` |
| §5 raw header mirror | **12/12 동일**: c/cpp/go/rust × enum/socket/eventing | `static-checks.log` |
| §5 `git diff --check` | **PASS** | `static-checks.log` |
| C++ binding | **contract 16/16 + sample 7/7 PASS** | `cpp-build.log`, `cpp-contract.log`, `cpp-samples.log` |
| Python binding | **190 tests + 4 subtests + sample 7/7 PASS** | `python-gate.log` |

원래 immediate 테스트는 수정된 source의 본문을 byte 동일하게 유지하고, 검증 디렉터리의
복사본에서 `main`만 transport 선택으로 제한하여 최종 archive에 링크했다. 원본 파일의
`main`은 변경하지 않았다. 기존 출력 문자열 `reply=timed-out`도 그대로지만, 실제 판정은
승인된 `NOT_CONNECTED` assertion이다.

C++는 round 1의 out-of-source `core/build-dev/cpp-gate`를 빌드하고 `contract`와
`sample-smoke` label을 실행했다. Python은 round 1의
`core/build-dev/reject-python-gate-repo`를 사용했으며 tracked source **148/148개가
원본과 byte 동일**함을 확인했다. `ZLINK_CORE_SOURCE=local`, 명시적 `ZLINK_LIBRARY_PATH`,
기존 `gate-venv/bin/python`, `PYTHONDONTWRITEBYTECODE=1`, `run_tests.sh -p no:cacheprovider`로
실행했다. 두 binding의 runtime은 현재 worktree의 `core/build-dev/lib/libzlink.so.0.17.0`이며
SHA-256은 `06e6ea999ef7cee0c139b0d91cd41abda38b62aa8d6b04395fb32c4bb626fd20`이다.
상세 증거는 `runtime-provenance.log`에 있다.

전체 CTest는 한 번 실행했다. Hotpath 원시 측정은 instructions/message 기준
DD 4476.0703, PAIR 3531.2453, RR TCP 3855.96075이며 DR REQUEST는
15140.0036 instructions/request다. 저장 reference 대비 4/4 FAIL이다. Round-2와 같은
dev/reference 차이이며, 요청대로 dev에서는 **n/a**로 분류한다. Release green으로 세지
않았고 reference도 변경하지 않았다.

## BLOCKERS

**D-089 close/completion-owner 수정이 현재 기준 HEAD에 없다.** 이 의존성 때문에 요청한
`test_close_completion_poller_release` green 조건이 남아 있다.

이 테스트는 현재 worktree의 CTest 목록에 없어서 main의 공개 C API 테스트 원본을
`core/build-dev/reject-3-validation/test_close_completion_poller_release.cpp`에 byte 동일하게
복사하고 현재 `libzlink.a`에 링크했다. 첫 `with_monitor` case의 iteration 0에서
`close stuck after completion poller release`가 발생하여 exit 134로 종료했다.
그 뒤 `without_monitor` case는 실행되지 않았다. Timeout·assertion을 변경하지 않았다.

원인 경로는 `socket_base_dispatch.cpp:256-289`의 마지막 completion poller 해제 →
`socket_base_api.cpp:926-950`의 resume → `socket_base_dispatch.cpp:119-200`의 owner 획득이다.
현재 `:124-125`에는 close와 owner 시작을 직렬화하는 기존 public admission scope가 없어,
close 승인 뒤 executor를 다시 시작할 수 있다. 이는 main의 D-089 결정과
`core-close-completion-release-restart-summary.md`에 기록된 **Core B 결함**과 일치한다.

Close admission과 completion owner 획득은 Core lifecycle이 소유하며, 근거는 socket README
§6 `zlink_close`와 polling spec §3/§5다. 별도 D-089 작업이 소유한 수정을 이 round에서
중복 구현하지 않았다. 감독이 그 수정과 본 diff를 통합한 뒤 이 회귀의 green을 확인해야 한다.
Inproc 단일 fixture나 D-090의 다섯 assertion에는 남은 실패가 없다.
