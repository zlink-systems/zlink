# Core D-090 — D-089 포함 main 기준 검증

2026-09-05. **지정 회귀 58/58 PASS, REJECT·close 각각 5/5 반복 PASS,
전체 기능 테스트 171/171 PASS**다. 전체 CTest 원시 결과는 **171/172 PASS**이며,
유일한 실패는 요청에서 dev tree의 n/a로 지정한 `hotpath_gate`다.
이번 검증에서 D-089 admission scope와 D-090 termination의 상호작용 결함은
관측되지 않았다. 추가 코드 수정은 없다.

## 검증 대상과 보존 범위

- 작업 트리: `/home/hep7/project/zlink-core-b`, detached HEAD
  `4dbba45d63f63c102c3b0cb81558876fd72b6d6a`.
- D-089 `cb62cb89f8`이 HEAD의 ancestor임을 확인했다. 해당 수정은
  `core/src/runtime/sockets/common/socket_base_dispatch.cpp:127`의 기존 public
  admission scope로 completion owner 획득과 close 승인을 직렬화한다.
- 입력 D-090: REJECT close, REQUEST pair 종료 단일 규칙, inproc overlap 수정,
  승인된 다섯 assertion 정렬. Tracked 변경 21개 파일과 untracked
  `test_router_reject_duplicate.cpp`를 합친 **22/22개 파일이 시작 시점과 byte 동일**하다.
  Tracked diff와 `git status --short`도 시작 시점과 동일하다.
- 추가 fix/diff: **없음**. Assertion·Core policy 변경, commit·branch 변경은 없다.
  기존 `diag-node-old-core/`도 그대로 보존했다.
- Main 트리에는 이 보고서만 작성했다. Main의 `core/build-dev`, spec,
  `bindings/`, `framework/`에는 쓰지 않았다.

## 단계별 결과

로그 위치: `/home/hep7/project/zlink-core-b/core/build-dev/d090-verify-on-main/`.

| 단계 | 결과 | 로그 |
|---|---|---|
| Core dev 빌드 | 1/1 PASS, exit 0; `RelWithDebInfo`, LTO OFF, JOBS=4 | `build.log` |
| 지정 회귀 CTest | 58/58 PASS, 64.39초, exit 0 | `targeted.log` |
| `test_router_reject_duplicate` until-fail:5 | 5/5 PASS | `repeat-5.log` |
| `test_close_completion_poller_release` until-fail:5 | 5/5 PASS | `repeat-5.log` |
| 반복 단계 합계 | 10/10 실행 PASS, 33.25초, exit 0 | `repeat-5.log` |
| 전체 CTest 1회 | 171/172 PASS, 228.61초, exit 8; 기능 171/171 PASS, hotpath 1 FAIL/n/a | `full-ctest.log` |
| Single-lane 포함 실행 | 지정 회귀 29/29 PASS, 전체 gate 29/29 PASS | `targeted.log`, `full-ctest.log` |
| Raw header mirror | c/cpp/go/rust × enum/socket/eventing 12/12 동일 | `static-provenance.log` |
| `git diff --check` 및 입력 보존 | PASS; 소스 22/22 동일, 추가 diff 0 | `static-provenance.log`, `initial-source-sha256.json`, `initial.patch` |

실행 명령은 모두 작업 트리 루트에서 실행했다. 빌드에는 §5의
`ulimit -v 16777216`을 적용했다.

```bash
nice -n 10 env JOBS=4 scripts/build-core.sh dev
ctest --test-dir core/build-dev -j2 -R 'test_router_reject_duplicate|test_close_completion_poller_release|test_disconnect_progress|test_router_reciprocal_handover_lanes|test_ctx_term_fixed_rid_handover|test_socket_disconnect_progress_without_app_poll|^test_single_lane_' --output-on-failure
ctest --test-dir core/build-dev -j2 -R '^(test_router_reject_duplicate|test_close_completion_poller_release)$' --repeat until-fail:5 --output-on-failure
ctest --test-dir core/build-dev -j2 --output-on-failure
```

Close 회귀는 지정 단계·반복·전체 gate에서 총 7/7 실행 PASS다. REJECT 회귀도
총 7/7 실행 PASS다. `ZLINK_TEST_CASE`는 unset으로, 선택 필터 없이 실행했다.
CTest가 등록된 serial 속성을 지키도록 두었으며 요청한 병렬도는 `-j2`다.
이번 작업은 요청한 Core dev 검증 범위이며 C++·Python binding gate와 release
성능 비교는 실행하지 않았다.

검증 runtime은 작업 트리의 `core/build-dev/lib/libzlink.so.0.17.0`이며 SHA-256은
`e8f388e42ceec39f035b25c73964da5964de3691a4609c057e9fe01c5827bd77`이다.

## BLOCKERS

**요청한 Core dev 기능 검증의 BLOCKERS는 없다.** Round 3에서 남았던
`test_close_completion_poller_release` 실패는 D-089 포함 기준에서 통과했다.

`hotpath_gate`는 원시 CTest 실패로 남는다. Reference 대비 측정 비율은
dealer/dealer inproc 1.2954, dealer/router REQUEST inproc 1.2558,
PAIR inproc 1.3167, router/router TCP 1.2970으로 4/4 FAIL이다.
요청에 따라 dev n/a로 분류하며 release 성능 green으로 세지 않는다.
Reference 변경이나 전체 gate 재실행은 하지 않았다.

수정 전/후 규칙 수: 입력 D-090의 REQUEST pair 종료 1규칙 → 1규칙.
이번 검증의 추가 runtime 규칙·상태·예외 경로는 없다.
