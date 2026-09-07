# diag: `test_sl_flow_snapshot_accounts_dr_reply_as_application` 진단 (G-1/G-3)

## 결론 한 줄
**G-1/G-3가 도입한 결함이 아니다.** 이 실패는 pristine main(2748d20e54, core 내용은 08da256f1e와 동일)에서 이미
**20~30% 확률로 재현되는 기존 간헐 결함**이며, G-1·G-3 적용 여부와 무관하다. 게이트 g1-g3 보고서의
"100% 결정적 회귀" 판정은 표본이 작았고(단독 3/5회) 게이트 실행 당시 부하가 겹쳐 생긴 오판이다.

## 1. 대상 테스트와 실패 지점
- integration: `core/tests/integration/test_dealer_router_single_lane_contract.cpp:2842`
  — `zlink_test_wait_until (5000ms, snapshot.current_accounted_bytes > baseline && snapshot.core_queue_accounted_bytes > baseline)` 타임아웃(`Expected TRUE Was FALSE`).
- unittest: `core/tests/unittest/unittest_single_lane_accounting.cpp:151` (같은 이름, pump 기반). 단독 실행 10/10 PASS —
  게이트가 본 `Expected 4 Was 2`는 ctest 레벨 집계 문구로, 단독 재현되지 않는다.
- 시나리오: DEALER를 `RECEIVE_FLOW_PAUSED`로 두고 ROUTER가 MORE prefix(1024B)를 socket-local 스테이징 →
  DEALER를 `RUNNING`으로 되돌리고 ROUTER가 FINAL(1024B) 제출 → 두 프레임(2×(1024+64)=**2176 B**)이
  application 물리 큐에 적재되어 `core_queue_accounted_bytes`/`current_accounted_bytes`가 baseline보다 커야 한다.
- 값을 만드는 주체: `ctx_physical_queue_registry_t::current_accounted_bytes`
  (`core/src/runtime/core/ctx_physical_queue_registry.cpp:996`) → application lane이면
  `sample_application_pipe_queue` (`:473`) → `pipe_t::snapshot_outbound_queue_accounting`,
  아니면 레지스트리 원장 `current_queue_bytes` fallback.

## 2. Bisect 표 (dev 빌드, ctest 단독 실행)
| 트리 | 내용 | 결과 |
|---|---|---|
| `~/project/zlink-work/diag-base` | pristine main 2748d20e54 | 0/10 FAIL, 이후 3/35, 계측판 **12/60·4/30·4/15 FAIL (≈20~30%)** |
| `~/project/zlink-work/diag-g1` | main + G-1만 | **1/10 FAIL** |
| `~/project/zlink-work/diag-g3` | main + G-3만 | 0/10 FAIL |
| `/home/hep7hep7/project/zlink` | main + G-1 + G-3 (게이트 상태) | 0/10 FAIL(+초기 ctest 1회 FAIL) |
| base, CPU 부하 8스레드 | pristine main | 2/20 FAIL |

→ 어느 패치도 실패율을 유의하게 바꾸지 않는다. pristine base가 가장 높은 실패율을 보였다(계측 빌드가 타이밍을 조금 바꾼 영향 포함).

## 3. 관측된 실패 양상 (base 계측)
성공/실패가 **이분법적**이다 — 창(window)을 놓치는 문제가 아니다.
- 성공: 첫 폴에서 곧바로 `cur=2176 cq=2176 prov=0 tot=2176`, 이후 테스트 끝까지 유지.
- 실패: 5 s / 200회 폴 내내 `cur=0 cq=0 prov=0 tot=0`. baseline도 0.
- 실패 시에도 reply는 **정상 배달**된다: 이어서 `zlink_completion_recv`가 `kind=REQUEST, reply_part_count=2`를 즉시 반환.
- 실패 시 router/dealer 모두 `flow_paused_connections=0` — 재개 lost-wake로 pending 큐에 갇힌 것이 **아니다**.

즉 "메시지는 흘렀는데 그 2176 B가 어떤 application 물리 큐에도 계상되지 않은" 상태다.

## 4. 근본 원인 (특정 지점까지, 미확정 부분 명시)
- G-1의 `refresh_application_hwm_if_drained` early return(`ctx_physical_queue_registry.cpp:955`)은
  원본이 락 안에서 하던 `planned==applied` 조기 반환과 **동일 효과**다(원본 `:975`). 누락된 부수효과 없음.
- G-1의 deferred-termination head atomic(`socket_base_dispatch.cpp:37,45`)은 큐 비어있음 판정만 락 밖으로 뺀 것으로,
  생산자가 같은 command owner라 drain 경계가 바뀌지 않는다. 회계 경로와 무관.
- G-3의 14개 술어 인라인화와 `publish_session_outbound_accounting_unlocked` hot/cold 분리
  (`pipe.hpp:1094` inline wrapper + `pipe.cpp:3958` slow half)는 `_session_io_writer` 가드·저장 순서·
  provisional 갱신 조건을 그대로 보존한다. 빠진 publish/flag reset/ordering 없음. 게다가 inproc 경로는
  `_session_io_writer == false`라 이 함수 자체가 이 테스트에 관여하지 않는다.
- 남은 유력 후보(**미확정**): 물리 큐 direction의 lane 분류가 원자적으로 나중에 바뀔 수 있다는 점.
  `accounting_lane()`(`ctx_physical_queue_registry.cpp:184`)은 `direction_.lane`을 `atomic` load 하고,
  `current_accounted_bytes`/`sample_application_pipe_queue`는 lane이 application이 아니면 레지스트리
  원장 fallback을 쓴다. inproc 단일 레인 핸드셰이크에서 이 분류가 확정되기 전/후 어느 쪽으로 굳는지가
  연결마다 한 번 결정되면 이 이분법적 0 vs 2176을 그대로 설명한다.
  (`pipe.cpp:191-193`의 `bind_application_pipe_queue`는 좌우 대칭·결정적이라 원인이 아니다.)
  1.5 h 상한 안에서 레지스트리 내부 계측까지는 가지 못했다.

## 5. 수정
**하지 않았다.** 책임 worktree가 G-1도 G-3도 아니므로 두 job의 diff에 손댈 것이 없다. 기존 결함이며,
수정은 별도 job(레지스트리 lane 분류 타이밍)으로 잡아야 한다. `diag-base` worktree에는 진단용 계측
(`test_dealer_router_single_lane_contract.cpp`의 DBG 출력)만 남아 있고 core 소스는 무변경이다.

## 6. 분류
- **B(기존 결함) — pre-existing timing exposed.** G-1/G-3가 도입한 결함이 아니다.
- 성능 주장에 영향 없음: G-1(STREAM −3.9%)·G-3(−1.1~−3.4%) claim은 이 실패와 무관하며,
  게이트 g1-g3의 hotpath·with_stream·perf/c 수치는 그대로 유효하다.

## 7. 권고
1. 게이트 g1-g3의 채택 보류 사유를 해제한다(이 실패는 채택 blocker가 아니다).
2. `test_sl_flow_snapshot_accounts_dr_reply_as_application`(integration)을 알려진 간헐 목록에 등록하고,
   §4의 lane 분류 타이밍을 별도 job으로 조사한다.
