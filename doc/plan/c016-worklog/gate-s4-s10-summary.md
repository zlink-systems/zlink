# gate s4-s10 — S-4(asio write-turn policy), S-10(TLS initial-exec) 포팅 + 채택 게이트

worktree: S-4 `~/project/zlink-work/s4`, S-10 `~/project/zlink-work/s10`. main 워킹트리는 patch 적용 상태로 남김(커밋 안 함).

## 1. patch 적용

- `git status --short` 확인: untracked progress-*.md/briefs만 있음 → 진행.
- `git pull --rebase -q` → HEAD `6f64e76b51`.
- S-4: `core/src/runtime/engine/asio/{asio_engine.cpp,asio_engine.hpp,asio_raw_engine.cpp,asio_raw_engine.hpp,asio_stream_fastpath_policy.hpp,asio_zmp_engine.cpp}`, `core/tests/unittest/unittest_asio_write_turn_policy.cpp` (7 files, +83/-57) — `git apply --3way` **충돌 없이 clean 적용**.
- S-10: `core/CMakeLists.txt` (+18) — `-ftls-model=initial-exec` 컴파일러 플래그 추가, **충돌 없이 clean 적용**.
- `git diff --stat -- core/include core/src/libzlink.vers` → 빈 결과(공개 인터페이스 불변 확인).

## 2. 빌드

- `JOBS=6 scripts/build-core.sh dev` → 성공 (`core/build-dev/lib`).

## 3. ctest (core/build-dev)

- 전체 1회: **208/209 통과**. 실패: `hotpath_gate` (dev 트리는 비최적화 빌드라 release 기준 reference와 항상 어긋남 — 브리프에 명시된 "정상" 케이스, 5셀 전부 ratio 1.10~1.31의 성능 FAIL이지 correctness 실패 아님). job이 바꾼 코드로 인한 회귀 아님, 3회 재실행 불필요로 판단.
- 합집합 suite(`stream|engine|asio|raw|zmp|recv|router|dealer|poll`, 54개) 5회: **5/5 run 모두 100% 통과(0 failed)**.

## 4. 공개 인터페이스 확인

- `git diff --stat -- core/include core/src/libzlink.vers` 재확인: 빈 결과.
- mirror cmp(8 헤더 × 4 bindings: c/cpp/go/rust) — `core/include`의 `zlink.h, zlink_enum.h, zlink_errno.h, zlink/common.h, zlink/core/api.h, zlink/eventing/api.h, zlink/message/api.h, zlink/socket/api.h`를 각 mirror와 `cmp`: **32/32 동일**.

## 5. release lib

- `JOBS=6 scripts/build-core.sh release --lib-only` → 성공, `core/build/lib/libzlink.so.0.17.0`.

## 6. hotpath_gate (5셀, valgrind)

| cell | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3423.179 | 0.9999 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19693.710 | 1.0006 | PASS |
| pair_inproc | 2527.834 | 2527.600 | 0.9999 | PASS |
| router_router_tcp | 2972.532 | 2972.270 | 0.9999 | PASS |
| stream_tcp | 15540.385 | 15500.207 | 0.9974 | PASS |

+5% 초과 개선(FAIL-by-improvement) 없음. load avg 측정 시 5.08→4.45.

## 7. 성능

### 7.1 with_stream (zlink,asio, size all, ccu 1000, runs 1, reuse-build, ZLINK_CORE_SOURCE=local)

| size | zlink (kops) | Phase 0 기준 | ratio | asio (kops) | Phase 0 기준 | ratio |
|---|---:|---:|---:|---:|---:|---:|
| 64 B | 267.76 | 268.9 | 0.996 | 342.48 | 322.0 | 1.064 |
| 1024 B | 252.46 | 243.0 | 1.039 | 318.01 | 316.4 | 1.005 |
| 65536 B | 31.18 | 30.4 | 1.026 | 39.30 | 39.2 | 1.003 |

64 KiB 하락 없음(필수 확인 통과). load avg 2.67→2.76.

### 7.2 perf/c 1024 B tcp 경량 3셀 (§7.2 Phase 0 기준 대비)

| cell | Phase 0 기준 | 측정 | ratio |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 744.4 Kmsg/s | 792.51 Kmsg/s | 1.065 |
| multi ROUTER_ROUTER_SENDSEND | 111.5 Kops/s | 247.53 Kops/s | **2.221** |
| multi ROUTER_ROUTER_REQREP | 73.0 Kops/s | 175.84 Kops/s | **2.409** |

single 셀은 합리적 범위(+6.5%). multi 두 셀은 기준 대비 2.2~2.4배로 이례적으로 큼 —
S-4/S-10의 변경 규모(엔진 write-turn 정책, TLS 모델 컴파일 플래그) 대비 과도해 보이며
머신 부하 차이(측정 시 load avg 0.90→2.11, S-2/S-9 자체 작업은 계속되는 중이라던
가정과 달리 실측 중 낮음)나 Phase 0 측정 조건 차이일 가능성이 있음. 코드 판단은
브리프 범위 밖이라 수정하지 않고 수치만 보고함 — 감독관 재검토 요망.

## 8. load average 기록

빌드/측정 구간별: dev 빌드 전 12.71(다른 job과 동시), ctest 전 후 관측 없음(백그라운드),
hotpath_bench 빌드 후 hotpath_gate 측정 5.08→4.45, with_stream 2.67→2.76,
perf/c single 0.98→0.98, perf/c multi 0.90→2.11.
