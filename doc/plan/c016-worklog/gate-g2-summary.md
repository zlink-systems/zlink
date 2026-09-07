# gate g2 (G-2, msg.cpp/msg.hpp/pipe.cpp 3줄)

## 0. 사전 점검
- `git status --short -- core bindings scripts` 비어 있음(doc/plan worklog만 잔여, 무시) → 진행.
- main HEAD `d98ef46fd7`, origin과 동일(추가 pull 불필요).

## 1. 패치 적용
- worktree `~/project/zlink-work/g2`(cut point `f67625990a`)에서 `git diff HEAD` → 4파일(+66/-92): msg.cpp, msg.hpp, pipe.cpp, unittest_pipe_byte_charge.cpp.
- `git apply --3way` **충돌 없이 clean 적용**.
- `git diff --stat -- core/include core/src/libzlink.vers` 비어 있음(공개 인터페이스 불변, core/include 미변경).

## 2. 빌드
- `JOBS=4 scripts/build-core.sh dev` — 성공.
- `JOBS=4 scripts/build-core.sh release --lib-only` — 성공(`libzlink.so.0.17.1`).
- `JOBS=4 cmake --build core/build-gate --target hotpath_bench -j4` — 성공.

## 3. ctest (core/build-dev)
- 전체 1회(-j2): **206/208 통과**. 실패 2건:
  - `test_single_lane_flow_snapshot_accounting` — 브리프에 명시된 기존 간헐. 단독 3회 재실행: 3/3 pass → 간헐 확인, job과 무관.
  - `hotpath_gate`(ctest 내장, dev=비최적화 빌드) — 5셀 모두 valgrind 없이 명령-수 기반이라 상시 어긋남. 권위 있는 신호는 §5의 valgrind 게이트.
- 대상 suite(`msg|message|part|stream|router|dealer|pubsub|pair|pipe|wake|poll`) 5회(-j2): 65개 테스트, **5/5 전부 100% 통과**, 회귀 없음.
- lost-wake 셋(`-L wake-invariant`, 4개 테스트) `--repeat until-fail:10`: **40/40 100% pass**.

## 4. 공개 인터페이스 확인
- `git diff --stat -- core/include core/src/libzlink.vers` 재확인 비어 있음.
- `scripts/gate/README.md`에 mirror cmp 절차 없음 → 대체 절차: `find . -path ./core -prune -o -name 'zlink*.h' -print`로 찾은 zlink.h/zlink_enum.h/zlink_errno.h × bindings 4개 미러(c/cpp/go/rust) = 12개 `cmp` **전부 일치**.

## 5. hotpath_gate 5셀 (build-gate, valgrind, PERF_LOCK)
load avg 5.36→3.17 측정 시작.

| cell | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3298.023 | 0.9633 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19322.696 | 0.9817 | PASS |
| pair_inproc | 2527.834 | 2348.457 | 0.9290 | **FAIL(개선 +7.1%, reference 미수정, 값만 보고)** |
| router_router_tcp | 2972.532 | 2929.606 | 0.9856 | PASS |
| stream_tcp | 14623.471 | 14377.902 | 0.9832 | PASS |

G-2 주장(pair_inproc −6.3%, 나머지 −1.5~−2.3%)과 방향·크기 대체로 일치. pair_inproc은 +5% 초과 개선으로 브리프 규칙상 FAIL 표기.

## 6. 성능 (PERF_LOCK 하에서 순차 측정)

### 6.1 with_stream (zlink,asio, size all, ccu 1000, runs 1, reuse-build, ZLINK_CORE_SOURCE=local)
load avg 2.57→2.11.

| size | zlink (kops) | Phase 0 기준(§7.1) | ratio | asio (kops) | Phase 0 기준 | ratio |
|---|---:|---:|---:|---:|---:|---:|
| 64 B | 313.95 | 268.9 | 1.168 | 384.01 | 322.0 | 1.193 |
| 1024 B | 294.08 | 243.0 | 1.210 | 359.42 | 316.4 | 1.136 |
| 65536 B | 32.55 | 30.4 | 1.071 | 42.28 | 39.2 | 1.079 |

세 크기 모두 기준 이상, 하락 없음.

### 6.2 perf/c 1024 B tcp 경량 3셀 (§7.2 Phase 0 기준 대비)
load avg: single 2.23→(측정 중), multi 1.79→(측정 중).

| cell | Phase 0 기준 | 측정 | ratio |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 744.4 Kmsg/s | 849.55 Kmsg/s | 1.141 |
| multi ROUTER_ROUTER_SENDSEND | 111.5 Kops/s | 258.94 Kops/s | **2.322** |
| multi ROUTER_ROUTER_REQREP | 73.0 Kops/s | 196.05 Kops/s | **2.686** |

single 셀은 합리적 범위(+14.1%). multi 두 셀의 2.3~2.7배는 gate-s1/gate-s4-s10에서도 동일하게
관측된 기존 이례(측정 조건·머신 부하 차이로 추정)와 일치하는 패턴 — G-2의 3줄 변경 규모로는
설명되지 않음. 코드 판단은 브리프 범위 밖이라 수치만 보고.

## 7. 결론
- 패치 충돌 없음, 공개 인터페이스 불변, dev/release/gate 빌드 전부 성공.
- ctest 전체·5회 반복(msg|message|part|stream|router|dealer|pubsub|pair|pipe|wake|poll)·until-fail:10 모두
  기존 간헐(test_single_lane_flow_snapshot_accounting) 외 회귀 없음.
- 결정론적 hotpath 5셀: 4 PASS, pair_inproc은 개선 초과로 FAIL(수치만 보고, reference 미변경) — G-2 주장과 부합.
- 성능: with_stream 전 구간 기준 이상, perf/c single +14.1%, multi 두 셀은 기존에도 관측된 이례적 배율.
- 메인 워킹트리는 패치 적용 상태 그대로 유지, 커밋하지 않음.
