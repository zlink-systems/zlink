# gate s3 (S-3, handler_allocator inline block 1 → 2)

## 0. 사전 점검
- `git status --short -- core bindings scripts` 비어 있음 → 진행. 전체 워킹트리에는
  `progress-S-3.md`(수정)·`progress-S-1.md`·`progress-S-5.md`(신규, untracked) worklog 잔여만 있어
  브리프대로 무시.
- `git pull --rebase -q` 중 위 worklog 파일이 막아 `git stash push -u`로 치우고 pull 후 즉시 복원.
  main HEAD: 828c0a782c8b 확인. (측정 도중 감독관/타 세션이 doc/plan 전용 커밋 2건을 추가로 push —
  `00261169a1`, `985ef5332e`(D-B151, perf/c multi는 조용한 머신에서만 판정) — core/bindings/scripts
  비영향이라 재확인만 하고 계속 진행.)

## 0.5 사전 flake 기준선(브리프 추가 지시)
- 패치 적용 전, HEAD 기준으로 dev 트리 재빌드 후
  `ctest -R test_stream_socket_recv_multiclient_ready_regression --repeat until-fail:20` (PERF_LOCK):
  **20/20 pass**.

## 1. 패치 적용
- worktree `~/project/zlink-work/s3`에서 `git diff` → 1파일(`handler_allocator.hpp`), +30/-10.
- `git apply --3way` **충돌 없이 clean 적용**.
- `git diff --stat -- core/include core/src/libzlink.vers` 비어 있음 확인(공개 인터페이스 불변).

## 2. 빌드
- `JOBS=6 scripts/build-core.sh dev` — 성공.
- `JOBS=6 scripts/build-core.sh release --lib-only` — 성공(`libzlink.so.0.17.0`, 방금 재빌드 확인).
- `cmake --build core/build-gate --target hotpath_bench -j6` — 성공.

## 3. ctest (core/build-dev)
- 전체 1회: **208/209 통과**. 실패 1건:
  - `hotpath_gate`(ctest 내장, dev=비최적화 빌드) — 단독 3회 재실행 모두 결정론적 FAIL, 5셀 전부
    ratio 1.10~1.31. dev 트리 특성상 상시 어긋나는 기존 케이스(정성적 회귀 아님, 브리프도 "dev 트리에
    없어야 정상"이라 명시 — 이번엔 존재했으나 권위 있는 신호는 §5 valgrind 게이트).
  - `test_close_completion_poller_release`(D-B147 기존 간헐)는 이번 1회 실행에서 실패하지 않음.
- 대상 suite(`decoder|raw|stream|zmp|memory|hwm|engine|asio`) 5회(PERF_LOCK): 35개 테스트 ×5회
  **175/175 전부 통과**, 회귀 없음.

## 3.5 flake 재측정(패치 적용 후, 브리프 추가 지시)
- `ctest -R test_stream_socket_recv_multiclient_ready_regression --repeat until-fail:20` (PERF_LOCK):
  **20/20 pass**.
- **비교: 20/20 (패치 전) vs 20/20 (패치 후) — flake 변화 없음.**

## 4. 공개 인터페이스 확인
- `git diff --stat -- core/include core/src/libzlink.vers` 재확인 비어 있음(패치가 core/src만 수정).
- `scripts/gate/README.md`에 mirror cmp 절차 없음 → 브리프 대체 절차: `core/include`의 8개 헤더
  × bindings 4개 미러(c/cpp/go/rust). 평평한 3개 헤더(zlink.h, zlink_enum.h, zlink_errno.h)는
  각 미러에 대응 파일 존재 — 12/12 cmp 전부 일치. 나머지 5개(zlink/common.h,
  zlink/core/api.h, zlink/eventing/api.h, zlink/message/api.h, zlink/socket/api.h)는 4개 미러
  어디에도 평면 사본이 없음(구조상 대응 없음, 패치가 건드리지 않은 영역이라 영향 없음).

## 5. hotpath_gate 5셀 (build-gate, valgrind, PERF_LOCK)

| cell | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3378.479 | 0.987 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19331.063 | 0.982 | PASS |
| pair_inproc | 2527.834 | 2501.690 | 0.990 | PASS |
| router_router_tcp | 2972.532 | 2952.453 | 0.993 | PASS |
| stream_tcp | 14623.471 | 14615.851 | 1.000 | PASS |

전 5셀 PASS(과대 개선으로 인한 FAIL 없음).

## 6. 성능 (PERF_LOCK 하에서 순차 측정)

측정 시각 내내 load average 6.2~9.75로 조용한 머신 조건이 아니었음(다른 세션의 동시 활동 정황,
§0 참조). D-B151(perf/c multi는 조용한 머신에서만 판정)이 지적한 정확히 그 상황이라 아래 perf/c
수치, 특히 multi 2셀은 참고용으로만 보고.

### 6.1 with_stream (zlink,asio, size all, ccu 1000, runs 1, reuse-build, ZLINK_CORE_SOURCE=local)

| size | zlink (kops) | asio (kops) | zlink/asio ratio | Phase 0 기준(§7.1) |
|---|---:|---:|---:|---:|
| 64 B | 292.24 | 354.51 | 0.824 | 0.835 |
| 1024 B | 160.58 | 181.00 | 0.887 | 0.768 |
| 65536 B | 13.44 | 20.76 | 0.647 | 0.775 |

64/1024 B는 기준 근방(1024는 기준 상회). 65536 B는 기준(0.775) 대비 낮음 — runs=1·load avg
6~9대 단발 측정이라 노이즈 가능성이 있음(코드 판단은 브리프 범위 밖, 수치만 보고).

### 6.2 perf/c 1024 B tcp 경량 3셀 (§7.2 Phase 0 기준 대비)

| cell | Phase 0 기준 | 측정 | ratio | load avg |
|---|---:|---:|---:|---|
| single ROUTER_ROUTER | 744.4 Kmsg/s | 291.31 Kmsg/s | 0.391 | ~6.2 |
| multi ROUTER_ROUTER_SENDSEND | 111.5 Kops/s | 46.274 Kops/s | 0.415 | ~9.75 |
| multi ROUTER_ROUTER_REQREP | 73.0 Kops/s | 39.254 Kops/s | 0.538 | ~9.75 |

세 셀 모두 기준 대비 큰 폭 하락 — hotpath_gate(valgrind, 명령어 수 기반, 머신 부하 무관)는 전부
PASS이므로 코드 자체의 회귀로 보기 어렵고, 측정 시점의 높은 load average(다른 세션 동시 활동)가
주 원인으로 추정됨. D-B151 규칙대로 조용한 머신에서 재측정 필요.

## 7. 결론
- 패치 충돌 없음, 공개 인터페이스 불변, dev/release/gate 빌드 전부 성공.
- flake 기준선: 패치 전 20/20 pass, 패치 후 20/20 pass — flake 변화 없음.
- ctest 전체 1회 208/209(hotpath_gate 제외 전부 통과, dev-tree 고유 이슈로 판단), 대상 suite 5회
  175/175 통과 — 새 회귀 없음.
- 결정론적 hotpath 5셀: **5/5 PASS**(1차 신호).
- 성능: 측정 시점 머신이 조용하지 않았음(load avg 6~9.75). with_stream은 대체로 기준 근방(65536 B만
  낮음), perf/c 경량 3셀은 셋 다 기준 대비 크게 낮음 — hotpath_gate PASS와 상충하는 정황이라 코드
  회귀보다는 부하 노이즈로 추정. 감독관이 조용한 머신에서 재측정 여부 판단.
- 메인 워킹트리는 패치 적용 상태 그대로 유지, 커밋하지 않음.
