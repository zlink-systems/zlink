<!-- framework-adapter-nav:start -->
[문서 목록](../../../README.ko.md) | [이전: Backend Policy](backend-dependency-policy.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)<!-- framework-adapter-nav:end -->

[공통 layering](../../common/internals/01-layering.ko.md) | [공통 spec](../../common/spec/README.ko.md)

# .NET Runtime Integration과 Receive Ownership

이 문서는 `.NET Framework` runtime이 `bindings/dotnet`의 public API를 사용하는 위치와,
그 사이에 남겨야 하는 semantic adapter의 책임을 고정한다. 독자는 각 adapter가 단순한
인자 전달인지, Framework와 binding의 의미·소유권·수명·동시성을 실제로 변환하는지 이
문서만으로 판단할 수 있어야 한다.

이 문서는 Framework public contract를 추가하지 않는다. Framework public contract는
[공통 spec](../../common/spec/README.ko.md)과 .NET exact interface 문서가 소유한다.

## 1. 계층 기준

`.NET` 구현은 다음 책임 그래프를 따른다. `Framework public/domain contract`와
`Framework semantic runtime core`는 binding type과 binding option을 알지 않는다.
binding type을 호출하는 코드는 마지막 두 계층의 경계 안에만 둔다.

```text
+----------------------------------------------+
| Framework public/domain contract             |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Framework semantic runtime core              |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Binding-facing runtime integration           |
| direct public call or semantic adapter       |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Systems.Zlink public API                     |
+----------------------------------------------+
                       |
                       v
+----------------------------------------------+
| Core                                         |
+----------------------------------------------+
```

binding operation과 Framework operation의 의미, ownership, lifecycle, readiness, error,
concurrency가 모두 같으면 binding public API를 직접 호출한다. 하나라도 다르면 차이를
호출부에 퍼뜨리지 않고 semantic adapter 또는 port에 둔다. 다른 언어의 class 이름이나
현재 구현은 이 선택의 근거가 아니다.

현재 source의 일부 파일명에 `Wrapper`가 남아 있지만, 아래 표에서 유지하는 type은
binding object를 그대로 노출하는 pass-through wrapper가 아니다. 각 type은 표에 적은
Framework 의미를 binding public API로 변환하거나, 하나의 Framework lifecycle을 여러
binding object로 조합한다.

## 2. 구현 분류

다음 표에서 `유지`는 semantic adapter 또는 port를 유지한다는 뜻이다. `직접 호출`은
binding-facing runtime integration에서 `Systems.Zlink` public API를 바로 사용한다는
뜻이다. `제거`는 동일한 입력을 전달하기만 하는 Framework 내부 표면을 없앤다는 뜻이다.

| Type/path | Binding operation | Semantic mismatch | Ownership/lifecycle | Hot-path cost | Decision |
|---|---|---|---|---|---|
| `ZLinkBackendContextWrapper` | `IContext` 생성·shutdown·dispose | Context identity와 Framework context 수명을 하나의 port로 묶는다. | Context가 만든 socket보다 먼저 닫히지 않도록 owner를 유지한다. | 생성·종료 경로만 사용한다. | semantic adapter 유지 |
| DEALER/ROUTER socket adapter | fluent `Send`·`Request`·`Reply` | Framework는 직접 operation과 `bool`/callback 결과를 사용하고 binding은 builder submit을 사용한다. | socket close와 raw operation의 실행 owner를 `_gate`로 관리한다. | send/request/receive/reply마다 gate가 있다. | semantic adapter 유지; gate 제거는 별도 동시성 증명이 필요하다. |
| DEALER/ROUTER receive | `Recv(Received, RecvFlags)` | 수신 결과를 새 객체로 반환하지 않고 호출자가 storage를 제공한다. | async queue가 보유한 동안 storage를 재사용하지 않는다. | receive마다 `Received.Create()`를 만들지 않는다. | caller-provided storage adapter 유지 |
| PUB/SUB socket adapter | fluent publish와 `Subscribe(TopicMessage, ...)` | Framework의 topic/message operation을 binding builder와 subscription result로 변환한다. | `TopicMessage`는 dispatch가 끝날 때까지 queue가 소유한다. | topic storage pool을 사용한다. | semantic adapter 유지 |
| monitor adapter | `MonitorEvent`, `ISocketMonitor.Recv` | native event·timeout·result를 Framework monitor event로 변환한다. | monitor dispose와 event callback 수명을 관리한다. | monitor array와 nonblocking poll storage를 재사용한다. | semantic adapter 유지 |
| socket poller | `IPoller.Poll(PollEvent[])` | binding event 배열을 Framework readiness flags로 바꾼다. | poller와 event array를 owner가 함께 dispose한다. | `PollEvent[1]`을 lifecycle 동안 재사용한다. | semantic port 유지 |
| `ZLinkBackendSpotNodeWrapper` / `ZLinkBackendSpotWrapper` | `IMeshNode`, `ISpot` 및 dispatch callback | binding mesh object를 Spot, Actor, completion, lifecycle 의미로 결합한다. | completion table, dispatch pump, actor/spot 수명을 관리한다. | application operation에 필요한 변환만 수행한다. | semantic adapter 유지 |
| `ZLinkBackendStreamSocketWrapper` | `IStreamSocket`, `IStreamSessionService` | raw frame과 bound actor session을 Framework Stream 의미로 결합한다. | session service와 socket의 종료 순서, shared MeshNode ownership을 관리한다. | `_sendGate`로 session submit 순서를 보장한다. | semantic adapter 유지 |
| `ZLinkRawRouterServicePort` / `ZLinkManagedMeshNode` raw receive | public `IRouterSocket.Recv(Received, ...)` | 이 경로는 Framework application message를 다시 감싸지 않고 service wire를 직접 처리한다. | 한 receive owner가 storage를 보유하고 처리 후 다음 receive에서 binding이 reset한다. | caller-provided `Received`와 재사용 event array를 사용한다. | binding public API 직접 호출 |
| `SetChannelName` on backend socket | 없음 | channel name은 Framework domain/config 값이며 binding socket에 전달할 의미가 없었다. | socket에 저장하거나 lifecycle을 바꾸지 않았다. | 호출·검증·fake method만 추가했다. | pass-through 제거 |

이 표의 `DEALER/ROUTER socket adapter`는 binding socket과 같은 의미의 public socket을
Framework public API로 재노출하지 않는다. Framework runtime이 요구하는 submit 결과,
option mapping과 lifecycle 규칙을 binding public builder에 맞추는 내부 경계다.

## 3. Direct call과 Adapter를 비교하는 방법

비자명한 operation은 구현 전에 두 설계를 비교한다.

| 대안 | 호출부에 남는 결정 | 판정 |
|---|---|---|
| binding public API 직접 호출 | builder 단계, binding option, raw event mapping, close 순서를 각 runtime 호출부가 알아야 한다. | semantic 결정이 여러 곳으로 새므로 선택하지 않는다. 의미가 정확히 같은 raw service path에서만 사용한다. |
| 인자만 전달하는 `*Wrapper` | 호출부 복잡성은 줄지 않고 class·method·test fake만 늘어난다. | shallow module이므로 제거한다. |
| semantic adapter/port | Framework operation의 결과·오류·ownership·lifecycle을 한 곳에서 결정한다. | 차이가 코드와 test로 증명될 때 선택한다. |

따라서 `IContext`, `IRouterSocket`, `IStreamSocket` 같은 binding object가 Framework
public contract에 나타나지 않는다는 사실만으로 모든 adapter를 유지하지 않는다. 반대로
adapter interface가 하나의 구현만 가진다는 이유만으로 제거하지도 않는다. 해당 port가
Framework domain 의미 또는 lifecycle owner를 보유하는지 먼저 확인한다.

금지하는 형태는 다음과 같다.

- binding object와 같은 인자·결과를 그대로 전달하는 `*Wrapper`
- testability만을 이유로 만든 한 구현짜리 `IBackend*`
- binding method 이름만 바꾸는 facade
- binding option의 두 번째 복제 표면
- Framework 호출부의 raw frame decode/encode 우회
- binding internal/private member, reflection 또는 native symbol 직접 호출

## 4. Receive storage ownership

`Received`와 `TopicMessage`는 message envelope를 만들기 위한 caller-provided storage다.
성공한 receive 뒤에는 envelope가 message parts를 소유한다. 다음 성공한 receive가 같은
storage를 overwrite하기 전까지, 현재 message를 읽는 모든 코드는 그 storage를 보유해야
한다.

### 4.1 동기 처리 경로

client control loop와 `ZLinkManagedMeshNode` receive loop는 message를 같은 실행 흐름에서
처리한다. 이 경로는 lifecycle 동안 `Received.Create()` 한 개를 보유하고
`Recv(storage, RecvFlags.DontWait)`에 반복해서 넘긴다. binding은 성공할 때 이전 parts와
metadata를 reset한 뒤 새 결과를 채운다. 처리가 끝난 뒤에만 다음 receive를 호출하므로
storage를 덮어써도 consumer가 이전 message를 보유하지 않는다.

### 4.2 비동기 application dispatch

client-server와 fanout receive loop는 application queue에 message를 넘길 수 있다. queue
worker가 handler를 완료하기 전에는 receive loop가 다음 message에 같은 storage를 사용할
수 없다. 이 경로는 `ZLinkReceivedStoragePool`과 `ZLinkTopicMessageStoragePool`을 사용한다.

1. receive loop가 pool에서 storage를 빌린다.
2. binding public receive operation이 storage에 결과를 채운다.
3. control message는 같은 loop에서 처리한 뒤 storage를 반환한다.
4. application message는 queue item이 storage의 소유권을 가져간다.
5. handler 또는 reject callback이 끝난 뒤 parts를 dispose하고 storage를 pool에 반환한다.

queue가 꽉 차거나 cancellation이 발생해도 reject callback이 소유권을 반환한다. 따라서
pool에 들어간 storage는 queue, handler, reply callback 중 어느 곳에서도 더 이상 사용되지
않는다. 반대로 `ZLinkRawRouterServicePort`처럼 envelope를 별도 record에 보관하는 경로는
하나의 storage를 공유해서는 안 된다. 그 경로는 envelope 수명에 맞는 별도 storage를
사용한다.

`DispatchWork`는 queue에 저장되는 값 타입이다. message마다 별도의 heap work record를
만들지 않지만, queue의 async handler가 필요한 경우에만 operation/lifecycle 경로의
delegate와 task를 사용한다. message parts 자체의 native allocation은 binding ownership
규칙에 따라 처리하며 Framework가 같은 bytes를 다시 복사하지 않는다.

## 5. Lifecycle과 동시성

DEALER/ROUTER adapter의 `_gate`는 binding fluent builder를 숨기기 위한 facade가 아니다.
Framework의 receive, reply, send/request와 socket dispose가 같은 socket owner를 사용하도록
하는 lifecycle guard다. binding 내부의 submit serialization과 별개로, Framework는 raw
receive와 close를 동시에 호출하지 않는 실행 순서를 보장해야 한다. 이 gate를 없애려면
다음 조건을 별도 test와 측정으로 증명해야 한다.

- receive, reply, request callback 등록과 dispose가 동시에 실행될 때 public binding contract가 허용하는가?
- close가 in-flight public operation을 거부하는 경우 Framework가 어떤 owner에서 join하는가?
- gate 제거 뒤 throughput, p99 latency와 lock contention이 기준선보다 나아지는가?

STREAM adapter의 `_sendGate`는 `IStreamSessionService.SendToActor` submit을 직렬화한다.
동시 bound actor send가 한 번에 하나만 binding session service에 도달한다는 test 계약이
있으므로, 대체할 단일 owner가 정해지기 전에는 제거하지 않는다.

client connection close에서 사용하는 10ms `Task.Delay`는 message hot path가 아니다.
in-flight request callback이 종료된 뒤 native close를 재시도하는 bounded lifecycle 경로다.
이를 receive/send operation의 per-message await로 확장하지 않는다.

## 6. 검증 기준

구현 변경은 다음 조건을 함께 확인한다.

- Framework public assembly가 binding socket, `IContext`, `IMeshNode`를 public contract로
  노출하지 않는다.
- Framework source가 binding internal/private member나 reflection을 사용하지 않는다.
- `Recv`와 `Subscribe`의 caller-provided storage가 queue handler 완료 전에 재사용되지 않는다.
- poll event array, receive envelope와 topic envelope가 정상 수명 범위에서 재사용된다.
- send/request/reply builder를 Framework 호출부가 직접 조립하지 않는다.
- 단순 `SetChannelName` 같은 pass-through contract와 fake method가 다시 생기지 않는다.

기본 검증 명령은 다음과 같다.

```text
dotnet build framework/languages/dotnet/src/Zlink.Framework/Zlink.Framework.csproj --nologo
dotnet build framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj --nologo
```

native runtime을 사용할 수 있는 환경에서는 backend factory, client-server channel,
fanout, stream concurrency와 receive dispatch budget test를 함께 실행한다. release
판정에서는 throughput, p99 latency, allocation/GC와 lock contention을 변경 전 기준선과
비교하고, 설명하지 못한 regression이 있으면 완료로 판정하지 않는다.

## 7. Regression Tests

다음 테스트가 이 문서의 설계 결정을 고정한다.

| Test | 통과 기준 |
|---|---|
| `BackendAdapterFactoryTests.BackendFactory_Creates_Channel_Spot_And_Stream_Wrappers` | backend factory가 Framework에 필요한 semantic adapter를 만들고, public surface에 binding object를 노출하지 않는다. |
| `ClientServerChannelRuntimeTests.BackendWrappers_DeliverUnsolicitedLivenessProbe` | caller-provided `Received` envelope를 control receive에서 재사용하고 application queue가 이를 보관하지 않는다. |
| `InboundDispatchBudgetTests.Dispatch_queue_rejects_when_full_without_blocking_receive_loop` | application queue가 가득 차면 소유한 envelope를 reject 경로에서 반환하고 control receive loop를 block하지 않는다. |
| `BackendStreamSocketConcurrencyTests.ConcurrentBoundActorMessages_AreSubmittedSerially` | Stream semantic adapter가 binding session service에 필요한 단일 submit owner를 유지한다. |
| `RegressionTests.DotNetContractDocuments_AllExposeRegressionTestSection` | 이 문서가 .NET 정식 문서 회귀 검증 집합에 포함되어 있다. |

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../README.ko.md) | [이전: Backend Policy](backend-dependency-policy.ko.md) | [다음: Regression Test Matrix](regression-test-matrix.ko.md)<!-- framework-adapter-nav:bottom:end -->
