# .NET Socket Surface 실행 가이드

> 상태: 완료
> 기준 범위: `bindings/dotnet/`, `bindings/dotnet/plan/socket/`
> 목적: `.NET` raw socket public surface를 concrete socket facade + internal kernel 구조로 끝까지 재구성
> 최종 종료 판정: `미적용 사항이 없습니다.`

## 1. 문서 목적

이 문서는 `bindings/dotnet` socket surface refactor의 유일한 실행 authority다.

이 문서는 별도 계획 문서를 전제로 진행하지 않는다.
필요한 설계 결정, 작업 레지스터, 검증 기준, 종료 조건을 이 문서 하나에 유지한다.

참고 입력:

- [`2026-03-27-dotnet-socket-surface-detailed-design.ko.md`](2026-03-27-dotnet-socket-surface-detailed-design.ko.md)

운영 규칙:

- 위 상세 설계 문서는 reference다.
- 실제 실행 authority는 이 실행 가이드다.
- 상세 설계와 실행 가이드가 어긋나면 먼저 이 실행 가이드를 고치고, 필요 시 상세 설계를 동기화한다.

이번 실행의 핵심 목표는 아래다.

- giant [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs) 를 public facade와 internal kernel로 분리
- `.NET` 사용자 기준의 concrete socket class를 도입
- generic `Socket` 은 compat shim으로 축소
- ownership / callback / send contract는 현재 동작을 유지
- option taxonomy 재설계는 범위 밖으로 고정

## 2. 고정 결정

이번 실행에서 더 이상 열어두지 않는 결정은 아래다.

- public namespace는 계속 `Zlink` 다.
- 새 public 타입은 아래 계층으로 고정한다.
  - `SocketBase`
  - `MessageSocketBase`
  - `PublisherSocketBase`
  - `SubscriberSocketBase`
  - `PairSocket`, `DealerSocket`, `RouterSocket`, `StreamSocket`, `PubSocket`, `XPubSocket`, `SubSocket`, `XSubSocket`
- interop/lifecycle/callback/native delegate pinning/option marshalling은 `internal SocketKernel` 에 모은다.
- `SocketKernel` 은 `internal sealed class` 다.
- public facade는 제한된 계층 상속을 사용하고, native ownership은 composition으로 숨긴다.
- generic `Socket` 은 typed socket을 상속하지 않는다.
- generic `Socket` 은 이번 작업에서 삭제하지 않는다.
- `SocketType` enum은 public compat 요소로 유지한다.
- `SetOption` / `GetOption` 은 `SocketBase` 공통 API로 유지한다.
- `SocketOptionKey<T>` taxonomy는 이번 작업에서 바꾸지 않는다.
- data-plane과 special API만 class surface로 분리한다.
- `StreamSocket.AttachStreamRaw` / `DetachStream` 은 `StreamSocket` 에만 둔다.
- `XPubSocket.ReceiveSubscriptionEvent` 는 `XPubSocket` 에만 둔다.
- `Publish` 는 publisher 계열에만 둔다.
- `Subscribe` / `SetSubscription` / `UnsetSubscription` / `SubscribeHandler` 는 subscriber 계열에만 둔다.
- `Send` / `Receive` / `RecvHandler` 는 message 계열에만 둔다.
- ownership 계약은 현재 코드와 동일하게 유지한다.
  - send 성공 시 library가 message ownership 소비
  - recv/callback 수신 시 application이 message ownership 소비

## 3. 금지 규칙

아래는 이번 실행에서 금지한다.

- 새 public namespace 추가
- `Socket.cs` 를 partial 분할만 하고 public giant surface를 유지하는 것
- option taxonomy 재설계까지 같이 밀어 넣는 것
- `Spot`, `Discovery`, `Registry` 를 raw socket hierarchy refactor에 끼워 넣는 것
- generic `Socket` 에 runtime type check만 추가해서 facade 분리 없이 끝내는 것
- hot path에 `IEnumerable`, LINQ, hidden copy, per-call delegate allocation을 넣는 것
- 문서 밖에서 별도 main/master/gap/spec/residual 문서를 추가 생성하는 것

## 4. 최종 public shape

최종 public 타입은 아래로 고정한다.

```text
SocketBase
  +-- MessageSocketBase
  |     +-- PairSocket
  |     +-- DealerSocket
  |     +-- RouterSocket
  |     +-- StreamSocket
  |
  +-- PublisherSocketBase
  |     +-- PubSocket
  |     +-- XPubSocket
  |
  +-- SubscriberSocketBase
        +-- SubSocket
        +-- XSubSocket

compat:
  Socket
```

핵심 의미:

- `PubSocket` 사용자는 `Receive(...)` 와 `Subscribe(...)` 를 보지 않는다.
- `SubSocket` 사용자는 `Publish(...)` 를 보지 않는다.
- `StreamSocket` 특수 API는 `StreamSocket` 에서만 보인다.
- generic `Socket` 은 compat shim 외 역할을 갖지 않게 줄인다.

## 5. old-to-new 매핑

새 문서, 새 샘플, 새 테스트는 아래 매핑을 기준으로 작성한다.

| 기존 | 새 기준 |
|---|---|
| `new Socket(ctx, SocketType.Pair)` | `new PairSocket(ctx)` |
| `new Socket(ctx, SocketType.Dealer)` | `new DealerSocket(ctx)` |
| `new Socket(ctx, SocketType.Router)` | `new RouterSocket(ctx)` |
| `new Socket(ctx, SocketType.Stream)` | `new StreamSocket(ctx)` |
| `new Socket(ctx, SocketType.Pub)` | `new PubSocket(ctx)` |
| `new Socket(ctx, SocketType.XPub)` | `new XPubSocket(ctx)` |
| `new Socket(ctx, SocketType.Sub)` | `new SubSocket(ctx)` |
| `new Socket(ctx, SocketType.XSub)` | `new XSubSocket(ctx)` |

## 6. 파일 배치

새 파일 배치는 아래로 고정한다.

- `src/Zlink/Sockets/Internal/SocketHandle.cs`
- `src/Zlink/Sockets/Internal/SocketKernel.cs`
- `src/Zlink/Sockets/SocketBase.cs`
- `src/Zlink/Sockets/MessageSocketBase.cs`
- `src/Zlink/Sockets/PublisherSocketBase.cs`
- `src/Zlink/Sockets/SubscriberSocketBase.cs`
- `src/Zlink/Sockets/PairSocket.cs`
- `src/Zlink/Sockets/DealerSocket.cs`
- `src/Zlink/Sockets/RouterSocket.cs`
- `src/Zlink/Sockets/StreamSocket.cs`
- `src/Zlink/Sockets/PubSocket.cs`
- `src/Zlink/Sockets/XPubSocket.cs`
- `src/Zlink/Sockets/SubSocket.cs`
- `src/Zlink/Sockets/XSubSocket.cs`

유지 파일:

- [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs)
- [`Message.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Message.cs)
- [`RoutingIdCodec.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/RoutingIdCodec.cs)
- [`SocketOptions.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/SocketOptions.cs)
- [`Monitor.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Monitor.cs)

## 7. 작업 레지스터

상태 값은 아래 네 개만 쓴다.

- `미착수`
- `진행중`
- `검증중`
- `완료`

### 7.1 Slice 1. internal kernel 추출

상태: `완료`

완료 메모:

- `SocketHandle` + `SocketKernel` 로 native handle/callback/option marshalling 을 internal로 이동했다.
- compat [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs) 는 kernel forwarding 중심 shim 으로 축소됐다.

목표:

- native handle ownership utility와 kernel 추출
- 현재 `Socket.cs` 의 interop/callback/helper 복잡성을 internal로 이동

대상 파일:

- 추가: `src/Zlink/Sockets/Internal/SocketHandle.cs`
- 추가: `src/Zlink/Sockets/Internal/SocketKernel.cs`
- 수정: [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs)

작업:

- `Bind`, `Connect`, `Unbind`, `Disconnect` 이동
- `AttachDiscovery`, `SendReadyHandler`, `OpenMonitor` 이동
- `Send*`, `Receive*`, `Publish*`, `Subscribe*`, `ReceiveSubscriptionEvent*` 이동
- `RecvHandler`, `SubscribeHandler`, `AttachStreamRaw`, `DetachStream` 이동
- native callback trampoline과 delegate field 이동
- option marshalling helper 이동

완료 기준:

- `Socket.cs` 는 kernel forwarding 위주로 줄어듦
- `Socket.cs` 안에 native callback trampoline이 남지 않음
- `Socket.cs` 안에 option marshalling helper가 남지 않음
- public API 변화 없이 기존 테스트 통과

### 7.2 Slice 2. abstract facade 도입

상태: `완료`

완료 메모:

- `SocketBase`, `MessageSocketBase`, `PublisherSocketBase`, `SubscriberSocketBase` 를 추가해 공통 API와 semantic API를 분리했다.
- `Poller`, `Runtime`, `ZlinkPoll` 도 `SocketBase` 기준으로 typed socket 을 직접 받도록 맞췄다.

목표:

- semantic base facade 추가

대상 파일:

- 추가: `src/Zlink/Sockets/SocketBase.cs`
- 추가: `src/Zlink/Sockets/MessageSocketBase.cs`
- 추가: `src/Zlink/Sockets/PublisherSocketBase.cs`
- 추가: `src/Zlink/Sockets/SubscriberSocketBase.cs`
- 수정: [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs)

작업:

- `SocketBase` 에 lifecycle, monitor, discovery, common option API 배치
- `MessageSocketBase` 에 `Send` / `Receive` / `RecvHandler` 배치
- `PublisherSocketBase` 에 `Publish` 배치
- `SubscriberSocketBase` 에 `SetSubscription` / `UnsetSubscription` / `Subscribe` / `SubscribeHandler` 배치

완료 기준:

- base class는 전부 `abstract`
- common public API가 giant `Socket` 밖으로 이동
- concrete 구현 없는 public helper static class를 새로 만들지 않음

### 7.3 Slice 3. concrete socket 도입

상태: `완료`

완료 메모:

- `PairSocket`, `DealerSocket`, `RouterSocket`, `StreamSocket`, `PubSocket`, `XPubSocket`, `SubSocket`, `XSubSocket` 를 `sealed` concrete facade로 추가했다.
- `StreamSocket` 에만 `AttachStreamRaw` / `DetachStream`, `XPubSocket` 에만 `ReceiveSubscriptionEvent` 를 남겼다.

목표:

- concrete socket class 추가

대상 파일:

- 추가: `src/Zlink/Sockets/PairSocket.cs`
- 추가: `src/Zlink/Sockets/DealerSocket.cs`
- 추가: `src/Zlink/Sockets/RouterSocket.cs`
- 추가: `src/Zlink/Sockets/StreamSocket.cs`
- 추가: `src/Zlink/Sockets/PubSocket.cs`
- 추가: `src/Zlink/Sockets/XPubSocket.cs`
- 추가: `src/Zlink/Sockets/SubSocket.cs`
- 추가: `src/Zlink/Sockets/XSubSocket.cs`

작업:

- 각 concrete class 생성자 추가
- `StreamSocket` 에만 `AttachStreamRaw` / `DetachStream` 노출
- `XPubSocket` 에만 `ReceiveSubscriptionEvent` 노출

완료 기준:

- concrete class는 모두 `sealed`
- type별 irrelevant method가 public surface에서 사라짐
- 새 public namespace 추가 없음

### 7.4 Slice 4. compat `Socket` 축소

상태: `완료`

완료 메모:

- `Socket(Context, SocketType)` 생성자는 유지했다.
- generic `Socket` 은 typed hierarchy를 상속하지 않고 같은 kernel을 forwarding 하는 compat facade로 축소됐다.

목표:

- generic `Socket` 을 compat shim으로 축소

대상 파일:

- 수정: [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs)

작업:

- `Socket(Context, SocketType)` 유지
- 구현은 `SocketKernel` composition forwarding으로 축소
- typed socket 상속 없이 legacy API 유지

완료 기준:

- generic `Socket` 은 실제 구현체가 아니라 compat facade
- typed socket과 같은 kernel을 공유

### 7.5 Slice 5. tests / docs / samples 전환

상태: `완료`

완료 메모:

- reflection 기반 [`test_socket_surface.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/tests/Zlink.Tests/test_socket_surface.cs) 로 typed/compat surface 를 검증한다.
- compat roundtrip test를 추가했고, 대표 contract tests와 sample programs/README를 concrete socket 기준으로 전환했다.
- 현재 저장소 범위에서 사용자 노출 문서는 `samples/*/README.md` 와 실행 가이드이며, 새 샘플 문서는 generic `Socket` 생성 예시를 사용하지 않는다.

목표:

- surface 검증 자산을 typed socket 기준으로 전환

대상 파일:

- `tests/Zlink.Tests/*.cs`
- `doc/bindings/dotnet*.md`
- `samples/` 하위 관련 프로젝트

작업:

- reflection 기반 API surface test 추가
- compat layer test 추가
- 샘플과 문서를 concrete socket 기준으로 전환

완료 기준:

- 새 샘플과 새 문서는 generic `Socket` 을 사용하지 않음
- reflection 기반 surface test 존재
- compat 경로도 계속 동작

### 7.6 Slice 6. POSD 기반 잔여 리팩토링 소거

상태: `완료`

완료 메모:

- 사용자가 알아야 할 interop/callback/option marshalling 세부사항을 `SocketKernel` 안으로 밀어 넣어 facade 설명 범위를 줄였다.
- typed facade는 역할별 API만 노출하고, compat `Socket` 만 legacy union surface 를 유지하게 해 change amplification 을 줄였다.
- 남은 forwarding은 intentional tradeoff다. generic `Socket` compat 유지 비용 외에 추가 복잡도 감소를 위해 public breaking change를 감수할 이유가 현재는 없다.

목표:

- socket surface 분리 이후에도 남는 shallow wrapper, change amplification, hidden coupling, temporal decomposition 흔적을 계속 줄인다.
- 설명 가능한 complexity reduction 항목이 더 이상 없을 때까지 반복한다.

대상 파일:

- `src/Zlink/Sockets/*.cs`
- [`Socket.cs`](/home/hep7/project/kairos/zlink/bindings/dotnet/src/Zlink/Socket.cs)
- 관련 tests / docs

작업:

- facade와 kernel 경계에서 중복 forwarding/중복 validation/중복 ownership 분기를 줄인다.
- public facade가 알아야 할 내부 지식이 남아 있으면 kernel 안으로 다시 밀어 넣는다.
- 이름만 분리된 얕은 wrapper가 있으면 더 깊은 모듈로 흡수한다.
- 타입 간 복제 코드가 남아 있으면 공통 private/internal helper로 정리한다.
- 설명하기 어려운 lifecycle/callback invariant가 남아 있으면 더 단순한 계약으로 정리한다.
- 각 라운드 끝마다 "이 리팩토링이 실제로 사용자가 알아야 할 것을 줄였는가"를 문서에 짧게 기록한다.

완료 기준:

- POSD 기준으로 설명 가능한 잔여 리팩토링 항목이 더 이상 남지 않음
- 남아 있는 중복/결합/복잡성이 intentional이며 문서로 정당화 가능
- 추가 리팩토링 제안이 모두 "복잡도 감소보다 이동 비용이 큼" 범주로 정리됨

## 8. iteration 순서

매 iteration은 아래 순서를 강제한다.

1. 작업 레지스터에서 첫 미완료 slice를 잡는다.
2. 해당 slice 파일만 수정한다.
3. 관련 테스트를 먼저 좁게 검증한다.
4. slice 상태를 `검증중` 또는 `완료`로 갱신한다.
5. 문서 전체를 다시 훑고 다음 미완료 slice가 남아 있으면 계속 진행한다.

예외:

- `Slice 6` 은 단일 구현이 아니라 반복 소거 단계다.
- `Slice 1-5` 가 모두 완료된 뒤에만 들어간다.
- `Slice 6` 에 들어간 뒤에는 각 iteration마다 최소 하나의 complexity reduction 또는 "추가 리팩토링 불필요" 판단 근거를 남겨야 한다.

진행 중 설계와 코드가 어긋나면:

1. 이 실행 가이드를 먼저 수정한다.
2. 그 다음 코드를 수정한다.

## 9. 검증 명령

기본 검증:

```bash
dotnet build /home/hep7/project/kairos/zlink/bindings/dotnet/Zlink.sln
dotnet test /home/hep7/project/kairos/zlink/bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj
```

slice별 최소 검증:

- Slice 1

```bash
dotnet build /home/hep7/project/kairos/zlink/bindings/dotnet/Zlink.sln
dotnet test /home/hep7/project/kairos/zlink/bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj --filter FullyQualifiedName~test_callback_contract
```

- Slice 2-4

```bash
dotnet build /home/hep7/project/kairos/zlink/bindings/dotnet/Zlink.sln
dotnet test /home/hep7/project/kairos/zlink/bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj --filter "FullyQualifiedName~test_callback_contract|FullyQualifiedName~test_pair_tcp|FullyQualifiedName~test_pubsub|FullyQualifiedName~test_stream_socket|FullyQualifiedName~test_socket_surface"
```

- Slice 5

```bash
dotnet build /home/hep7/project/kairos/zlink/bindings/dotnet/Zlink.sln
dotnet test /home/hep7/project/kairos/zlink/bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj
```

- Slice 6

```bash
dotnet build /home/hep7/project/kairos/zlink/bindings/dotnet/Zlink.sln
dotnet test /home/hep7/project/kairos/zlink/bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj
```

검증 규칙:

- dedicated surface test가 아직 없을 때는 `FullyQualifiedName~surface` 같은 느슨한 필터를 사용하지 않는다.
- Slice 5에서 reflection 기반 surface test가 추가된 이후에만 그 테스트 이름으로 좁은 필터를 다시 정의한다.
- 새로운 surface test 파일이 생기면 이 섹션의 최소 검증 명령도 같이 갱신한다.

이번 실행 검증 결과:

- `dotnet build /home/hep7/project/kairos/zlink/bindings/dotnet/Zlink.sln`
- `dotnet test /home/hep7/project/kairos/zlink/bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj --filter "FullyQualifiedName~test_callback_contract|FullyQualifiedName~test_pair_tcp|FullyQualifiedName~test_pubsub|FullyQualifiedName~test_stream_socket|FullyQualifiedName~test_socket_surface"`
- `dotnet test /home/hep7/project/kairos/zlink/bindings/dotnet/tests/Zlink.Tests/Zlink.Tests.csproj`
- `./bindings/dotnet/plan/socket/run_dotnet_socket_surface_execution.sh --max-iterations 0`

## 10. 로그와 운영 규칙

- 기본 로그 디렉터리는 [`logs/`](logs) 다.
- 실행 wrapper는 별도 실행 lock을 두지 않는다.
- 병렬 실행이 필요하면 `--logs-dir` 와 `--gate-label` 을 분리한다.
- commit / push는 사용자 지시가 있을 때만 수행한다.
- unrelated 변경은 현재 실행 범위와 분리한다.
- 이 refactor는 기본적으로 장시간 gate를 요구하지 않는다.
- gate가 필요해지면 아래 명령을 같은 셸에서 사용한다.

```bash
./core/tools/ralphloop/run_execution_gate_loop.sh \
  --logs-dir /home/hep7/project/kairos/zlink/bindings/dotnet/plan/socket/logs \
  --label dotnet_socket_surface_gate \
  --count 1
```

## 11. 종료 조건

아래를 모두 만족해야 완료다.

- Slice 1-5 가 모두 `완료`
- Slice 6 이 `완료`
- concrete socket public surface가 문서와 일치
- generic `Socket` 이 compat shim으로 축소
- build/test green
- 이 문서를 다시 읽어도 남은 미적용 구현 항목이 없음
- POSD 기준으로 더 진행할 가치가 있는 잔여 refactoring target이 없다고 판단 가능

## 12. API review gate

구현 후 아래 질문에 모두 `예` 라고 답할 수 있어야 한다.

- `PubSocket` 사용자가 `Receive(...)` 와 `Subscribe(...)` 를 보지 않는가?
- `SubSocket` 사용자가 `Publish(...)` 를 보지 않는가?
- `StreamSocket` 특수 API가 다른 socket에서 사라졌는가?
- `XPubSocket.ReceiveSubscriptionEvent` 가 publisher base에 섞이지 않았는가?
- generic `Socket` 이 실제 구현체가 아니라 compat facade로 축소되었는가?
- callback/ownership/send 계약이 refactor 전과 동일한가?
- option taxonomy를 억지로 같이 바꾸지 않았는가?
- 남은 구조가 "그냥 아직 못 건드린 중복"이 아니라 intentional tradeoff라고 설명 가능한가?

## 13. wrapper 실행

자동 반복 실행은 아래 wrapper를 사용한다.

- [`run_dotnet_socket_surface_execution.sh`](run_dotnet_socket_surface_execution.sh)

smoke 확인:

```bash
./bindings/dotnet/plan/socket/run_dotnet_socket_surface_execution.sh --max-iterations 0
```

이 경로는 supervisor까지 실제 호출하지만 Codex iteration은 돌리지 않는다.
