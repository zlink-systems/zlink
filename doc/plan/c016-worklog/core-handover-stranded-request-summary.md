# Core HANDOVER pending REQUEST 종료 결과

HANDOVER로 교체된 pair의 pending REQUEST를 기존 completion queue로
`ZLINK_REQUEST_NOT_CONNECTED`(109) 한 번만 게시하도록 수정했다.
Reciprocal 및 같은 방향 takeover 모두 2초 request timeout을 기다리지 않는다.
계약 테스트 8개 × 5회가 통과했으며 completion 관측은 10–21ms였다.

- 작업 위치: `/home/hep7/project/zlink-core-a`, detached `bb730c654f`.
- 미커밋 diff: Core 소스 5개와 통합 테스트 1개, 281 insertions / 77 deletions.
- main tree에는 이 요약만 작성했다. main의 `core/build-dev`, spec, bindings,
  framework 소스는 변경하지 않았다. Commit·branch 전환은 수행하지 않았다.
- 로그: `/home/hep7/project/zlink-core-a/core/build-dev/handover-validation/`.

## 소유권과 계약

- 소유 계층: Core ROUTER admission이 pair 교체를 결정하고, Core request/reply
  상태 머신이 pending 제거와 terminal completion을 소유한다.
- Spec 조항: `core/doc/spec/core/socket/README.ko.md:155` §4 RID duplicate policy,
  특히 `:165`의 “handover로 물러나는 즉시 … 정확히 한 번 종결” 및 자기 timeout을
  기다리지 않는 계약(`bb730c654f`).
- 교차언어 대조: Framework runtime 수정은 없다. 공통 Core 후보 라이브러리로
  C++ 계약·sample과 Python 테스트·sample을 통과했다. 언어별 handover 보상 상태는 추가하지 않았다.
- 변경 분류: **A — 개정된 HANDOVER terminal 계약에 대한 Core 적응**.

`03-errors.ko.md:368`은 `ENOTCONN`·`EHOSTUNREACH`를 REQUEST 결과 109로 정규화하고,
`:466`은 REQUEST completion channel이 typed result를 전달한다고 정한다.
구현은 기존 `request_result_internal::from_errno(EHOSTUNREACH)`를 사용한다.
공개 completion에는 REQUEST errno 필드가 없으므로 별도 필드나 API를 추가하지 않았다.

## 원인과 변경 위치

기준 commit의 `router_admission.cpp:302`는 reciprocal 방향을 RID로 결정하고,
`:398`에서 기존 route를 demote한다. `:408`의 standby 보관과 `:439`의 후처리에는
그 pair의 REQUEST를 종료하는 연결이 없었다. REQUEST는
`socket_request_reply_submit_api.cpp:121`에서 submit-time pair/generation을 저장하고,
`socket_request_reply_pending_api.cpp:108`의 reply fence를 유지하므로 교체 route의
reply가 pending을 소비하지 못한다. 결국 `socket_request_reply_internal.cpp:582`의
기존 timeout terminal까지 남았다. 수정 전 공개 C API 재현은
`Expected 109 Was 101`로 실패했다(`repro-before.log`).

아래 line은 수정 후 worktree 기준이며, 경로는 worktree에 상대적이다.

| 파일:line | 변경 |
|---|---|
| `core/src/runtime/sockets/router/router.hpp:116` | 기존 route-adoption action에 교체된 pair ID와 generation을 전달하는 두 값을 추가 |
| `core/src/runtime/sockets/router/router_admission.cpp:403`, `:451` | 기존 current route가 교체되는 공통 분기에서 pair를 기록하고, route·transport lock 해제 후 request/reply 종료 호출 |
| `core/src/api/socket/socket_request_reply_pending_api.cpp:208` | 기존 pending pool에서 pair ID와 generation 모두 일치하는 record만 mutex 아래에서 제거 |
| `core/src/api/socket/socket_request_reply_dispatch.cpp:402`, `:455` | 기존 terminal 게시 helper에 result를 전달하고, correlation lease 해제 → 기존 reservation 게시 → completion wake 경로 재사용 |
| `core/src/api/socket/socket_request_reply_internal.hpp:128`, `:623`, `:656` | pair identity 역할 주석과 내부 선언 |
| `core/tests/integration/test_router_reciprocal_handover_lanes.cpp:306`, `:373`, `:564` | 공개 poller completion 관측, 200ms 상한, 같은 방향 takeover, 여러 pending·다른 peer·늦은 reply·중복 terminal 검증 |

Pending pool은 sequence index와 record의 pair 정보를 이미 보유한다. 새 pair index를
유지하는 대안과 HANDOVER 시 기존 pool을 조회하는 대안을 비교해 후자를 선택했다.
평상시 REQUEST submit에 새 index 관리 비용을 추가하지 않으며 별도 pending 상태도 없다.
Reply fence, reply 전송 경로, standby 보관·승격 조건은 변경하지 않았다.
Pending 제거는 timeout/reply와 같은 mutex 및 기존 remove 경로를 사용하므로 terminal
소유권을 한 번만 얻는다. ROUTER command와 submit의 기존 public API 동기화도 유지한다.

## 검증

빌드는 지정 명령 `nice -n 10 env JOBS=4 scripts/build-core.sh dev`로 수행했다.
Core와 C++ CTest는 모두 `-j2`, C++·Python native 빌드는 `-j2`를 사용했다.
전체 Core gate는 최종 후보에서 한 번 실행했다.

| 검증 | 결과 |
|---|---|
| `test_router_reciprocal_handover_lanes` 최초 최종 fixture 검증 | 8/8 통과 |
| 위 테스트 `--repeat until-fail:5` | 40/40 case 통과, 106.99초 |
| `test_ctx_term_fixed_rid_handover` (D-B94) | TCP/inproc × reconnect 10/100/1000ms × 20회 = 120/120 admission; TCP p95 6ms, inproc p95 0ms |
| `test_router_handover` | 4/4 case 통과 |
| `ctest --test-dir core/build-dev -j2 -V` | **143/144 통과**, 202.76초; 실패는 `hotpath_gate` 한 개 |
| `test_wake_invariants` | 전체 gate에서 통과 |
| `^test_single_lane_` 추가 2회 | 29항목, until-fail 실행 57회 중 56 pass / 1 fail; 아래 알려진 간헐 실패 참고 |
| `test_single_lane_flow_snapshot_accounting` 단독 재실행 1회 | 통과(0.01초) |
| 공개 C header mirror | c/cpp/go/rust × 3 headers = 12/12 일치 |
| `git diff --check` | 통과 |
| C++ contract / sample-smoke | 16/16, 7/7 통과 |
| Python tests / samples | 180/180 및 subtest 4/4, sample 7/7 통과 |

반복한 계약 case 이름:

- `test_reciprocal_handover_tcp_10ms`, `test_reciprocal_handover_tcp_100ms`,
  `test_reciprocal_handover_tcp_1000ms`: 15/15; completion 10–20ms.
- `test_reciprocal_handover_inproc_10ms`, `test_reciprocal_handover_inproc_100ms`,
  `test_reciprocal_handover_inproc_1000ms`: 15/15; completion 10–11ms.
- `test_same_direction_takeover_tcp`: 5/5; completion 10–21ms.
- `test_same_direction_takeover_inproc`: 5/5; completion 10–21ms.

Same-direction case는 교체 pair의 pending 3개가 각각 한 번 종료되고 다른 RID의
pending은 성공하는지 확인한다. 두 시나리오 모두 승자 방향 재전송 성공과 늦은 reply를
확인하며, 원래 2초 timeout보다 긴 관측을 통해 중복 terminal 부재를 검증한다.
Reciprocal의 standby 관측 구간에는 양쪽 DISCONNECTED/CLOSED가 0회다.

Single-lane 추가 실행의 실패 위치는
`test_dealer_router_single_lane_contract.cpp:3102`의
`test_sl_flow_snapshot_accounts_dr_reply_as_application`이다. 이는
CONTRIBUTING §4가 단독 재실행 1회로 판정하도록 명시한 알려진 load flake다.
최종 전체 gate에서는 통과했고, 추가 실행 실패 후 단독 재실행도 통과했다.
전체 suite 재시도나 assertion 변경은 하지 않았다.

C++는 `tests/run_tests.sh`와 같은 CMake 옵션·label을 사용하되, 원본 script의
`bindings/cpp/build` 생성 및 sample script의 무제한 `nproc` 빌드를 피하기 위해
`core/build-dev/cpp-gate`에서 configure/build/CTest를 실행했다.
Python은 원본 script 실행 시 native extension이 없어 수집이 중단되어, 소스 147개를
바이트 동일하게 복사한 `core/build-dev/python-gate-repo/bindings/python`에 native
extension을 빌드하고 복사본의 동일한 `tests/run_tests.sh -p no:cacheprovider`를 실행했다.
두 언어 모두 `/home/hep7/project/zlink-core-a/core/build-dev/lib/libzlink.so`를 사용했다.

## Hotpath 수치

`scripts/`에는 별도 hotpath policy 검사기가 없으며, 등록된
`core/tests/perf/hotpath_gate.py`를 사용했다. 변경 전에는 `ctest -R '^hotpath_gate$'`,
변경 후에는 최종 전체 CTest에 포함해 실행했다. 둘 다 dev/RelWithDebInfo, LTO OFF다.
각 값의 단위는 instructions/message이며 reqrep cell은 instructions/request다.

| cell | 반복 수 | 저장 기준 | 변경 전 | 변경 후 | 변경 전 대비 |
|---|---:|---:|---:|---:|---:|
| dealer_dealer_inproc | 20,000 | 3455.3810 | 4476.097050 | 4476.086350 | -0.000239% |
| dealer_router_reqrep_inproc | 5,000 | 12054.8948 | 15149.815800 | 15161.585200 | +0.077687% |
| pair_inproc | 20,000 | 2681.9566 | 3531.229600 | 3531.219200 | -0.000295% |
| router_router_tcp | 20,000 | 2972.8817 | 3855.861450 | 3858.098200 | +0.058009% |

4개 cell 모두 저장 기준의 ±5% 범위를 변경 전부터 초과했다. 변경 후 기준 대비 비율은
각각 1.2954 / 1.2577 / 1.3167 / 1.2978로 모두 FAIL이다. 기준값이나 gate는 변경하지
않았으며 비용 튜닝도 하지 않았다. Hotpath 진입점은 spec `10-hot-path.ko.md` §2의
I/O command → `xread_activated` → route-adoption 후처리 경계다. 새 pending 조회는
실제 route 교체 action이 있을 때만 수행한다.

## BLOCKERS

- **전체 gate green 조건 미충족: `hotpath_gate` 실패.** 동일한 미수정 Core로도
  재현된 dev 기준 초과다. 기준 생성 기록(`hotpath-gate-summary.md`)은 Release이며,
  이번 job은 지시된 dev/no-LTO 빌드다. Release/LTO gate와 release throughput 비교는
  실행하지 않았고, 저장 기준에 대한 성능 판정은 별도로 남는다.
- 기능 계약의 미해결 실패는 없다. Single-lane 추가 실행의 알려진 간헐 실패와
  단독 재실행 통과는 위에 원시 결과대로 기록했다.

Supervisor는 위 6개 파일의 worktree diff를 검토해 main에 적용할 수 있다.
