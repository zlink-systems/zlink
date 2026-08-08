# Java Instance Spot E2E feature map

공통 Config 14의 `IS-E2E-01..36`을 Java process fixture로 매핑한다. PASS는 Java caller와
owner process가 실제로 시작되고, public API 호출과 wire 결과, owner가 제공하는 application
evidence를 모두 assertion으로 확인한 경우만 사용한다. BLOCKED는 scenario를 성공으로 가장하지
않고, 현재 fixture에서 필요한 public control 또는 다른 언어/process topology가 없다는 사실과
실행 evidence를 함께 남긴다.

실행 결과는 runner가 출력한 `log_dir` 아래의 `scenario-status.tsv`에 기록한다. BLOCKED
scenario도 client `/health`, client `/lookup`, owner `/evidence`를 호출한 뒤 판정하므로 marker만
기록하지 않는다. `e2e.disable-relocation=true`는 relocation 시나리오를 임의로 PASS 처리하지
않기 위한 명시적 fixture 조건이다.

| Scenario | 상태 | 실제 검증 또는 정확한 BLOCKED 원인 |
|----------|------|------------------------------------|
| `IS-E2E-01` | PASS | 두 owner와 client process에서 cold request를 호출하고 typed reply, public lookup의 Ready identity, factory/initialize/handler exactly-once를 확인한다. |
| `IS-E2E-02` | PASS | owner gate를 닫은 cold send가 handler evidence보다 먼저 반환되는지 확인한 뒤 gate를 열고 one-way handler exactly-once를 확인한다. |
| `IS-E2E-03` | PASS | 두 client process가 같은 Missing ID에 16개 request를 동시에 보내고 하나의 factory, 모든 operation의 단일 reply/commit, active handler `<= 1`을 확인한다. |
| `IS-E2E-04` | PASS | Spot A handler를 gate로 대기시키고 다른 ID의 Spot B request reply가 gate 개방 전에 도착하는지 process 간에 확인한다. |
| `IS-E2E-05` | PASS | Ready owner process를 `SIGKILL`한 뒤 lease가 만료된 상태에서 후속 request가 bounded `UNAVAILABLE`로 끝나고, surviving owner에 새 factory·initialize·handler가 없음을 확인한다. 최신 evidence는 `logs/20260807-235210-2024824/`에 남긴다. |
| `IS-E2E-06` | BLOCKED | factory entry 중 process crash와 same-generation recovery boundary를 제어할 fixture가 없다. |
| `IS-E2E-07` | BLOCKED | owner factory를 `disableRelocation()`으로 등록했고 public relocation process가 없다. |
| `IS-E2E-08` | PASS | public close packet completion을 기다린 뒤 같은 ID를 다시 request하고 factory/initialize 2회, 세대 변경, close evidence와 두 handler commit을 확인한다. |
| `IS-E2E-09` | BLOCKED | Ready-owner crash 뒤 동시 request와 public lease invalidation을 함께 제어할 fixture가 없다. |
| `IS-E2E-10` | BLOCKED | process pause/resume와 stale-owner fencing을 제어할 fixture가 없다. |
| `IS-E2E-11` | BLOCKED | public admission rejection 또는 capacity controller를 설정한 topology가 없다. |
| `IS-E2E-12` | BLOCKED | target acceptance 직후 connection을 끊는 network proxy가 없다. |
| `IS-E2E-13` | BLOCKED | accepted send 직후 target 종료와 replacement owner를 orchestration할 fixture가 없다. |
| `IS-E2E-14` | BLOCKED | Redis outage/proxy를 제어하면서 owner public evidence를 유지하는 fixture가 없다. |
| `IS-E2E-15` | BLOCKED | fixture에는 Instance factory만 있고 동일 ID의 User Spot contention process가 없다. |
| `IS-E2E-16` | BLOCKED | no-eligible-node와 exhausted-capacity를 각각 실행할 별도 topology가 없다. |
| `IS-E2E-17` | BLOCKED | public activation-concurrency 설정과 factory gate를 함께 제공하는 fixture가 없다. |
| `IS-E2E-18` | BLOCKED | Java 이외 Framework 언어의 caller/owner process가 현재 작업 범위에 없다. |
| `IS-E2E-19` | PASS | 첫 request handler를 gate로 지연시키고 follow-up request가 먼저 실행되지 않는지 handler-enter sequence와 typed replies로 확인한다. |
| `IS-E2E-20` | BLOCKED | close callback 지연 중 owner crash를 제어하는 fixture가 없다. |
| `IS-E2E-21` | BLOCKED | 하나의 Mesh만 설정되어 initial Mesh와 Ready 이후 다른 Mesh 지정의 차이를 비교할 수 없다. |
| `IS-E2E-22` | BLOCKED | process pause/resume로 monotonic owner deadline을 검증하는 fixture가 없다. |
| `IS-E2E-23` | BLOCKED | forbidden capability를 등록하는 negative factory가 없다. |
| `IS-E2E-24` | BLOCKED | Location Store response를 request deadline보다 늦추는 proxy가 없다. |
| `IS-E2E-25` | BLOCKED | initialize one-shot failure injection이 없다. |
| `IS-E2E-26` | PASS | 두 client process가 같은 Missing ID에 동시에 request하고 single claim, single factory, owner 일치 handler evidence를 확인한다. |
| `IS-E2E-27` | BLOCKED | handler gate는 있지만 shared activation waiter와 caller별 deadline을 독립 제어하는 fixture가 없다. |
| `IS-E2E-28` | BLOCKED | Close entry와 admission을 정밀하게 경쟁시키는 public fixture control이 없다. |
| `IS-E2E-29` | BLOCKED | relocation을 비활성화했고 cross-Mesh in-flight relocation process가 없다. |
| `IS-E2E-30` | BLOCKED | relocation을 비활성화했고 concurrent relocation controller가 없다. |
| `IS-E2E-31` | PASS | 두 client process가 같은 Missing ID에 동시에 cold request를 보내고 선택된 owner 한 곳의 factory/handler evidence만 존재하는지 확인한다. |
| `IS-E2E-32` | BLOCKED | activation 중 source/target crash boundary를 분리해 실행할 orchestration이 없다. |
| `IS-E2E-33` | BLOCKED | factory 또는 initialize failure를 주입하는 fixture control이 없다. |
| `IS-E2E-34` | BLOCKED | unpublished activation 중 target crash/restart를 제어하는 fixture가 없다. |
| `IS-E2E-35` | BLOCKED | Ready owner crash 뒤 pending queue를 유지하고 자동 queue recovery가 발생하지 않음을 확인하는 별도 process sequence가 없다. 현재 `IS-E2E-05`와 같은 lease-loss probe만으로 이 scenario를 PASS로 계산하지 않는다. |
| `IS-E2E-36` | BLOCKED | handler entry 전후 crash injection을 분리할 fixture가 없다. |

이 표의 BLOCKED 항목은 완료로 계산하지 않는다. 해당 public control을 구현하지 않는 한
`run_e2e.sh all`은 BLOCKED 결과와 함께 non-zero를 반환한다.
