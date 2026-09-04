# PUBSUB/inproc 64 KiB 회귀 수정 요약

## 결과

- 작업 기준: `main` / `8d58b7f891d34affd49b42f52930895e2d9a2684`
- 최종 64 KiB throughput: candidate `134,057.4`, core/v0.15.1 baseline `126,964.0` msg/s
- 최종 비율: `1.056` — 요구치 `>= 0.95` 통과
- 기능·성능 gate: 모두 통과
- commit, push, branch 전환 없음. baseline worktree와 금지 경로는 수정하지 않음.

## A/B 결과

모든 임시 되돌림은 source 편집과 `libzlink` 재빌드만으로 수행했고 즉시 원복했다. 측정은 candidate와 baseline을 동시에 실행하지 않고 순차 실행했다.

| 단계 | 64 KiB runs=3 median (msg/s) | 비교 | 판정 |
|---|---:|---:|---|
| 최초 current / baseline | 106,373.6 / 109,371.2 | 0.973 | 현 세션에서는 감독관의 0.38까지 재현되지 않았으나 하락 방향 확인 |
| `a339149dbb` 단독 revert 1차 | 127,221.8 | current 대비 1.196 | 회복 |
| `a339149dbb` 단독 revert 2차 | 103,407.6 | 인접 current 84,847.6 대비 1.219 | 회복 재현 |
| `e3d5c5b79f` 단독 revert | 106,610.0 | 인접 current 106,373.6 대비 1.002 | 영향 없음 |
| peer-control helper noinline-only | 108,465.8 / 125,317.6 | 0.866 | 근본 수정 아님, 제거 |
| wake 수정 + noinline 진단 | 123,718.4 / 111,224.8 | 1.112 | wake 원인성 확인 |
| noinline 제거, wake 수정만 | 120,769.4 / 105,994.6 | 1.139 | 최소 수정 통과 |
| 최종 full gate 뒤 재측정 | 134,057.4 / 126,964.0 | **1.056** | **통과** |

`a339149dbb`의 새 registry branch는 PUB/SUB application pipe에서 실행되지 않는다. 그러나 compiler가 rare peer-control body를 공통 `flush_unlocked` clone에 inline하면서 instruction footprint와 HWM 경계의 timing이 바뀌었다. helper 격리만으로는 기준을 안정적으로 넘지 못했으므로 이는 회귀의 촉발 요인이지 수정 지점은 아니었다. `e3d5c5b79f`의 CONNECT_ROUTING_ID alias 경로는 DEALER/ROUTER 전용이라 PUB/SUB와 무관했다.

## 실증 원인

벤치의 한 publish record charge는 topic, 65,536B payload, empty final frame을 합쳐 65,733B다. Auto HWM 1,048,576B에는 15 records만 들어가고, reader는 약 8 records drain마다 LWM credit 경계를 지난다. 1,024B record는 약 430 records마다 같은 경계를 지나므로 64 KiB 셀이 owner wake timing에 약 54배 민감하다.

XPUB NODROP은 실제 분배 전에 `dist_t::check_hwm(msg)`로 matching pipe를 preflight한다. HWM_FULL이면 pipe는 waiter를 publish하고 `_out_active=false`가 되지만, 실제 write가 없었으므로 `dist_t`는 그 pipe를 matching/active set에서 제거하지 않는다. 기존 재시도는 reader가 peer atomic에 충분한 byte credit를 이미 publish한 뒤에도 `_out_active=false`만 보고 즉시 HWM_FULL을 반환했다. 따라서 publisher owner가 queued `activate_write` command를 처리할 때까지 불필요한 scheduler/mailbox round trip이 매 LWM 경계에 들어갔다.

결정적 테스트에서 다음 순서를 고정했다.

1. 1-frame HWM을 채우고 dist message preflight로 waiter를 arm한다.
2. reader가 frame을 읽어 credit를 publish하되 writer owner command는 처리하지 않는다.
3. 두 번째 dist preflight와 전송·수신이 owner wake 전에 성공해야 한다.
4. 뒤늦은 `activate_write`는 중복 activation을 만들지 않아야 한다.

수정 전 분기로 임시 복원하면 새 테스트가 즉시 실패하고(`Expected 0 Was 1`), 기존 passive LB wake invariant는 계속 통과했다.

## 수정

- `core/src/runtime/core/pipe.cpp`
  - `check_hwm_for_message()`의 유일한 호출자인 `dist_t` message preflight에만 local peer-credit recovery를 허용했다.
  - `_out_active=false`일 때 peer의 단조 증가 credit snapshot을 acquire로 재확인하고, 충분할 때 waiter를 clear한다.
  - credit가 부족하면 기존 HWM_FULL 및 lost-wakeup arm/fence 경로를 그대로 사용한다.
  - 일반 `check_hwm()`과 LB/write admission은 변경하지 않아, 이미 active set에서 제거된 pipe를 passive probe가 임의로 재활성화하지 않는다.

- `core/tests/integration/test_router_multiple_dealers.cpp`
  - owner wake 처리 전 dist 재시도 성공과 stale wake의 무중복을 검증하는 회귀 테스트를 추가했다.
  - `a339149dbb`의 의도인 session Completion control enqueue/dequeue registry charge 대칭을 검증하는 테스트를 추가했다.
  - 두 테스트 모두 pipe/socket cleanup 뒤 결과를 assert해 실패도 결정적으로 종료한다.

공개 header, 객체 layout, API/ABI, HWM byte 산식, multipart atomicity, PUB NODROP 계약은 변경하지 않았다. `a339149dbb`의 physical-queue ledger commit/release와 `e3d5c5b79f`의 connect별 alias binding도 그대로 유지했다.

## 최종 재측정

| 트리 | run 1 | run 2 | run 3 | median |
|---|---:|---:|---:|---:|
| candidate | 134.06 K | 145.08 K | 103.87 K | **134,057.4** |
| core/v0.15.1 | 126.96 K | 129.63 K | 118.30 K | **126,964.0** |

- 비율: `134057.4 / 126964.0 = 1.0559`
- candidate report: `/home/hep7hep7/project/zlink-work/c016/light-results/ps64/single/report/perf_c_single_linux_20260904_093713_ps64-gate-final-r2.txt`
- baseline report: `/home/hep7hep7/project/zlink-work/c016/light-results/ps64/single/report/perf_c_single_linux_20260904_093741_ps64-gate-final-r2-b.txt`

WSL2 스케줄링 영향으로 세션 전체 raw 값의 분산은 컸다. 병렬 실행 한 쌍은 무효 처리했고, 최종 판정은 full gate 뒤 순차·인접 실행한 위 한 쌍을 사용했다. 최소 wake 수정은 별도의 순차 측정에서도 1.139를 기록했다.

## Gate

| Gate | 결과 |
|---|---|
| `ulimit -v 16777216; cmake --build core/build -j4` | PASS |
| `ulimit -v 16777216; ctest --test-dir core/build -j2 --output-on-failure` | PASS, 139/139, 168.27s |
| `ulimit -v 16777216; ctest --test-dir core/build -R '^hotpath_gate$' --output-on-failure` | PASS, 1/1 |
| `ulimit -v 16777216; ctest --test-dir core/build -L wake-invariant -j1 --output-on-failure` | PASS, 3/3 |
| raw header mirror `cmp` | PASS, 12/12 |
| `git diff --check` | PASS |

최종 tracked diff는 다음 두 파일뿐이다.

- `core/src/runtime/core/pipe.cpp`
- `core/tests/integration/test_router_multiple_dealers.cpp`
