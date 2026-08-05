# Node.js SubmitAdmission E2E feature map

기준 문서는
[`Config 13 — One-way submit admission`](../../../../doc/framework/common/e2e/config-13-submit-admission.ko.md)이다.
Runner는 `Caller`, `MeshTarget`, `FanoutPublisher`를 서로 다른 process로 실행한다. `ZLINK_NODE_FRAMEWORK_PACKAGE_ROOT`를
지정하면 그 격리 workspace에 설치된 Node binding package와 그 workspace에서 build한 Framework package를
사용한다.

`부분 구현`은 public 결과와 process 순서 일부를 검증하지만 공통 scenario가 요구하는 gate나 내부 observer를
모두 제공하지 못한다는 뜻이다. 이 상태는 해당 scenario의 완료 증거가 아니다. `미구현` selector를 직접
지정하면 runner는 성공으로 건너뛰지 않고 종료 코드 3을 반환한다.

| 시나리오 | 상태 | 근거 또는 blocker |
|---|---|---|
| SA-E2E-01 | 부분 구현 | Ready 상태인 remote Node direct와 ChannelName call이 public `submit()` 한 번으로 `Submitted`가 되고 target handler가 각각 한 번 처리하는지 process에서 확인한다. Local family와 scheduler enqueue·transport attempt·commit observer는 없다. |
| SA-E2E-02 | 미구현 | TCP read를 중단하는 `ReceiverGate`와 send-ready signal·retry attempt observer가 없다. 반복 submit으로 대체하지 않는다. |
| SA-E2E-03 | 미구현 | Pending capacity marker와 최초 transport attempt observer를 process topology에 연결해야 한다. |
| SA-E2E-04 | 미구현 | Deterministic pending gate와 late admission observer가 없다. Timeout을 늘려 통과시키지 않는다. |
| SA-E2E-05 | 부분 구현 | Manual expected-RID registry에 없는 RID를 100회 `TargetNotFound`로 분류하고, 같은 registry에 있는 target process를 종료해 route-ready가 해제된 뒤 100회 `RouteNotConnected`로 분류한다. 각 operation의 public call과 terminal은 한 번이다. 독립 connection gate와 native teardown observer는 아직 없다. |
| SA-E2E-06 | 미구현 | Source admission barrier와 source 종료 뒤에도 terminal을 보관하는 독립 `EvidenceCollector`가 없다. |
| SA-E2E-07 | 미구현 | Process topology에 AbortSignal winner와 Logical Multicast commit 전·후 barrier를 연결해야 한다. |
| SA-E2E-08 | 부분 구현 | Self RID direct를 기존 node-direct dispatcher에 local admission으로 전달한다. Local·remote 즉시 수락은 각각 `Submitted`이고 각 process의 handler가 한 번 처리한다. Pending·deadline과 transport observer 비교는 아직 없다. |
| SA-E2E-09 | 부분 구현 | ChannelName call이 positive-weight target에서 한 번 처리되는지 확인한다. Pending deadline과 선택 member observer가 없다. |
| SA-E2E-10 | 미구현 | ClientServer 역할 process와 DEALER admission evidence가 없다. RouteMesh Channel로 대체하지 않는다. |
| SA-E2E-11 | 미구현 | Spot generation, stale handle과 local·remote mailbox gate를 같은 runner에 연결해야 한다. |
| SA-E2E-12 | 미구현 | Actor generation, stale handle과 local·remote mailbox gate를 같은 runner에 연결해야 한다. |
| SA-E2E-13 | 미구현 | Binding async publish는 구현됐지만 process runner에는 executor handoff observer와 `PublishSnapshotBarrier`가 없다. Focused contract test를 process 완료 증거로 바꾸지 않는다. |
| SA-E2E-14 | 구현 | Subscriber process가 없는 publisher가 classic fanout public call을 한 번 submit하고 `Submitted` terminal 한 번을 반환한다. |
| SA-E2E-15 | 미구현 | Bound session·session Actor relay process와 binding generation·mailbox gate evidence가 없다. |
| SA-E2E-16 | 미구현 | Server STREAM session과 실제 connector peer의 wire ordering evidence가 없다. |
| SA-E2E-17 | 미구현 | 실제 request reply token, concurrent terminator barrier와 token claim observer가 없다. |
| SA-E2E-18 | 미구현 | Direct target generation과 select-one의 attempt별 eligible member observer가 없다. |
| SA-E2E-19 | 미구현 | Terminal 뒤 connection generation과 late transport attempt를 함께 기록하는 topology가 없다. |
| SA-E2E-20 | 부분 구현 | Remote Node handler gate를 닫은 상태에서 submit terminal이 먼저 `Submitted`가 되고 handler 완료는 gate 해제 뒤 한 번인지 확인한다. 나머지 family matrix는 없다. |
| SA-REG-01 | 구현 | Package-mode Framework declaration에서 `submit()` positive compile과 제거된 동기 one-shot terminator compile-negative를 실행하고 공통 verifier를 확인한다. |
| SA-REG-02 | 구현 | Async publisher·Logical Multicast commit boundary focused test와 고정 allowlist verifier로 Core call 1회, commit 전 abort Core 0회, commit 후 최종 결과 보존을 확인한다. |
| SA-REG-03 | 해당 없음 | Kotlin 전용 result 보존 scenario다. |
| SA-REG-04 | 미구현 | Process disposal과 send-ready event를 같은 barrier에서 발생시키고 waiter·reservation·callback 수를 확인할 observer가 없다. |

## 실행

```bash
ZLINK_NODE_FRAMEWORK_PACKAGE_ROOT=/path/to/candidate/framework-node \
  ./framework/languages/node/e2e/SubmitAdmission/run_e2e.sh all
```

`all`은 실제 assertion과 evidence가 있는 scenario만 실행한다. 미구현 selector는 종료 코드 3을 반환한다.
