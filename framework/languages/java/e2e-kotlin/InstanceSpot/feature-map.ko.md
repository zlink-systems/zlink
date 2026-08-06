# Kotlin InstanceSpot E2E feature map

이 fixture는 Java fixture를 복사하지 않고 Kotlin process 두 종류로 구성한다. Owner는
Kotlin `ZLinkSuspendingInstanceSpot`과 public factory를 등록하고, Client는 Kotlin route-call
wrapper와 public `ZLinkSpotManager.find`를 사용한다. 각 process에는 health/evidence 또는
request/send/lookup/close/concurrent HTTP endpoint가 있으며, runner는 endpoint 결과와 owner
evidence를 함께 판정한다.

## 실제 검증 결과

`IS-E2E-01`, `02`, `03`, `04`, `08`, `19`, `26`, `31`은 실제 process·Redis·typed wire 결과를
검증한다. `run_e2e.sh`는 selector별 `logs/<run-id>/scenario-IS-E2E-*.log`와
`scenario-status.tsv`에 결과를 남긴다. `all`은 검증 가능한 selector를 PASS로 기록하고 나머지는
public control이 없는 정확한 이유와 함께 BLOCKED로 기록한다.

최종 전체 실행은 `logs/20260806-043215-1405755/scenario-status.tsv`에 기록되어 있다. 결과는
`PASS 8개`, `BLOCKED 28개`이며 FAIL은 없다. PASS selector는
`IS-E2E-01`, `IS-E2E-02`, `IS-E2E-03`, `IS-E2E-04`, `IS-E2E-08`, `IS-E2E-19`,
`IS-E2E-26`, `IS-E2E-31`이다. BLOCKED selector는
`IS-E2E-05..07`, `IS-E2E-09..18`, `IS-E2E-20..25`, `IS-E2E-27..30`,
`IS-E2E-32..36`이며, 각 항목의 정확한 이유와 scenario log 경로는 같은 `scenario-status.tsv`에
있다.

| Scenario | Status | 실제 근거 또는 blocker |
|---|---|---|
| IS-E2E-01 | PASS | cold request의 typed reply, public lookup Ready identity, factory/initialize/handler exactly-once |
| IS-E2E-02 | PASS 또는 BLOCKED | gated one-way send completion과 SEND_HANDLER 순서를 실제 확인한다. runtime이 handler 완료까지 기다리면 BLOCKED다. |
| IS-E2E-03 | PASS | 두 Client process의 16개 first request, 단일 activation, operation별 commit |
| IS-E2E-04 | PASS | 한 Spot handler를 gate한 동안 다른 Spot의 reply와 activation이 진행되는지 확인 |
| IS-E2E-05..07 | BLOCKED | owner crash/restart 또는 relocation 비교를 제어하는 public fixture가 없다. relocation은 명시적으로 disable한다. |
| IS-E2E-08 | PASS | public close packet 완료 뒤 다른 object generation으로 재활성화 |
| IS-E2E-09..18 | BLOCKED | crash, stale fencing, admission/capacity, store outage, User Spot conflict, multi-Mesh, cross-language topology가 없다. |
| IS-E2E-19 | PASS | 첫 handler gate 동안 follow-up이 실행되지 않고 queue 순서가 evidence에 남는지 확인 |
| IS-E2E-20..25 | BLOCKED | close callback crash, deadline, negative capability, delayed store, initialize failure 제어가 없다. |
| IS-E2E-26 | PASS | 두 Client process가 같은 Missing Spot을 동시에 요청해 단일 claim/factory로 수렴하는지 확인 |
| IS-E2E-27..30 | BLOCKED | 독립 activation deadline, close/admission 경쟁, cross-Mesh relocation 제어가 없다. |
| IS-E2E-31 | PASS | 두 Client의 동시 cold request가 하나의 selected owner와 두 handler commit으로 수렴하는지 확인 |
| IS-E2E-32..36 | BLOCKED | activation 경계 crash, failure injection, unpublished cleanup, Ready-owner queue recovery 제어가 없다. |

## Kotlin public contract gap

Kotlin의 `ZLinkSuspendingSpotPacketHandler<TSpot : ZLinkSpot<*>>` projection은
`ZLinkInstanceSpot`를 handler type으로 받을 수 없다. 따라서 Instance Spot handler에는 이를
억지로 맞추기 위한 reflection, raw frame, private API를 사용하지 않는다. Kotlin source에서 public
Java `ZLinkSpotPacketHandler`와 `ZLinkSpotRequestHandler`를 구현하고 `CompletionStage`를 반환한다.
이 gap을 메우는 Kotlin framework public API 추가는 이번 작업 범위에 포함하지 않으며, 공통
contract parity 작업의 입력으로 남긴다.
