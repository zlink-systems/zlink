# gate s2-s9 — S-2(mutex 통합/fast_mutex 제거), S-9(asio_engine read path + fastpath policy) 포팅 + 채택 게이트

worktree: S-2 `~/project/zlink-work/s2`, S-9 `~/project/zlink-work/s9`. main HEAD은 시작 시 `bf86bdc23c`(597f134d68을 포함, S-4/S-10 반영됨). main 워킹트리는 patch 적용 상태로 남김(커밋 안 함).

## 1. patch 적용

- `git status --short` 확인: 비어 있음 → 진행. `git pull --rebase -q` → 변경 없음(이미 최신, HEAD `bf86bdc23c`).
- S-2: 22개 파일(core/src/api/socket 2, core/src/runtime/core 8, core/src/runtime/sockets 6, core/src/runtime/utils 3 — `fast_mutex.hpp` 삭제, `mutex.hpp` 통합 — core/tests/unittest 3, `unittest_fast_mutex.cpp` → `unittest_mutex.cpp`). `git diff HEAD`로 스테이지/언스테이지 변경을 모두 포함해 patch 생성(단순 `git diff`는 staged rename/delete를 놓침). `git apply --3way` **충돌 없이 clean 적용**.
- S-9: `core/src/runtime/engine/asio/{asio_engine.cpp,asio_engine.hpp,asio_stream_fastpath_policy.hpp}` (3 files) — S-4/S-10이 이미 main에 있는 상태에서 **충돌 없이 clean 적용**(mechanical/manual 해결 불필요).
- `git diff --stat -- core/include core/src/libzlink.vers` → 빈 결과(공개 인터페이스 불변 확인).

## 2. 빌드

- `JOBS=6 scripts/build-core.sh dev` → 성공, exit 0 (`core/build-dev/lib`).

## 3. ctest (core/build-dev)

- 전체 1회: **208/209 통과**. 실패: `hotpath_gate`(dev 트리 비최적화 빌드 — 브리프에 명시된 정상 케이스, 5셀 전부 ratio 1.07~1.30의 성능 FAIL이지 correctness 실패 아님).
- 합집합 suite(`stream|pipe|mailbox|poller|wake|socket|mutex|engine|asio|raw|zmp|large|fragment|progress`, 81개) 5회: **5/5 run 모두 100% 통과(0 failed)**, 각 run ~93~95초.
- **S-2 lock-type 변경에 대한 10x hang 점검** (`wake|poll|stream|pipe|mailbox`, 26개, 시스템은 병행 job들로 부하 있음): 10회 중 **3회에서 간헐 실패/타임아웃 발견 — 행(hang) 성향의 finding**:
  - 테스트: `test_close_completion_poller_release` (integration)
  - 정상 단독 실행 시 0.10~0.13초. 실패한 3회 중 최소 1회는 CTest가 **Timeout(10.01초, ≈77~100배)**으로 강제 종료했고, 어서션 로그는 `test_close_completion_poller_release.cpp:69:test_close_completion_poller_release_with_monitor:FAIL: Expected 1 Was 0`(모니터 완료 카운트 불일치)였음.
  - 단독 재실행 3회는 모두 정상 통과(0.10~0.13초) — 오직 이 suite를 동시 실행하는 조건에서만 재현. S-2가 락 타입을 바꾼 경로(모니터 완료 통지 레이스)에서 노출된 것으로 보이나 코드 판단은 브리프 범위 밖 — **감독관 재검토 요망**.
  - 전체 10회 각 실행 시간: 50~57초(정상 범위, suite 자체의 hang 아님 — 개별 테스트 1건만 이례적).

## 4. 공개 인터페이스 확인

- `git diff --stat -- core/include core/src/libzlink.vers` 재확인: 빈 결과.
- `scripts/gate/README.md`에 mirror cmp 절차가 문서화되어 있지 않아 브리프의 fallback 사용: `find . -path ./core -prune -o -name 'zlink*.h' -print`로 core에는 `zlink.h, zlink_enum.h, zlink_errno.h` 3개만 존재, bindings(c/cpp/go/rust) 각 mirror와 `cmp`: **12/12 동일**.

## 5. release lib

- `JOBS=6 scripts/build-core.sh release --lib-only` → 성공(`core/build/lib/libzlink.so.0.17.0`, 소스 수정 직후 재빌드된 mtime으로 확인).

## 6. hotpath_gate (5셀, valgrind)

| cell | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3378.107 | 0.9867 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19422.162 | 0.9868 | PASS |
| pair_inproc | 2527.834 | 2501.058 | 0.9894 | PASS |
| router_router_tcp | 2972.532 | 2952.592 | 0.9933 | PASS |
| stream_tcp | 15540.385 | 14973.789 | 0.9635 | PASS |

+5% 초과 개선(FAIL-by-improvement) 없음. load avg 측정 시 5.26→4.61.

## 7. 성능

### 7.1 with_stream (zlink,asio, size all, ccu 1000, runs 1, reuse-build, ZLINK_CORE_SOURCE=local)

| size | zlink (kops) | Phase 0 기준 | ratio | asio (kops) | Phase 0 기준 | ratio |
|---|---:|---:|---:|---:|---:|---:|
| 64 B | 280.23 | 268.9 | 1.042 | 343.97 | 322.0 | 1.068 |
| 1024 B | 258.50 | 243.0 | 1.064 | 325.17 | 316.4 | 1.028 |
| 65536 B | 30.42 | 30.4 | 1.001 | 39.88 | 39.2 | 1.017 |

64 KiB 하락 없음(필수 확인 통과). load avg 2.41→2.71.

### 7.2 perf/c 1024 B tcp 경량 3셀 (§7.2 Phase 0 기준 대비)

| cell | Phase 0 기준 | 측정 | ratio |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 744.4 Kmsg/s | 706.78 Kmsg/s | 0.950 |
| multi ROUTER_ROUTER_SENDSEND | 111.5 Kops/s | 228.09 Kops/s | **2.046** |
| multi ROUTER_ROUTER_REQREP | 73.0 Kops/s | 144.90 Kops/s | **1.985** |

single 셀은 기준 대비 -5%(합리적 변동 범위). multi 두 셀은 앞선 gate-s4-s10 보고서(2.2~2.4배)와 같은 방향·비슷한 크기로 기준 대비 약 2배 — S-2/S-9의 변경 규모(mutex 통합, asio read path) 대비 과도해 보이며 두 게이트 모두에서 반복 관측된 것으로 보아 Phase 0 측정 조건 차이(머신/부하) 쪽에 무게가 실림. 코드 판단은 브리프 범위 밖이라 수정하지 않고 수치만 보고 — 감독관 재검토 요망.

## 8. load average 기록

dev 빌드/ctest 전 2.45(load1), 5x suite 구간 2.1~2.5 부근(병행 job 존재), 10x hang 점검 구간 타임아웃 발생 시 부하 상승 관측, hotpath_gate 측정 5.26→4.61, with_stream 2.41→2.71, perf/c single 2.26→2.24, perf/c multi 2.05→2.25.
