# Kotlin SubmitAdmission E2E feature map

기준 문서는
[`Config 13 — One-way submit admission`](../../../../doc/framework/common/e2e/config-13-submit-admission.ko.md)이다.
이 suite는 Kotlin public one-way wrapper의 `await()`를 실제 JVM process에서 호출하고, source
terminal과 target process의 handler evidence를 따로 확인한다.

공통 scenario의 완료 여부와 현재 fixture에서 실행 가능한 부분 검증을 구분한다. `부분 검증`은
실제 process/client evidence를 만들지만 공통 문서가 요구하는 전체 family matrix를 충족하지 않는
상태다. `차단`은 public contract 또는 현재 framework/runtime 경계 때문에 공통 조건을 만족할 수
없는 상태이며, runner가 성공으로 분류하지 않는다.

| 시나리오 | 상태 | 근거와 최신 실행 evidence |
|---|---|---|
| SA-E2E-01 | 부분 검증 | Kotlin Node direct와 RouteMesh Channel의 정상 terminal 및 target handler를 확인했다. Spot·Actor·Session·Stream family는 fixture에 없다. 최신 aggregate: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-012714-2299635/evidence.jsonl` |
| SA-E2E-02 | 차단 | Target의 public `paused=true`는 확인했지만 다음 remote Node `await()`가 pending이 아니라 즉시 `Submitted`가 됐다. HWM admission 경계를 고치려면 framework/core 변경이 필요하다. 실행: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-011218-2100708/process.log` |
| SA-E2E-03 | 차단 | 두 target 모두 HWM pause 상태에서 operation이 즉시 `Submitted`가 되어 success/deadline pending terminal을 만들 수 없었다. 실행: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-011845-2138045/process.log` |
| SA-E2E-04 | 차단 | deadline 전에 operation이 `Submitted`가 되어 `DeadlineExceeded`와 late-admission 차단을 검증할 수 없었다. 실행: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-011909-2168548/process.log` |
| SA-E2E-05 | 부분 검증 | 생성하지 않은 Node RID의 `NOT_FOUND`와 종료한 known target의 `UNAVAILABLE`을 각각 100회 확인했다. Actor·Spot logical identity variant는 fixture에 없다. 최신 aggregate: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-012714-2299635/evidence.jsonl` |
| SA-E2E-06 | 차단 | Shutdown variant에서 public status의 `acceptingWork=false`를 확인했지만 후속 send가 `SHUTTING_DOWN`이 아니라 `raw MeshNode is not started`로 끝났다. Relocate variant에는 public Location/Relocation Store fixture도 필요하다. 실행: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-012552-2297303/process.log` |
| SA-E2E-07 | 차단 | cancellation 대상 send가 pending이 되지 않고 `Submitted`가 됐다. 따라서 pre-commit cancellation과 publish commit을 구분할 수 없다. 실행: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-011858-2148381/process.log` |
| SA-E2E-08 | 부분 검증 | 같은 caller process의 local Node와 별도 target process의 remote Node에 Kotlin `await()`를 실행하고 각 handler 1회를 확인했다. HWM pending variant는 SA-E2E-02 blocker와 같다. 최신 aggregate: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-012714-2299635/evidence.jsonl` |
| SA-E2E-09 | 차단 | 기본 Channel submit 경로는 존재하지만 공통 scenario의 HWM success/timeout variant는 paused 상태에서도 즉시 `Submitted`가 됐다. SA-E2E-02/03 실행 evidence가 같은 admission 경계를 재현한다. |
| SA-E2E-10 | 차단 | ClientServer target의 handler 진입과 `paused=true`는 확인했지만 follow-up operation이 pending이 되지 않았고 client HTTP process evidence도 유지되지 않았다. 실행: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-011657-2124442/process.log` |
| SA-E2E-11 | 차단 | Spot owner generation과 route-loss/recovery를 제어하는 public process fixture가 없다. |
| SA-E2E-12 | 차단 | Actor owner generation과 route-loss/recovery를 제어하는 public process fixture가 없다. |
| SA-E2E-13 | 차단 | Logical Multicast target snapshot과 unavailable member를 제어하는 public Spot fixture가 없다. |
| SA-E2E-14 | 부분 검증 | subscriber가 없는 classic fanout publish의 정상 terminal과 late subscriber의 no replay를 확인했다. 최신 aggregate: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-012714-2299635/evidence.jsonl` |
| SA-E2E-15 | 차단 | bound Session과 Session Actor relay를 분리한 gateway process fixture가 없다. |
| SA-E2E-16 | 차단 | public STREAM peer와 server send sequence evidence를 제공하는 fixture가 없다. |
| SA-E2E-17 | 차단 | public request reply token과 concurrent terminal barrier를 제공하는 fixture가 없다. |
| SA-E2E-18 | 완료 | Direct target A는 `UNAVAILABLE`이고 A handler count는 0, Channel은 ready인 B를 선택해 `Submitted`와 B handler 1회를 기록했다. 최신 aggregate: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-012714-2299635/evidence.jsonl` |
| SA-E2E-19 | 완료 | route 단절 뒤 기존 operation은 `UNAVAILABLE`과 handler 0회로 끝났고, route 복구 뒤 새 operation만 `Submitted`와 handler 1회를 기록했다. 최신 aggregate: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-012714-2299635/evidence.jsonl` |
| SA-E2E-20 | 부분 검증 | remote Node handler gate가 닫힌 동안 Kotlin `await()` terminal이 먼저 완료되고, gate 해제 뒤 handler 1회를 확인했다. Channel·Spot·Actor·Session·Stream 전체 matrix는 fixture에 없다. 최신 aggregate: `/home/hep7/project/kairos/zlink/framework/languages/java/e2e-kotlin/SubmitAdmission/logs/20260806-012714-2299635/evidence.jsonl` |

## 실행

`Role:compileKotlin`은 `BUILD SUCCESSFUL`로 끝났다. 지원되는 부분 검증과 완전한 공통
scenario는 다음 명령으로 실행했다.

```bash
./run_e2e.sh supported
# PASS: SA-E2E-01 SA-E2E-05 SA-E2E-08 SA-E2E-14 SA-E2E-18 SA-E2E-19 SA-E2E-20
```

`SA-E2E-18`과 `SA-E2E-19`는 공통 scenario 완료 조건을 충족했다. HWM·Shutdown 및
fixture가 없는 scenario는 runner의 blocked selector로 유지한다. `SA-E2E-06-shutdown`은
Shutdown variant를 실제 실행하기 위한 별도 진단 selector이며, 공통 SA-E2E-06 완료로
분류하지 않는다.

Core/runtime, Java suite, common 문서는 수정하지 않았다. HWM admission과 Shutdown
terminal의 차이는 현재 Kotlin suite에서 우회하지 않고 blocker로 보고한다.
