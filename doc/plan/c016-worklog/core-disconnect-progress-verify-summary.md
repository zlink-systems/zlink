# Reduced disconnect-progress 검증 결과

감독자가 main의 D-B102 위에 적용된 completion poller 수정의 기능 검증 결과를 판단하기 위한 기록이다.

**회귀 테스트 24개 × 5회와 지정된 subsystem 34개가 모두 통과했다. 전체 CTest는 169/170 통과했으며, 실패는 dev 빌드의 `hotpath_gate` 1건이다. 테스트 기대값 조정은 없고 추가 Core 수정도 없다.**

## 검증 대상

- 실행 tree: `/home/hep7/project/zlink-core-b`, detached HEAD `4567ded51fa82a7e5c2e05d293d77129a186bd17`.
- D-B102 커밋 `1c69086a4a`가 HEAD의 조상임을 확인했다.
- 적용된 runtime 수정: `core/src/runtime/sockets/common/socket_base_dispatch.cpp`, `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`. Completion poller 획득 시 monitor의 command lease를 유지한다.
- 테스트: `core/tests/integration/test_disconnect_progress.cpp`, 등록: `core/tests/CMakeLists.txt`.
- 빌드: `RelWithDebInfo`, `ENABLE_LTO=OFF`. 실행 로그는 위 worktree의 `core/build-dev/disconnect-progress-verify-logs/`에 보존했다.

## 결과 표

| 검증 명령 | 결과 | 로그 |
|---|---|---|
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, exit 0 | `build.log` |
| `ctest --test-dir core/build-dev -R '^test_disconnect_progress_' -j2 --repeat until-fail:5 --output-on-failure` | 24개 × 5 = 120회 PASS, 6.62 s | `progress-repeat5.log` |
| `ctest --test-dir core/build-dev -j2 -R '^test_single_lane_\|test_router_reciprocal_handover_lanes\|test_ctx_term_fixed_rid_handover\|test_wake'` | 34/34 PASS, 65.86 s | `subsystem.log` |
| `ctest --test-dir core/build-dev -j2` (1회) | 169/170 PASS, hotpath_gate FAIL, exit 8, 215.81 s | `full-gate.log`, `hotpath-failure.log` |
| `git diff --check` | PASS | 명령 exit 0 |

위 subsystem 명령의 정규식은 `^test_single_lane_`, `test_router_reciprocal_handover_lanes`, `test_ctx_term_fixed_rid_handover`, `test_wake`를 OR로 연결했다.

| Transport | Application 등록 | automatic idle/blocked | disconnect idle/blocked | request idle/blocked |
|---|---|---|---|---|
| tcp | POLLIN | 각 5/5 | 각 5/5 | 각 5/5 |
| tcp | POLLIN + POLLCOMPLETION | 각 5/5 | 각 5/5 | 각 5/5 |
| inproc | POLLIN | 각 5/5 | 각 5/5 | 각 5/5 |
| inproc | POLLIN + POLLCOMPLETION | 각 5/5 | 각 5/5 | 각 5/5 |

`idle`은 application poller에 등록만 하고 기다리지 않는 경우다. `blocked`는 application poller의 한 번의 wait 안에 머무는 경우다. 자동 reconnect의 새 READY, explicit disconnect의 old connection terminal, 즉시 reconnect 뒤 REQUEST/reply를 기존 assertion 그대로 검증했다.

## 테스트 기대값과 계약

**변경 없음.** `core/doc/spec/core/06-monitoring.ko.md` §3.1과 `decisions.ko.md` D-B102를 대조했다. 기존 테스트는 같은 old connection ID의 DISCONNECTED 또는 CLOSED를 terminal로 인정하므로, inproc에서 CLOSED를 반드시 요구하지 않는다. 새 READY는 old connection과 다른 ID여야 한다. 이 기대값은 B의 shared inproc identity·reconnect 구현과 충돌하지 않았으며 120회 모두 통과했다. Timeout, retry, assertion, fixture 조건을 변경하지 않았다.

- **소유 계층:** Core socket command 처리와 completion drain 소유권.
- **Spec 조항:** `core/doc/spec/core/05-polling.ko.md` §3·§4, `core/doc/spec/core/06-monitoring.ko.md` §3.1·§4, socket 계약의 disconnect/reconnect. 기존 수정 보고서의 계약 근거를 유지한다.
- **교차언어 대조:** 이번 작업은 public C API를 통한 Core 재검증이며 Framework runtime 변경 및 교차언어 재실행은 없다. 이전 보고서의 binding 검증을 이번 실행 결과로 세지 않는다.
- **변경 분류:** B — 기존 Core 결함 수정의 축소 패치 검증. 이번 작업의 추가 runtime 변경 없음.

## Hotpath와 BLOCKERS

Dev는 LTO가 꺼져 있어 release 성능 판정에는 N/A다. 다만 실제 전체 CTest invocation의 실패는 그대로 기록한다.

| Hotpath cell | Reference instructions/message | Dev 측정 | 비율 | 실제 결과 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3455.381 | 4476.086 | 1.2954 | FAIL |
| dealer_router_reqrep_inproc | 12054.895 | 15149.597 | 1.2567 | FAIL |
| pair_inproc | 2681.957 | 3531.347 | 1.3167 | FAIL |
| router_router_tcp | 2972.882 | 3855.948 | 1.2970 | FAIL |

**BLOCKERS: 기능 검증 실패 없음. 전체 dev gate는 hotpath 1건 때문에 green이 아니다.** 이번 축소 패치의 release/LTO hotpath는 실행하지 않았으므로 성능 gate 통과를 주장하지 않는다. Dev 측정은 이전 보고서의 dev 측정과 같은 수준이다.

Worktree의 기존 4개 파일 diff를 uncommitted 상태로 유지했다. 이번 작업에서 작성한 tracked-source 범위의 파일은 main tree의 이 보고서뿐이다. Spec·bindings·framework 및 main tree의 `core/build-dev`는 수정하지 않았다. Commit·push는 수행하지 않았다.
