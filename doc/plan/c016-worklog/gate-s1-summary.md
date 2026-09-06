# gate s1 (S-1, activate_read 왕복 축소)

## 0. 사전 점검
- 브리프 수정 반영: 청결도 확인 범위 `git status --short -- core bindings scripts`만 대상(전체 워킹트리에는
  `doc/plan/c016-worklog/progress-S-3.md`(수정)·`progress-S-1.md`(신규, untracked) 잔여 — worklog 문서로 코드 아님, 감독관 확인상 정상).
  범위 확인 결과 비어 있음 → 진행.
- `git pull --rebase -q` 수행 중 위 두 worklog 파일이 rebase를 막아 `git stash push -u`로 잠시 치우고 pull 후 즉시 복원.
  main HEAD: `bc1519e105` → `974b2231b7`(docs(plan): 게이트 브리프 — 깨끗함 검사는 core/bindings/scripts만; S-2/S-4/S-9/S-10 포함).

## 1. 패치 적용
- worktree `~/project/zlink-work/s1`(HEAD `45eaddeb32`)에서 `git diff` → 2파일, +99/-82.
- `git apply --3way` **충돌 없이 clean 적용**(pipe.cpp, socket_base_api.cpp 모두 "Applied patch ... cleanly").
- `git diff --stat -- core/include core/src/libzlink.vers` 비어 있음 확인(공개 인터페이스 불변).

## 2. 빌드
- `JOBS=6 scripts/build-core.sh dev` — 성공.
- `JOBS=6 scripts/build-core.sh release --lib-only` — 성공(`libzlink.so.0.17.0`).
- `cmake --build core/build-gate --target hotpath_bench -j6` — 성공.

## 3. ctest (core/build-dev)
- 전체 1회: **207/209 통과**. 실패 2건:
  - `test_close_completion_poller_release` — 브리프에 명시된 기존 간헐(D-B147, S-12에서 수정 예정). 단독 3회 재실행: 2 pass / 1 fail(`Expected 1 Was 0` @ line 69) → 간헐 확인, job과 무관.
  - `hotpath_gate`(ctest 내장, dev=비최적화 빌드) — 5셀 모두 ratio 1.20~1.31 FAIL. dev 트리 특성상 상시 어긋나는 케이스(정성적 correctness 실패 아님). 권위 있는 신호는 §5의 valgrind 게이트.
- 대상 suite (`wake|poll|stream|pipe|mailbox|drain|progress|router|dealer`) 5회: 73개 테스트, 5/5 실행 모두 `test_close_completion_poller_release`만 산발 실패(run1, run5), 나머지 통과 — 위 간헐과 일치, 새 회귀 없음.
- lost-wake 6종(`test_wake_invariants|test_two_poller_wake|test_wake_invariant_hwm_lwm_shrink|test_wake_invariant_completion_owner|test_stream_packet_progress|test_stream_send_blocking_wakeup`) `--repeat until-fail:10`: **6/6 100% pass**.

## 4. 공개 인터페이스 확인
- `git diff --stat -- core/include core/src/libzlink.vers` 재확인 비어 있음.
- `scripts/gate/README.md`에 mirror cmp 절차 없음(내용에 header/mirror 언급 없음) → 브리프 대체 절차로 진행: `core/include`의 8개 헤더(zlink.h, zlink_enum.h, zlink_errno.h, zlink/common.h, zlink/core/api.h, zlink/eventing/api.h, zlink/message/api.h, zlink/socket/api.h) × bindings 4개 미러(c/cpp/go/rust) = 32개 `cmp` **전부 일치**.

## 5. hotpath_gate 5셀 (build-gate, valgrind, PERF_LOCK)
load avg 4.47→3.16.

| cell | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3386.459 | 0.989 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19378.512 | 0.985 | PASS |
| pair_inproc | 2527.834 | 2508.879 | 0.993 | PASS |
| router_router_tcp | 2972.532 | 2953.696 | 0.994 | PASS |
| stream_tcp | 15540.385 | 14623.471 | 0.941 | FAIL(개선 +5.9%, 브리프 규칙대로 reference 미수정, 값만 보고) |

## 6. 성능 (PERF_LOCK 하에서 순차 측정)

### 6.1 with_stream (zlink,asio, size all, ccu 1000, runs 1, reuse-build, ZLINK_CORE_SOURCE=local)
load avg 2.56→2.16.

| size | zlink (kops) | Phase 0 기준(§7.1) | ratio | asio (kops) | Phase 0 기준 | ratio |
|---|---:|---:|---:|---:|---:|---:|
| 64 B | 286.67 | 268.9 | 1.066 | 350.41 | 322.0 | 1.088 |
| 1024 B | 262.44 | 243.0 | 1.080 | 327.19 | 316.4 | 1.034 |
| 65536 B | 33.47 | 30.4 | 1.101 | 40.61 | 39.2 | 1.036 |

64 KiB 하락 없음(필수 확인 통과).

### 6.2 perf/c 1024 B tcp 경량 3셀 (§7.2 Phase 0 기준 대비)
load avg: single 2.25→2.23, multi 1.89→2.66.

| cell | Phase 0 기준 | 측정 | ratio |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 744.4 Kmsg/s | 792.38 Kmsg/s | 1.064 |
| multi ROUTER_ROUTER_SENDSEND | 111.5 Kops/s | 249.72 Kops/s | **2.240** |
| multi ROUTER_ROUTER_REQREP | 73.0 Kops/s | 186.05 Kops/s | **2.549** |

single 셀은 합리적 범위(+6.4%). multi 두 셀은 gate-s4-s10에서도 동일하게 관측된 기존 이례
(2.2~2.4배)와 같은 패턴 — S-1(activate_read 경로 축소)의 변경 규모 대비 과도해 보이며 Phase 0
측정 조건 차이 또는 머신 부하 차이일 가능성. 코드 판단은 브리프 범위 밖이라 수치만 보고.

## 7. 결론
- 패치 충돌 없음, 공개 인터페이스 불변, dev/release/gate 빌드 전부 성공.
- ctest 전체·5회 반복·until-fail:10 반복 모두 D-B147 간헐 외 회귀 없음.
- 결정론적 hotpath 5셀: 4 PASS, stream_tcp는 개선 초과로 FAIL(수치 보고만, reference 미변경).
- 성능: with_stream 전 구간 기준 이상, perf/c single +6.4%, multi 두 셀은 기존에도 관측된 이례적 배율.
- 메인 워킹트리는 패치 적용 상태 그대로 유지, 커밋하지 않음.
