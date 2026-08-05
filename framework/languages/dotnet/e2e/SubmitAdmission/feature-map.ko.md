# .NET SubmitAdmission E2E feature map

기준 문서는
[`Config 13 — One-way submit admission`](../../../../doc/framework/common/e2e/config-13-submit-admission.ko.md)이다.
이 runner는 `Caller`, `MeshTarget`, `FanoutPublisher`를 서로 다른 process로 실행한다. Application evidence는
공통 `OperationEvidenceStore`가 public invocation, terminal과 handler 진입·완료를 operation ID별로 기록한다.

`부분 구현`은 해당 시나리오의 일부 family나 process 순서만 검증했거나 Config 13이 요구하는 internal counter
전체를 아직 수집하지 못했다는 뜻이다. 이 상태는 시나리오 완료 증거가 아니다.

| 시나리오 | 상태 | 근거 또는 blocker |
|---|---|---|
| SA-E2E-01 | 부분 구현 | RID-direct와 ChannelName ready route가 `Submitted`이며 public invocation·terminal이 각각 한 번임을 실제 process에서 확인한다. Scheduler enqueue, transport attempt와 commit observer가 없으므로 전체 gate는 열지 않는다. |
| SA-E2E-02 | 부분 구현 | RID-direct에서 sender HWM과 pending capacity를 1로 적용했다. Linux x64 manifest가 socket buffer 요청 4096 bytes의 실제 적용값 8192 bytes를 고정하며, `ReceiverGate`를 닫은 뒤 32 KiB public submit을 최대 64개까지 한 번씩 시작한다. Pending operation은 gate를 열기 전 추가 attempt가 없고, Core `SEND_READY` 1회에 retry 1회가 대응하여 transport attempt 2회·commit 1회·terminal `Submitted` 1회로 끝난다. Polling submit, application retry와 timeout 변경은 없다. 다른 remote family와 local mailbox family가 남아 있으므로 전체 완료로 판정하지 않는다. |
| SA-E2E-03 | 부분 구현 | RID-direct의 실제 sender HWM에서 pending capacity 1을 사용한다. 첫 operation이 pending인 동안 다른 ID의 public submit은 transport attempt 1회 뒤 waiter를 만들지 않고 `Backpressured`로 끝나며 commit은 0이다. 다른 family의 capacity 검증이 남아 있으므로 전체 완료로 판정하지 않는다. |
| SA-E2E-04 | 미구현 | Deterministic pending gate와 late admission observer가 없다. Timeout을 늘리거나 반복 submit하는 방식으로 대체하지 않는다. |
| SA-E2E-05 | 부분 구현 | Manual expected-RID registry에서 알 수 없는 RID는 `TargetNotFound`, 등록 뒤 public disconnect가 끝난 RID는 `RouteNotConnected`다. Native route teardown monitor marker는 아직 수집하지 않으므로 전체 완료로 판정하지 않는다. |
| SA-E2E-06 | 미구현 | Source admission barrier와 독립 `EvidenceCollector` process, admission-closed marker가 필요하다. |
| SA-E2E-07 | 부분 구현 | 유효 call의 pre-cancel은 `OperationCanceledException` terminal 한 번이며 null message validation은 pre-cancel보다 먼저 실패한다. Logical Multicast commit 전·후 barrier가 남아 있다. |
| SA-E2E-08 | 부분 구현 | Local·remote RID-direct가 모두 `Submitted`이고 동일한 metadata와 handler dispatch pipeline으로 각각 한 번 처리된다. 별도 Object Client와 manual Ready connection을 만든 뒤 Node-direct Send·Request가 모두 `NotFound`이고 peer 수와 Ready peer 수가 변하지 않음을 Linux actual-process에서 확인했다. 증거는 `logs/20260728-154231-1164570/`에 있다. Pending·deadline의 transport attempt·commit observer는 남아 있다. |
| SA-E2E-09 | 부분 구현 | ChannelName ready route가 public invocation·terminal 한 번의 `Submitted`임을 process topology에서 확인한다. Pending deadline·member reselection evidence는 아직 없다. |
| SA-E2E-10 | source 구현·process 미검증 | .NET에 reviewed ClientServer Channel public builder와 runtime route가 source·contract·unit test에 존재한다. 실제 three-process selector와 terminal/evidence 검증은 아직 없다. RouteMesh helper로 대체하지 않는다. |
| SA-E2E-11 | 미구현 | Spot create·handle generation과 local mailbox gate를 같은 runner에 아직 연결하지 않았다. |
| SA-E2E-12 | 미구현 | Actor owner·handle generation과 local mailbox gate를 같은 runner에 아직 연결하지 않았다. |
| SA-E2E-13 | 미구현 | Logical Multicast executor direct handoff observer와 `PublishSnapshotBarrier`가 없다. 별도 publish transport나 raw frame으로 대체하지 않는다. |
| SA-E2E-14 | 구현 | Subscriber process를 만들지 않은 publisher가 public fanout call을 한 번 submit하고 `Submitted` terminal 한 번을 반환한다. |
| SA-E2E-15 | 미구현 | Bound session·session Actor relay 역할과 local·remote gate, binding generation evidence가 아직 없다. |
| SA-E2E-16 | 미구현 | Server STREAM session과 실제 connector peer, wire ordering evidence가 아직 없다. |
| SA-E2E-17 | 미구현 | 실제 server request의 reply token, 동시 terminator barrier와 token claim observer가 필요하다. |
| SA-E2E-18 | 미구현 | Direct target generation과 select-one first-attempt member를 operation별로 기록하는 observer가 없다. |
| SA-E2E-19 | 미구현 | Timeout·shutdown terminal 이후 connection generation과 late transport attempt를 함께 기록하는 observer가 없다. |
| SA-E2E-20 | 부분 구현 | RID-direct handler gate를 닫은 상태에서 submit `Submitted` terminal이 먼저 완료되고 handler 완료는 gate를 연 뒤 한 번 기록됨을 확인한다. 나머지 family matrix가 남아 있다. |
| SA-REG-01 | 구현 | Public contract·sample·E2E C# source의 제거 이름 no-hit와 제거된 동기 terminator의 compile-negative fixture를 실행한다. |
| SA-REG-02 | 구현 | Internal primitive allowlist 검증과 `ZLinkAsyncSubmitterTests`를 실행하여 public 제거 뒤에도 non-blocking first attempt·send-ready primitive를 유지한다. |
| SA-REG-03 | 해당 없음 | Kotlin 전용 시나리오다. JVM feature map과 runner가 소유한다. |
| SA-REG-04 | 부분 구현 | RID-direct pending operation에서 host stop 2회와 gate open을 경쟁시켜 terminal·cleanup이 각각 한 번이고 waiter·reservation·callback final count가 모두 0이며 process exit code가 0임을 확인한다. Internal primitive test는 writable callback과 double dispose를 경쟁시켜 dispose가 이긴 경우에도 cleanup 1회와 resource 0을 검증한다. 다른 family와 `SA-E2E-17.c` 100회 반복이 남아 있으므로 전체 완료로 판정하지 않는다. |

## 실행

```bash
# 구현된 process scenario와 .NET 회귀를 실행한다.
./framework/languages/dotnet/e2e/SubmitAdmission/run_e2e.sh all

# 하나의 scenario만 실행한다.
./framework/languages/dotnet/e2e/SubmitAdmission/run_e2e.sh SA-E2E-20

# 격리한 binding package만 resolve하고 package 안의 Core runtime을 대조한다.
ZLINK_SUBMIT_ADMISSION_PACKAGE_ROOT=/tmp/zlink-dotnet-candidate \
ZLINK_SUBMIT_ADMISSION_CORE_RUNTIME="$PWD/core/build/lib/libzlink.so" \
  ./framework/languages/dotnet/e2e/SubmitAdmission/run_e2e.sh all
```

Runner의 `all`은 기본 process matrix와 `SA-REG-01~02`를 실행한다. `ReceiverGate`를 사용하는
`SA-E2E-02`, `SA-E2E-03`과 `SA-REG-04`는 개별 selector로 실행한다.
미구현 selector를 직접 지정하면 성공으로 건너뛰지 않고 실패한다.
Candidate mode에서는 `Systems.Zlink`를 지정한 NuGet 디렉터리에서만 resolve한다. Runner는 package와
NuGet cache의 hash가 같은지 확인하고, package에서 복원한 native library, server output과 지정한 Core
runtime의 SHA-256·Build ID를 `candidate-package.evidence.log`에 기록한다.
이 mode는 같은 process scenario를 package consumer로 다시 실행할 뿐, 아직 없는 counter나 barrier를
대체하지 않는다. 따라서 위 표의 `부분 구현` 상태는 package 검증이 통과해도 그대로 유지한다.
