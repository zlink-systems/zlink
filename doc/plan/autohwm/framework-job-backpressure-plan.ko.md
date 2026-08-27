# Framework job backpressure 후속 계획

> 이 문서는 Core 작업이 끝난 뒤 별도로 시작할 Framework 구현 계획이다. 현재 Framework
> 공개 계약을 정의하지 않는다. 설정, status와 metric 이름은 공통 spec과 모든 언어별 exact
> interface에서 확정하기 전에는 사용할 수 없다.

작업 전에 [HWM과 application backpressure 설계 의도](./00-hwm-backpressure-design-intent.ko.md)를
먼저 읽는다. 공통 문서는 전체 책임과 message 흐름을 설명하고, 이 문서는 Framework 후속
구현과 언어 parity 절차를 소유한다.

> **0.13.2 입력 계약** — Core queue가 complete message를 dequeue해 binding에 넘기면 Core byte
> HWM charge는 끝난다. Framework는 retained-credit lease를 사용하지 않는다. Core와 binding은
> 수용한 send의 HWM 대기·내부 재시도·operation별 `send_completion`을 소유하며 공개
> `send_ready` callback·event는 없다. Framework service-wire `SendReady` kind `12`는 별도 service
> control record라 유지한다.

## 1. 시작 조건과 범위

이 작업은 [Core byte HWM과 흐름 제어 작업](./core-byte-hwm-flow-control-plan.ko.md)의 기능,
binding parity와 승인된 spec 반영이 끝난 뒤에 시작한다. Core 계획에서 별도 성능 단계로 이관한
과거 `0.10.1` 회귀 비교 항목은 `DEFERRED` 상태를 그대로 인계하며 완료했다고 간주하지 않는다.
그 항목은 이 문서 §9.4의 Framework RUNNING·PAUSE/RESUME·liveness 3개 짧은 성능 case를
생략하는 근거가 아니다. Core와 Framework를 같은 변경 묶음으로 구현하거나 검증하지 않는다.

시작할 때 다음 Core 결과를 입력으로 받는다.

- Paired DEALER/ROUTER에 한정된 receive-flow C API와 binding signature
- `RUNNING`·`PAUSED`의 local acceptance, reconnect와 close 경쟁 계약
- Remote PAUSE와 byte HWM 재시도를 Core가 소유하고 binding operation completion으로 완료한 결과
- Flow transition event와 metric
- Flow state가 계속 `RUNNING`일 때의 Core 성능 상태와, 미완료 비교가 있으면 명시적인
  `DEFERRED` 인계

Framework는 host-shared application job capacity를 기준으로 pressure를 계산하고, 지원되는
paired socket에 Core receive-flow state를 설정한다. 첫 범위는 RouteMesh와 ClientServer처럼
paired DEALER/ROUTER를 사용하는 경로다. Classic fanout, PUB/SUB와 STREAM에는 Core remote
PAUSE를 적용하지 않고 기존 bounded queue와 transport backpressure를 유지한다.

Core HWM profile과 Framework Application Job Queue profile은 별도 public 설정이며 둘의 기본값은
각각 `Balanced`다. 같은 label을 사용해도 값·단위·계산·status를 공유하지 않는다. Framework가
Core에 주는 runtime feedback은 지원 socket의 `RUNNING`·`PAUSED` 절대 상태 하나뿐이다.

Framework heartbeat, topology, relocation과 기타 Framework control message는 data line에
남긴다. Core completion lane을 범용 Framework control channel로 사용하지 않는다.

### 1.1 새 작업자의 시작 gate

이 문서만으로 작업 순서와 변경 범위를 정할 수 있어야 하지만, 실제 계약과 repository 규칙은
아래 원본 문서에서 확인한다. 이전 대화 기록이나 Core 작업의 중간 구현을 입력으로 사용하지
않는다.

1. `git branch --show-current`와 `git status --short`를 확인하고 dirty worktree의 기존 변경을
   보존한다.
2. [Core 계획](./core-byte-hwm-flow-control-plan.ko.md)의 완료 보고에서 C API signature,
   지원 socket, lifecycle test, binding parity와 승인된 spec을 확인한다. 별도 이관한 paired perf
   항목은 `DEFERRED` 근거와 현재 측정 상태를 확인하고 Framework 결과와 섞지 않는다.
3. `scripts/local-package/README.ko.md`에 따라 검증된 `0.13.2` Core release와 local binding
   package를 준비한다. Framework가 stale package나 서로 다른 native runtime을 사용하면 작업을
   시작하지 않는다.
4. 공통 spec과 모든 언어 exact interface 변경 범위를 사용자에게 제시하고 protected path
   승인을 받은 뒤 문서를 수정한다.
5. 공통 계약을 먼저 고정한 다음 한 언어의 focused 구현과 test를 완료하고 나머지 언어로
   같은 관찰 결과를 전파한다.

Core 완료 보고에 하나라도 빠졌으면 Framework helper나 raw frame으로 우회하지 않는다.
부족한 계약을 `Core prerequisite gap`으로 보고하고 Core 작업으로 돌려보낸다.

### 1.2 필수 규칙과 계약 문서

| 구분 | 읽을 문서 | 확인할 내용 |
|---|---|---|
| 공통 설계 의도 | `doc/plan/autohwm/00-hwm-backpressure-design-intent.ko.md` | Core safety HWM, job pressure, completion lane과 data-line heartbeat |
| 저장소 규칙 | `AGENTS.md` | Branch, dirty worktree, 검증과 보호 경로 |
| Framework 규칙 | `framework/AGENTS.md` | 공통 계약, 언어 parity와 package 사용 |
| Framework 문서 규칙 | `framework/doc/AGENTS.md`, `doc/AGENTS.md` | Spec·guide·internal 문서 위치와 승인 |
| 문서 작성 원칙 | `doc/principal/documentation/documentation-principles.ko.md`, `spec-writing-guide.ko.md` | 현재 계약, exact interface와 test |
| Core·binding package | `scripts/local-package/README.ko.md` | Core release 검증, local binding package 생성과 provenance |
| Public governance | `framework/doc/framework/common/spec/server/00-foundation/01-public-contract-governance.ko.md` | 공통 계약과 언어별 표현 |
| 용어 | `01-glossary.ko.md` | Pressure, generation과 token 비교 규칙 |
| Async 결과 | `05-async-execution-policy.ko.md`, `32-framework-error-model.ko.md` | Admission, timeout, cancellation과 오류 owner |
| Framework API | `06-framework-api.ko.md` | 설정 owner, startup validation과 public surface |
| 관측 | `24-runtime-monitoring.ko.md`, `25-runtime-metrics.ko.md` | Snapshot, metric과 reset |
| Message flow | `26-message-flow-tracing.ko.md`, `27-flow-correlation.ko.md` | 기존 `backpressured` outcome과 tracing 비용 |
| Liveness | `29-transport-liveness.ko.md`, `49-internal-liveness-and-state.ko.md` | Topology별 progress 증거와 route 상태 |
| HWM/job 경계 | `33-core-hwm-application-job-flow.ko.md` | Core byte HWM과 Framework job permit owner |
| Dispatch | `42-internal-progress-isolation.ko.md`, `46-internal-dispatch-loop.ko.md` | Host queue, state evaluation과 shutdown |
| Wire 경계 | `51-internal-service-wire-protocol.ko.md`, `framework/runtime/protocol/service-wire-v1.schema.json` | Framework control이 data line에 남는 계약 |
| Exact interface | `framework/doc/framework/common/spec/server/languages/<lang>/interfaces/` | C++, .NET, Java, Kotlin과 Node.js의 실제 이름 |

위 `.ko.md`가 기준이고 같은 위치의 `.en.md`는 같은 작업에서 맞춘다. `01`처럼 상대 이름만
적힌 문서는 모두 `framework/doc/framework/common/spec/server/` 아래에 있다.

### 1.3 구현 탐색 지도

먼저 현재 job queue와 socket lifecycle owner를 찾고 같은 책임 위치에 pressure state를
추가한다. 언어별로 같은 class 이름을 강제하지 않고 공통 관찰 결과를 맞춘다.

| 언어 | Job capacity 시작점 | Socket·liveness 시작점 | 관측·test 시작점 |
|---|---|---|---|
| C++ | `framework/languages/cpp/framework/src/runtime/dispatch/application_job_queue.hpp`, `application_job_queue_capacity.hpp`, `host_capacity_runtime.hpp` | `runtime/mesh/raw_mesh_node_owner.*`, `runtime/client_server/raw_client_server_owner.*` | `runtime/diagnostics/monitoring_runtime.*`, `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_application_job_queue.cpp` |
| .NET | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Dispatch/ZLinkApplicationJobQueue.cs` | `Runtime/Service/ZLinkManagedMeshNode.cs`, `Runtime/Channels/ZLinkClientServerClientRuntime.cs` | `Runtime/Diagnostics/ZLinkRuntimeMetrics.cs`, `tests/Zlink.Framework.UnitTests/Runtime/ApplicationJobQueueTests.cs` |
| Java | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/dispatch/ZLinkApplicationJobQueue.java` | 같은 module의 `runtime/binding/ZLinkJavaRawMeshNode.java`, `runtime/channels/ZLinkChannelSocketRegistry.java` | 같은 module의 `runtime/metrics/ZLinkRuntimeMetrics.java`, `src/test/java/systems/zlink/framework/runtime/host/ZLinkApplicationJobQueueRuntimeMonitoringTest.java` |
| Kotlin | Java Core queue와 runtime을 사용한다. Kotlin 별도 queue를 만들지 않는다. | `framework/languages/java/zlink-framework-kotlin/`의 public extension만 확인한다. | `src/test/kotlin/systems/zlink/framework/kotlin/KotlinCapacityMonitoringContractTest.kt`, `src/contractTest/kotlin/systems/zlink/framework/kotlin/KotlinPublicSurfaceContractTest.kt` |
| Node.js | `framework/languages/node/packages/framework/src/runtime/host/application-job-queue.ts` | `runtime/channels/channel-socket-registry.ts`, `runtime/foundation/raw-service-mesh-runtime.ts` | `runtime/diagnostics/runtime-metrics.ts`, `framework/languages/node/test/` |

`node_modules`와 `dist`는 source owner가 아니다. Node.js는 `packages/framework/src`를 수정하고
정식 build로 generated output을 갱신한다. 한 언어에서 control frame을 직접 encode하거나
Core raw handle을 새로 노출하지 않는다.

### 1.4 Core release와 local binding 준비

배포된 Core `0.13.2` release와 같은 repository version에서 다음을 실행한다.

```bash
scripts/local-package/build-wsl.sh --sync-versions
scripts/local-package/build-wsl.sh --verify-versions
scripts/local-package/build-wsl.sh cpp dotnet java node
```

기본 release source mode로 checksum과 clean provenance를 검증한다. 아직 release되지 않은 Core
변경을 함께 검증할 때만 `--core-source local`을 사용하며, 최종 Framework gate는 배포된
`core/v0.13.2`의 revision·runtime hash를 가리키는 package로 다시 실행한다. Framework 언어별
중앙 version pin은 다음 위치가 소유한다.

- C++: `ZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION`을 선언한 CMake 설정
- .NET: `framework/languages/dotnet/Directory.Packages.props`
- Java/Kotlin: `framework/languages/java/gradle/libs.versions.toml`
- Node.js: `framework/languages/node/package.json`과 package별 `package.json`

Framework source에서 binding source directory를 직접 참조하지 않는다.

### 1.5 언어별 Core receive-flow binding API

Framework는 §4에서 지원 paired socket에 receive-flow state를 설정할 때 **각 언어 binding이
이미 노출한 API**를 호출한다. Core raw handle을 새로 노출하거나 control frame을 직접 encode하지
않는다. 정확한 signature와 값은 Core binding 산출물이 소유하며, 시작 게이트에서 provenance와
함께 확인한다(§1.1, §1.4).

| 언어 | receive-flow 설정 API | 상태 enum |
|---|---|---|
| C (기준) | `zlink_socket_set_receive_flow_state(handle, state)` | `zlink_receive_flow_state_t` {`ZLINK_RECEIVE_FLOW_RUNNING`=0, `ZLINK_RECEIVE_FLOW_PAUSED`=1} |
| C++ | `socket_t::set_receive_flow_state(receive_flow_state_t)` | `receive_flow_state_t` |
| .NET | `SocketBase.SetReceiveFlowState(ReceiveFlowState)` | `ReceiveFlowState` |
| Java | `CommonSocketOptions.receiveFlowState(ReceiveFlowState)` | `ReceiveFlowState` |
| Kotlin | Java binding의 `receiveFlowState(...)`를 그대로 사용 (별도 API를 만들지 않는다) | Java `ReceiveFlowState` |
| Node.js | socket의 `setReceiveFlowState(state)` | `ReceiveFlowState` 상수 |

이 호출은 **local receive-flow를 설정**해 remote send를 PAUSE시킨다. Flow 전이 관측(§7)은 socket
monitor event로 받는다 — `ZLINK_SOCKET_MONITOR_EVENT_SEND_FLOW_PAUSED`·`_RESUMED`·
`_FLOW_STATE_STALE`(각 언어 monitor·eventing surface의 대응 이름). 위 표의 이름이 Core 완료 보고와
다르면 우회 helper를 만들지 않고 `Core prerequisite gap`으로 보고한다(§1.1).

## 2. 책임 경계

| 주체 | 소유하는 결정 | 소유하지 않는 결정 |
|---|---|---|
| Application 개발자 | 성능 시험으로 job hard 상한과 필요하면 threshold override를 정한다. | Core flow frame, pair generation과 epoch를 만들거나 해석하지 않는다. |
| Framework | Pressure count, 80%/60% 상태 전이, Core 절대 상태 적용과 기존 topology liveness·route-ready 판정을 소유한다. | Core queued byte와 HWM 재시도·operation completion을 수정하지 않으며, PAUSE를 이유로 topology liveness나 route-ready 판정을 바꾸지 않는다. |
| Core | Local flow state를 paired connection에 동기화하고 remote PAUSE를 send admission에 적용한다. | Job 처리 능력이나 Framework queue 점유율을 판단하지 않는다. |

## 3. Pressure count와 threshold

### 3.1 정확한 분자

Hard 상한은 기존 `EffectiveMaxQueuedApplicationJobs`다. Pressure 분자는 현재 queue가 capacity
판정에 사용하는 `PermitsInUse`와 같아야 한다.

```text
pressure count
  = reserved supply permits
  + queued application jobs
```

Reserved permit이 queued 상태로 바뀌면 한 항목에서 다른 항목으로 이동할 뿐 pressure count는
변하지 않는다. Permit을 release하면 count가 감소한다. Capacity waiter는 아직 permit을 받지
않았으므로 분자에 포함하지 않는다. Handler가 permit을 반환한 뒤 실행 중인 상태도 분자에
포함하지 않는다.

문서, 상태 머신, status, metric과 test에서는 `queued jobs`라는 축약어 대신
`application job permits in use` 또는 확정된 하나의 이름을 사용한다.

### 3.2 Threshold 계산

```text
pause permit count  = ceil(effective max * pause percent / 100)
resume permit count = floor(effective max * resume percent / 100)
```

기본값은 pause 80%, resume 60%다. `resume < pause`를 startup에서 검증하고 runtime 중
자동 조정하지 않는다.

공통 설정 후보는 다음과 같다.

| 설정 후보 | 기본값 | 검증 후보 | 의미 |
|---|---:|---|---|
| `ApplicationJobQueuePauseThresholdPercent` | 80 | `1..100` | Pressure count가 계산된 pause count 이상이면 `PAUSED`로 전환한다. |
| `ApplicationJobQueueResumeThresholdPercent` | 60 | `0..99`, pause보다 작음 | Pressure count가 계산된 resume count 이하이면 `RUNNING`으로 전환한다. |

정확한 이름과 type은 공통 spec과 각 언어 exact interface가 소유한다. 별도 public
`Pause()`·`Resume()` operation은 제공하지 않는다.

## 4. 상태 전이와 socket 적용

Framework host runtime은 하나의 host-shared pressure state를 소유한다.

```text
RUNNING
  permits in use >= pause permit count -> PAUSED

PAUSED
  permits in use <= resume permit count -> RUNNING
```

60%와 80% 사이에서는 기존 상태를 유지한다. 같은 상태에서 count만 바뀌면 Core API를
반복 호출하지 않는다.

상태 전이 시 현재 host가 소유한 지원 paired socket마다 같은 receive-flow state를 한 번
설정한다. Socket 생성과 전이가 경쟁하면 socket 등록 뒤 현재 host state를 읽어 한 번
동기화한다. Socket close와 경쟁한 `invalid state`는 종료 중인 socket에서 기대할 수 있는
결과로 처리하고 새 socket에 현재 상태를 적용한다. 다른 config 오류는 숨기지 않고
diagnostic과 metric에 기록한다.

첫 범위는 host 전체다. Actor, Spot, service 또는 connection별 pressure scope는 포함하지
않는다. 특정 target만 선택적으로 멈추거나 data queue에서 특정 message만 꺼내는 기능도
추가하지 않는다.

## 5. Application send 계약

Remote PAUSE를 새 public terminal result로 노출하지 않는다. Core send가 backpressured이면
기존 operation family의 admission·timeout 규칙을 그대로 적용한다.

| Operation 경계 | 유지할 결과 |
|---|---|
| 첫 exact-target binding operation을 시작함 | Core가 HWM 재시도를 소유하고 operation별 completion을 완료한다. Framework는 별도 readiness waiter나 retry adapter를 만들지 않는다. Deadline·detach는 해당 operation의 terminal이며 다른 route로 replay하지 않는다. |
| One-way가 source-local admission을 이미 완료함 | PAUSE 때문에 취소하거나 자동 재전송하지 않고 기존 정상 완료를 유지한다. |
| Request가 source가 소유한 local bounded resource를 얻지 못함 | 기존 `CapacityExceeded` 규칙을 유지한다. |
| Request가 remote route·target queue를 사용할 수 없음 | 기존 `Unavailable` 규칙을 유지한다. Remote queue 상태를 local `CapacityExceeded`로 바꾸지 않는다. |
| Request가 이미 제출된 뒤 reply deadline이 만료됨 | 기존 `DeadlineExceeded`로 한 번 완료하며 다른 route로 자동 replay하지 않는다. |
| Cancellation이나 shutdown이 admission보다 먼저 확정됨 | 기존 cancellation 또는 `ShuttingDown`으로 끝내고 late admission을 금지한다. |
| Nonblocking binding call | Core의 기존 `EAGAIN` 의미를 유지한다. |
| Multipart | 이미 시작한 message의 atomicity와 terminal 규칙을 유지한다. |

Deadline, cancellation, route timeout이 경쟁하면 기존 operation state machine의 첫 terminal만
유효하다. PAUSE가 terminal 우선순위를 바꾸지 않는다.

## 6. Framework control과 liveness

이 절은 서로 다른 세 전달 경로를 구분한다. 이 구분을 놓치면 liveness 계약을 잘못 읽는다 —
특히 remote PAUSE가 멀쩡한 연결을 죽은 것으로 만든다고 오해하게 된다.

| 경로 | 실어 나르는 것 | remote PAUSE의 영향 |
|---|---|---|
| Data line | application payload와 Framework control(heartbeat·topology·relocation)을 하나의 FIFO로 | Core의 operation completion을 지연시킬 수 있다 |
| Transport liveness | 연결 progress 증거인 `livenessAck` (§6.2 · `29`·`49`가 소유) | **영향받지 않는다.** PAUSE와 독립된 경로라 PAUSE 중에도 계속 오간다 |
| Core completion lane | Core가 내부 처리하는 flow state | Framework control 채널이 아니다 |

따라서 remote PAUSE는 **data line의 application 흐름만** 조절하고 transport liveness 경로는
건드리지 않는다. 연결을 not-ready로 바꿀지는 이 별도 경로의 `livenessAck`가 정하며(§6.2),
PAUSE 자체는 그 판정을 바꾸지 않는다. 아래 §6.1은 data line, §6.2는 transport liveness 경로를
설명한다.

### 6.1 Data-line FIFO

Framework heartbeat, topology, relocation과 기타 Framework control message는 application
message와 같은 data line의 FIFO 순서를 유지한다. Framework는 PAUSE 동안 heartbeat만
선택적으로 receive하지 않고, 앞선 application payload를 임시 side backlog로 옮기지 않는다.

Core completion lane에는 Core가 내부 처리하는 flow state만 둔다. Framework raw control
send/recv API와 remote PAUSE 우회 send API를 요구하지 않는다.

### 6.2 Topology별 progress 증거

`data-line progress`를 모든 topology에서 임의의 application message 수신으로 정의하지
않는다. 기존 liveness spec의 증거를 유지한다.

| Topology | Deadline을 갱신하는 증거 | 일반 application message |
|---|---|---|
| RouteMesh·ClientServer | 현재 physical connection이 기다리는 ID와 같은 첫 `livenessAck` | 진단용 마지막 수신 시각만 갱신하고 liveness deadline은 연장하지 않는다. |
| Classic fanout | 기존 application record 또는 publisher beacon 규칙 | 기존 fanout progress 규칙을 유지한다. |
| STREAM | 기존 stream session과 transport liveness 규칙 | 이번 remote PAUSE 범위 밖이다. |

PAUSE 상태도 기존 heartbeat timeout에서 제외하지 않는다. RouteMesh·ClientServer에서 올바른
ACK가 deadline 안에 도착하지 않으면 connection을 not-ready로 바꾸고 닫는다. TCP나 Core
completion lane이 유지됐다는 사실만으로 route를 available 상태에 두지 않는다.

Reconnect 뒤에는 기존 handshake와 matching liveness 증거를 다시 만족하면 route를 available
후보로 게시할 수 있다. 새 socket에는 게시 전에 현재 host pressure 절대 상태를 적용한다.
Pressure가 `PAUSED`라는 사실은 route ready나 liveness를 직접 바꾸지 않는다. Timeout으로 끝난 request와 이미 수락한 one-way를 자동 재전송하지
않는다.

별도 public max-pause timeout은 첫 계약에 추가하지 않는다. 기존 topology별 liveness deadline이
route 정리 시점을 소유한다.

## 7. 관측 계획

Runtime status 후보는 다음 값을 같은 snapshot에서 제공한다.

- Effective maximum application job permits
- Reserved supply permit count
- Queued application job count
- Current permits in use
- Current pressure state: `running` 또는 `paused`
- Pause와 resume permit count
- Current pause duration

Metric 후보는 다음과 같다.

- Pause/resume transition total
- Current·cumulative pause duration
- Core flow-state config failure total과 result별 count
- Topology별 liveness timeout total
- Pause 중 not-ready로 바뀐 connection과 unavailable route total

Message-flow tracing은 기존 `backpressured` outcome과 `backpressure` reason을 유지한다.
Liveness timeout terminal은 기존 `Unavailable` 분류와 연결한다. 새 string과 event는 tracing이
활성화된 경우에만 만들어 hot path의 off 비용을 늘리지 않는다.

## 8. 구현 단계

1. Core 완료 결과의 exact API, 지원 socket, lifecycle과 성능 기준을 다시 확인한다.
2. 공통 Framework spec과 모든 언어 exact interface에서 pressure count, threshold와 결과를
   확정한다.
3. Host job queue의 permit 변경과 같은 lock 경계에서 high/low state machine을 구현한다.
4. Paired socket registry와 Core receive-flow API 호출을 연결한다.
5. RouteMesh·ClientServer liveness timeout과 unavailable 전이를 연결한다.
6. Runtime status, metric과 message-flow reason을 추가한다.
7. C++, .NET, Java, Kotlin과 Node.js contract test를 같은 관찰 결과로 맞춘다.

Core source와 Core byte-HWM 구조를 이 단계에서 다시 변경하지 않는다. Core 계약 부족이
발견되면 Framework 우회 helper를 추가하지 않고 Core 변경 요청으로 분리한다.

### 8.1 단계별 산출물

| 단계 | 변경 owner | 산출물 | 다음 단계 조건 |
|---|---|---|---|
| 계약 고정 | 공통 spec과 exact interface | Pressure count, setting, status, terminal과 topology 범위 | 한국어·영어와 5개 언어 interface가 같은 관찰 결과를 정의함 |
| Reference 구현 | 한 언어의 host queue·socket registry | 80%/60% state machine과 Core API adapter | Reserved·queued·release focused test 통과 |
| Lifecycle | RouteMesh·ClientServer owner | New socket sync, close race, reconnect와 unavailable | Lifecycle와 liveness focused test 통과 |
| 관측 | Runtime monitoring·metrics·flow | Snapshot, transition, duration과 error reason | Tracing off 비용과 reset test 통과 |
| 언어 parity | C++, .NET, Java/Kotlin, Node.js owner | 같은 public 설정·status·terminal | 언어별 contract test 통과 |
| Cross-language | 기존 cross-language harness | 서로 다른 언어 peer의 PAUSE/RESUME과 reconnect | Canonical binding API만 사용한 smoke 통과 |

Reference 언어는 임의의 새 계약을 만들기 위한 기준이 아니다. 공통 spec을 가장 직접적으로
검증할 수 있는 언어를 선택하고, 구현 중 발견한 계약 gap은 먼저 공통 spec 판단으로 돌린다.

### 8.2 상태 변경의 동시성 경계

각 언어 구현은 다음 순서를 보장한다.

1. Job permit 변경과 새 pressure state 계산을 queue owner의 기존 synchronization 안에서
   원자적으로 수행한다.
2. 동일 상태이면 Core 호출 목록을 만들지 않는다.
3. 상태가 바뀌면 새 state와 monotonic local transition sequence를 저장한다.
4. Core API 호출은 user handler를 실행하는 lock 안에서 blocking하지 않는다. Socket snapshot을
   얻은 뒤 각 socket에 절대 상태를 적용한다.
5. Socket 등록은 현재 state와 transition sequence를 읽어 stale 적용을 피한다.
6. Close 경쟁의 `invalid state`는 종료 socket에만 허용하고 다른 config result는 진단한다.

구체적인 lock type과 scheduler는 언어별 구현이 소유하지만 위 관찰 순서는 같아야 한다.

## 9. 검증 계획

### 9.1 Pressure state

- Effective maximum이 10이면 permits in use가 8이 될 때 PAUSE를 한 번 설정한다.
- Reserved permit만으로 8에 도달해도 같은 PAUSE가 발생한다.
- Reserved가 queued로 바뀌어 합계가 유지되면 중복 PAUSE를 만들지 않는다.
- Permits in use가 6 이하가 될 때 RUNNING을 한 번 설정한다.
- 7에서는 이전 상태를 유지해 hysteresis 구간에서 진동하지 않는다.
- Capacity waiter는 permit을 받기 전까지 pressure count에 포함하지 않는다.
- Shutdown에서 마지막 RUNNING 전달을 기다리느라 종료가 무기한 지연되지 않는다.

### 9.2 Socket과 send 결과

- 현재 `PAUSED` 상태에서 새 paired socket을 등록하면 PAUSED를 한 번 적용한다.
- Close와 설정이 경쟁해도 종료 socket의 `invalid state`를 정상 lifecycle로 분류한다.
- Unsupported PUB/SUB·STREAM socket에는 API를 호출하지 않는다.
- One-way, request, cancellation, deadline과 shutdown 결과가 §5 표와 같다.
- 이미 제출한 request와 one-way를 PAUSE나 timeout 때문에 자동 replay하지 않는다.
- Multipart와 nonblocking binding 결과가 기존 계약과 같다.

### 9.3 Liveness와 FIFO

- PAUSED 중 heartbeat를 data queue에서 선택적으로 receive하지 않는다.
- RouteMesh·ClientServer 일반 application message는 matching ACK deadline을 연장하지 않는다.
- Matching ACK가 없으면 기존 deadline에서 not-ready와 route unavailable로 전환한다.
- Classic fanout은 기존 record·beacon progress 규칙을 유지한다.
- Reconnect가 handshake와 liveness 조건을 만족하고 현재 local pressure 절대 상태를 socket에
  적용하기 전에 route를 available로 게시하지 않는다. Pressure 값 자체는 ready 조건이 아니다.

### 9.4 성능

Framework 성능도 전체 언어·topology matrix를 한 번에 실행하지 않는다. 언어 하나,
RouteMesh 또는 ClientServer 하나, transport 하나, message size 하나씩 짧게 실행한다.

1. Flow state가 계속 `RUNNING`인 기준 case
2. 80%에서 PAUSE 한 번, 60%에서 RUNNING 한 번 발생하는 case
3. 지속 PAUSE 뒤 기존 liveness timeout으로 route가 제외되는 case

각 case에서 job throughput, enqueue-to-handler latency, Core API 호출 횟수와 transition latency를
기록한다. 첫 회귀 case에서 원인을 해결한 뒤 다음 언어와 transport로 확장한다.

### 9.5 언어별 focused gate 명령

먼저 변경한 언어의 관련 test만 실행한다. 각 언어가 통과한 뒤 최종 parity gate로 넓힌다.

```bash
# C++ configure/build/focused unit label
cd framework/languages/cpp
cmake --preset linux-ninja-debug
cmake --build --preset linux-ninja-debug --parallel 2
ctest --preset linux-ninja-debug -L framework-unit --output-on-failure

# .NET unit project만 실행한다. 전체 solution은 실행하지 않는다.
cd framework/languages/dotnet
dotnet test tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj

# Java Core와 Kotlin public surface
cd framework/languages/java
./gradlew --no-daemon \
  :zlink-framework-core:test \
  :zlink-framework-kotlin:test \
  :zlink-framework-kotlin:contractTest

# Node.js build·typecheck·lint·runtime gate
cd framework/languages/node
npm test
```

C++ ctest의 WSL SIGABRT 86/134는 한 번만 재실행하고 두 번째 결과를 보고한다. 다른 실패는
간헐 실패로 가정하지 않는다. Java source를 바꾼 뒤 cross-language gate를 실행할 때는 먼저
다음을 실행한다.

```bash
cd framework/languages/java
./gradlew --no-daemon -p cross-language :Host:installDist

cd ../cpp
./cross-language/run_cross_language_smoke.sh
```

간헐 실패는 임시 logging보다 기존 message-flow와 file log를 먼저 켠다. Application dispatch는
`26-message-flow-tracing`을 사용하고 RouteMesh control·liveness는 언어별 transport trace와
`29`·`49`의 transition을 확인한다.

## 10. 문서 변경 계획

이 절의 문서는 Framework 후속 작업에서만 변경한다. Core 단계에서는 수정하지 않는다.
한국어와 영어 mirror를 같은 작업에서 변경한다.

### 10.1 공통 spec과 guide

| 문서 | 변경할 계약 |
|---|---|
| `01-glossary.*.md` | Core byte HWM, Framework job pressure와 topology liveness의 책임을 구분한다. |
| `05-async-execution-policy.*.md` | Operation family별 PAUSE 대기, admission과 terminal 결과를 명시한다. |
| `06-framework-api.*.md` | Threshold 설정, 기본값, 범위, 반올림과 startup validation을 명시한다. |
| `24-runtime-monitoring.*.md`, `25-runtime-metrics.*.md` | Permits in use, pressure state, transition·duration과 liveness 결과를 명시한다. |
| `29-transport-liveness.*.md` | Topology별 progress 증거, PAUSE 중 timeout과 recovery 조건을 명시한다. |
| `33-core-hwm-application-job-flow.*.md` | Core charge가 dequeue에서 끝나는 경계, job permit, PAUSE 전파와 Core-owned send completion을 명시한다. Retained-credit lease를 Framework 경계에 복구하지 않는다. |
| `42-internal-progress-isolation.*.md` | 80%/60% 상태 전이, Core API 호출 owner와 data-line FIFO를 명시한다. |
| `46-internal-dispatch-loop.*.md` | Permit 변경, 상태 평가, 중복 방지와 shutdown 순서를 명시한다. |
| `51-internal-service-wire-protocol.*.md` | Framework control은 data line에 남고 Core flow frame은 Framework wire message가 아님을 명시한다. |
| `README.*.md` | Backpressure 관련 책임 문서의 색인을 갱신한다. |
| `guide/server/04-backpressure.*.md`, `12-operations.*.md` | Application이 hard 상한과 threshold를 정하는 방법 및 timeout 결과를 설명한다. |

### 10.2 언어별 exact interface

- C++: `languages/cpp/interfaces/02-configuration-host.*.md`,
  `languages/cpp/interfaces/08-monitoring.*.md`
- .NET: `languages/dotnet/interfaces/03-configuration-topology.*.md`,
  `languages/dotnet/interfaces/10-topology-monitoring.*.md`
- Java: `languages/java/interfaces/configuration-host.*.md`,
  `languages/java/interfaces/monitoring.*.md`
- Kotlin: `languages/kotlin/interfaces/configuration-host.*.md`,
  `languages/kotlin/interfaces/monitoring.*.md`
- Node.js: `languages/node/interfaces/01-foundation-configuration.*.md`,
  `languages/node/interfaces/03-location-observability.*.md`

정확한 property 이름, integer type, validation error와 snapshot field를 언어별 문서에
선언한다. 공통 pseudocode를 실제 signature처럼 복사하지 않는다.

Framework 공통 spec과 exact interface는 보호 경로다. 후속 작업을 시작할 때 사용자의 해당
경로 변경 승인을 다시 확인하고 `framework/AGENTS.md`와 하위 문서 규칙을 적용한다.

## 11. 중단 조건과 결과 인계

다음 조건에서는 우회 구현을 만들지 않고 작업을 중단해 근거를 보고한다.

- Core API signature, supported socket 또는 lifecycle 결과가 Core 완료 보고와 다름
- Local Framework가 staged local Core·binding 대신 release 또는 stale runtime을 사용함
- Protected Framework spec과 exact interface 변경 승인이 없음
- 공통 contract를 한 언어 public API로 표현할 수 없어 사용자 차이가 발생함
- RouteMesh·ClientServer 이외 topology 지원이 첫 범위에 필요해짐
- Heartbeat를 선택적으로 receive하거나 Framework control을 completion lane으로 옮겨야만
  구현할 수 있음
- 세 번 같은 liveness·shutdown 실패가 재현되고 Core와 Framework owner를 분리할 수 없음

완료 또는 중단 보고 형식은 다음과 같다.

```text
Core prerequisite revision and package provenance:
Changed common contract:
Changed exact interfaces:
Implemented languages:
Focused tests:
Cross-language tests:
Short performance reports:
First remaining failure:
Core source changed during Framework phase: no
```

## 12. 완료 조건

- Pressure count가 `reserved supply permits + queued application jobs`로 모든 구현·문서·test에
  동일하게 적용된다.
- 기본 80%/60% hysteresis와 startup validation이 모든 언어에서 같다.
- 지원 paired socket에만 Core flow API를 호출하고 unsupported topology fallback을 유지한다.
- Application send, request, deadline, cancellation, shutdown과 multipart 결과가 바뀌지 않는다.
- Framework control과 heartbeat는 data-line FIFO를 유지한다.
- RouteMesh·ClientServer와 Classic fanout의 liveness 증거를 혼합하지 않는다.
- Status, metric과 message-flow tracing에서 pressure와 timeout을 관측할 수 있다.
- Framework 언어별 contract test와 짧은 case별 성능 검증이 통과한다.
- Core source와 byte-HWM 구조를 Framework 편의 때문에 다시 변경하지 않는다.
- `git diff --check`, 문서 link 검사와 관련 문서 생성 검사가 통과한다.

## 13. 진행 checklist

완료한 항목만 `[x]`로 바꾸고 Evidence 열에 Core handoff, package provenance, test output 또는
변경 파일 경로를 기록한다. 막힌 항목은 `[ ]`를 유지하고 `BLOCKED:`와 owner를 적는다.

### 13.1 시작 gate

| Done | 확인 항목 | Evidence |
|---|---|---|
| [x] | 공통 설계 의도와 Core·Framework 계획을 읽었다. | `00-hwm-backpressure-design-intent.ko.md`, Core·Framework 계획 대조 |
| [x] | Core 기능·binding·spec final checklist가 완료됐고 별도 paired perf 항목의 `DEFERRED` 이관 상태를 확인했다. | `core/v0.13.2`; Core 계획의 paired perf `DEFERRED` 유지 |
| [x] | Core C API·binding signature, 지원 socket과 lifecycle 결과를 받았다. | §1.5의 0.13.2 binding API와 paired DEALER/ROUTER 범위 확인 |
| [x] | Core paired perf 항목의 `DEFERRED` 근거와 완료 revision을 확인했다. | Core revision `9cff16f3a4a24624390bdc6e2d3623b70e364fc3` |
| [x] | Local Core·C++·.NET·Java·Node binding package provenance를 확인했다. | `build-wsl.sh --verify-versions` 통과; runtime SHA-256 `48f4d928110614ef8edc6c421583828af1e31ee479bea0af50ee804634c90df7` |
| [x] | Framework 보호 문서 변경 범위를 승인받았다. | 사용자 승인: spec·guide에 HWM 의도와 책임 경계 반영 |

### 13.2 공통 계약

| Done | 확인 항목 | Evidence |
|---|---|---|
| [x] | Pressure count를 reserved supply permits와 queued application jobs의 합으로 확정했다. | 공통 spec `33`, 언어별 queue test |
| [x] | 80% PAUSE, 60% RUNNING과 반올림·validation을 확정했다. | 공통 spec `06`·`33`, 5개 언어 configuration/queue test |
| [x] | 첫 topology를 paired RouteMesh·ClientServer로 제한했다. | 공통 spec `33`, unsupported topology test |
| [x] | Operation family별 admission, timeout, cancellation과 terminal 표를 확정했다. | 공통 spec `05`·`33`, guide `04`·`12` |
| [x] | RouteMesh·ClientServer, Classic fanout과 STREAM liveness 증거를 구분했다. | 공통 spec `29`·`33` |
| [x] | 공통 한국어·영어 spec과 5개 언어 exact interface를 함께 갱신했다. | common spec/guide와 C++·.NET·Java·Kotlin·Node interface mirror |

### 13.3 구현과 언어 parity

| Done | 확인 항목 | Evidence |
|---|---|---|
| [x] | Reference 언어의 permit state machine과 hysteresis test가 통과했다. | C++ 18/18 및 기존 focused 39/39; Java/Node queue test 통과 |
| [x] | New socket sync, close race, reconnect와 shutdown test가 통과했다. | 언어별 receive-flow lifecycle focused test 통과 |
| [x] | Framework control과 heartbeat가 data-line FIFO에 남아 있다. | service-wire `SendReady` kind 12 유지; Core completion과 분리된 test/spec |
| [x] | Status, metric과 message-flow tracing의 off-cost test가 통과했다. | .NET 관련 96/96, Node queue·metrics 24/24, Java monitoring test 통과 |
| [x] | C++ focused unit·contract test가 통과했다. | HWM/queue 18/18; 전체 focused 39/39 |
| [ ] | .NET focused unit·contract test가 통과했다. | unit 1874/1874·HWM 필터 96/96 통과; contract 73/77, 기존 source-layout/API snapshot 4건 실패 |
| [x] | Java와 Kotlin focused unit·contract test가 통과했다. | Java core 1132/1133 후 유일 실패 격리 재실행 통과; Kotlin test 통과 |
| [ ] | Node.js build·typecheck·lint·runtime gate가 통과했다. | BLOCKED: build/typecheck와 HWM 47/47 통과, 기존 lint 오류 142건 및 diagnostics 계약 10건 실패 |
| [x] | Cross-language PAUSE/RESUME과 reconnect smoke가 통과했다. | `run_cross_language_smoke.sh all` 통과, 0.13.2 package 사용 |

### 13.4 최종 Framework gate

| Done | 확인 항목 | Evidence |
|---|---|---|
| [ ] | RUNNING, 단일 PAUSE/RESUME과 liveness timeout 성능 case를 짧게 비교했다. | BLOCKED: 기능 test는 있으나 §9.4의 throughput·latency 3-case 측정 결과가 아직 없음 |
| [x] | Application send, request, cancellation, shutdown과 multipart 결과가 기존과 같다. | 언어별 focused unit 및 cross-language smoke |
| [x] | Unsupported PUB/SUB·Classic fanout·STREAM fallback이 유지된다. | unsupported topology focused test와 source audit |
| [x] | 공개 spec, exact interface, guide와 구현이 일치한다. | 5개 언어 exact-interface parity audit |
| [ ] | `git diff --check`, link 검사와 관련 문서 생성 검사가 통과했다. | `git diff --check` 통과; BLOCKED: 기존 unchanged broken link 8건(전체 70건) |
| [ ] | 완료 보고에 Core provenance, 언어별 test, perf report와 남은 실패를 기록했다. | 최종 완료 보고 시 갱신 |
| [x] | Framework 단계에서 Core source와 byte-HWM 구조를 변경하지 않았다. | Framework phase에서 Core source diff 없음 |
