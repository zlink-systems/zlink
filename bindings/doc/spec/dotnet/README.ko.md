---
title: ".NET 바인딩 구현 청사진"
---

<!-- bindings-nav:start -->
[스펙 목록](../README.ko.md) | [이전: C](../c/README.ko.md) | [다음: C++](../cpp/README.ko.md)
<!-- bindings-nav:end -->

# .NET 바인딩 구현 청사진

> **이 장이 정의하는 것** — .NET 라이브러리가 갖춰야 할 `Contracts`/`Runtime`
> 형태와, 다른 래퍼 바인딩이 참조하는 기준 아키텍처 맵.

이 문서는 .NET 라이브러리가 갖춰야 할 형태를 정의한다. 모든 인터페이스 멤버를
빠짐없이 나열한 목록은 아니다. 실제 공개 계약의 소스는
`bindings/dotnet/src/Zlink/Contracts/`에 있다.

`Contracts/`, 런타임 구현 클래스, 테스트, 샘플, perf 러너, 패키지 동작이 이
청사진을 따르고 `core/include/zlink.h`의 안정 기능들을 .NET에 맞는 API로
사상할 때 .NET 구현은 정렬되었다고 본다.

이 README는 완성된 .NET 바인딩 형태를 기술하며, 일시적 목표 초안이 아니다.
또한 다른 래퍼 바인딩 문서들을 동일한 아키텍처 맵에 정렬시키는 기준 가이드
역할도 한다. 다른 바인딩이 언어별 명명을 사용하더라도 여기서 설명한
contract/runtime 소유, 공개 계약 카테고리, 파일 분할 기준, 검증 의도는 그대로
유지한다.

이 바인딩은 공통 바인딩 아키텍처 맵을 .NET 명명으로 따른다.
`Contracts/<Category>`가 공개 계약 소스를 소유하고 `Runtime/<Category>`가
구현을 소유한다. 다른 바인딩은 다른 대소문자 표기나 패키지 이름을 쓸 수
있지만, 이 문서는 동일한 맵을 .NET으로 투영한 것이다.

리뷰어가 처음 읽는 코드는 `Contracts/` 아래의 공개 계약이어야 한다. 런타임
파일은 그 계약을 구현해야 하며, 사용자에게 노출되는 새 동작이 런타임 파일에서
처음 발견되어서는 안 된다.

| 절 | 다루는 내용 |
|---|---|
| [공개 계약 소스](#공개-계약-소스) | 네임스페이스, 계약/런타임 소스 위치, API reference 링크 |
| [저장소 레이아웃](#저장소-레이아웃) | 정렬된 디렉터리 트리와 폴더 소유 경계 |
| [API 변경 워크플로](#api-변경-워크플로) | 신규 매핑·리팩터 절차, 제거해야 할 단축 경로 |
| [라이브러리 형태](#라이브러리-형태) | 인터페이스/구체 타입 분류, builder, `IDisposable`, RoutingId 헬퍼 |
| [Contract / Runtime 배치 규칙](#contract--runtime-배치-규칙) | 공개 선언과 런타임 구현의 경계 |
| [표준 인터페이스 규칙](#표준-인터페이스-규칙) | recv 시그니처, builder 시작 메서드, 이름 제약 |
| [Contract 폴더 레이아웃](#contract-폴더-레이아웃) | `Contracts/` 하위 카테고리별 소유 범위 |
| [Runtime 폴더 레이아웃](#runtime-폴더-레이아웃) | `Runtime/` 하위 카테고리별 구현 범위 |
| [생성 진입점](#생성-진입점) | 공개 팩토리 메서드 목록 |
| [필수 기능 커버리지](#필수-기능-커버리지) | 정렬 시 보장해야 할 사용자 노출 기능 |
| [Receive 및 Subscribe 형태](#receive-및-subscribe-형태) | 호출자 제공 저장소와 no-data 구분 |
| [Service 및 SPOT 형태](#service-및-spot-형태) | `ISpotNode`/`ISpot` 책임 분리 |
| [Byte HWM 및 monitoring ABI v4](#byte-hwm-및-monitoring-abi-v4) | `ulong` byte HWM과 monitor snapshot field |
| [Receive flow state](#receive-flow-state) | receive-flow 상태 타입, setter와 monitor 표면 |
| [에러 및 검증 정책](#에러-및-검증-정책) | 검증 시점과 예외 매핑 |
| [성능 정책](#성능-정책) | hot path 제약 |
| [구현 체크리스트](#구현-체크리스트) | 정렬 선언 전 확인 항목과 필수 검증 명령 |
| [Actor 및 Spot Route 결과](#actor-및-spot-route-결과) | route 결과 record와 Actor 대상 send/request |

## 공개 계약 소스

- 공개 네임스페이스: `Systems.Zlink`.
- 패키지 identity: `Systems.Zlink`.
- 공개 계약: `bindings/dotnet/src/Zlink/Contracts/`.
- 런타임 구현: `bindings/dotnet/src/Zlink/Runtime/`.
- 내부 구현: P/Invoke 선언, `SafeHandle` 또는 네이티브 핸들 소유,
  콜백 trampoline, request 진행 펌프, 네이티브 모델 변환기, socket 커널,
  옵션 접근자, 버퍼 코덱, 검증 헬퍼.
- 문서 역할: 이 README는 라이브러리 형태와 리뷰 규칙을 정의한다.
  `Contracts/`가 정확한 공개 동작 표면을 소유한다.
- API reference 주석: [`api-reference-comments.ko.md`](api-reference-comments.ko.md)가
  `Contracts/`의 XML 주석 작성과 리뷰 기준을 정의한다.

런타임 구현 파일은 `Contracts/`나 문서화된 생성 진입점만으로는 이해할 수 없는
사용자 노출 동작을 정의하지 않는다.

## 저장소 레이아웃

.NET 바인딩을 변경할 때는 다음 경로를 일관되게 사용한다.

- 공개 계약: `bindings/dotnet/src/Zlink/Contracts/`.
- 런타임 구현: `bindings/dotnet/src/Zlink/Runtime/`.
- 네이티브 브리지/아티팩트: `bindings/dotnet/src/Zlink/Runtime/Native/`,
  `bindings/dotnet/native/`. NuGet package 안에서는 이 파일들이
  `runtimes/<rid>/native/` 구조로 배치된다.
- 코덱 package: 제공하지 않는다. .NET 바인딩은 raw `Message`와 byte payload API만
  유지한다.
- 테스트: `bindings/dotnet/tests/Zlink.Tests/`.
- 샘플: `bindings/dotnet/samples/`.
- Perf: `bindings/dotnet/perf/`.

- `Contracts/`의 공개 시그니처에는 P/Invoke 선언, `SafeHandle` 세부사항, 마샬링 전용으로 쓰이는 네이티브 struct mirror, request 펌프 타입이 들어가지 않는다.
- 구체 값 타입은 ownership을 위해 내부적으로 네이티브 기반 저장소를 쓸 수 있지만, .NET은 VM이 관리하는 버퍼를 빌려 쓰는 zero-copy send 경로를 공개 또는 기본 동작으로 노출하거나 사용하지 않는다.
- 네이티브 브리지 선언과 마샬링 전용 mirror는 여전히 `Runtime/Native/`에 둔다.
- `Contracts/`와 `Runtime/`은 저장소상 고정 폴더다.
- `Systems.Zlink` 네임스페이스와 NuGet 패키지 표면은 그 계약을 .NET으로 투영한 결과다.
- `Contracts`나 `Runtime`이라는 이름의 네임스페이스 세그먼트를 사용자 표면용 주 네임스페이스로 노출하지 않는다.

아래 트리는 소유 권한에 대해 규범적이며 대표 파일들을 보여 준다. 전체 파일
목록은 아니다.

- 공개 동작을 정의하는 파일은 `Contracts/` 아래에 둔다.
- 네이티브 코드를 호출하거나 핸들을 소유하거나 struct를 마샬링하거나 콜백/request 진행 로직을 실행하는 파일은 `Runtime/` 아래에 두며, 네이티브 브리지 코드는 `Runtime/Native/` 아래에 둔다.

```text
bindings/dotnet/
+-- src/
|   +-- Zlink/
|   |   +-- Contracts/
|   |   |   +-- Core/
|   |   |   |   +-- Context.cs
|   |   |   |   +-- ContextOptions.cs
|   |   |   |   +-- RoutingId.cs
|   |   |   |   +-- Zlink.cs
|   |   |   +-- Messaging/
|   |   |   |   +-- Message.cs
|   |   |   |   +-- Received.cs
|   |   |   |   +-- TopicMessage.cs
|   |   |   |   +-- SubscriptionEvent.cs
|   |   |   |   +-- OperationContracts.cs
|   |   |   +-- Sockets/
|   |   |   |   +-- ISocket.cs
|   |   |   |   +-- MessageSocketContracts.cs
|   |   |   |   +-- RoutedSocketContracts.cs
|   |   |   |   +-- PubSubSocketContracts.cs
|   |   |   |   +-- IStreamSocket.cs
|   |   |   |   +-- SocketOptionFacades.cs
|   |   |   +-- Eventing/
|   |   |   |   +-- Monitor.cs
|   |   |   |   +-- Poller.cs
|   |   |   |   +-- PollEvent.cs
|   |   |   |   +-- Timer.cs
|   |   |   |   +-- ZlinkPoll.cs
|   |   |   +-- Service/
|   |   |   |   +-- SpotNode.cs
|   |   |   |   +-- Spot.cs
|   |   |   |   +-- Actor.cs
|   |   |   |   +-- SpotNodeModels.cs
|   |   |   +-- Errors/
|   |   |   |   +-- Errors.cs
|   |   +-- Runtime/
|   |   |   +-- Core/
|   |   |   +-- Handles/
|   |   |   +-- Messaging/
|   |   |   +-- Sockets/
|   |   |   +-- Eventing/
|   |   |   +-- Service/
|   |   |   +-- Errors/
|   |   |   +-- Buffers/
|   |   |   +-- Options/
|   |   |   +-- Native/
+-- tests/
+-- samples/
+-- perf/
+-- native/
+-- runtimes/
```

- `Contracts`와 `Runtime` 폴더 이름은 저장소상의 소유 경계다. `Systems.Zlink.Contracts`나 `Systems.Zlink.Runtime`을 사용자 노출 네임스페이스로 공개해도 된다는 허가가 아니다.
- 공개 생성은 공개 계약이 명시적으로 구체 값 타입을 요구하지 않는 한 `IContext`, socket 인터페이스, `ISpotNode`, `IPoller`, `IZlinkTimer` 같은 공개 계약을 반환한다.
- `Context`, socket 클래스, `SpotNode`, `Poller`, `Timer` 같은 런타임 클래스는 구현 소유자이며 소비자 표면으로 선호되는 대상이 아니다.

`Runtime/Buffers`, `Runtime/Handles`, `Runtime/Options`는 구현 지원 카테고리다.
.NET 바인딩에는 숨겨야 할 실제 네이티브 ownership, routing-id 인코딩,
옵션 검증 결정이 존재하기 때문에 이 폴더들이 존재한다. 다른 바인딩은 이
지원 영역들에 다른 이름을 쓸 수 있지만, 그 세부 내용을 공개 계약 파일로
옮기지 않는다.

## API 변경 워크플로

새 코어 기능을 사상할 때:

1. 사용자 노출 동작을 알맞은 `Contracts/` 카테고리에 추가한다.
2. 호출자가 대체 가능한 동작을 필요로 하지 않는 한 구체 DTO/value/record
   타입을 사용한다.
3. 네이티브 브리지 타입을 노출하지 않고 `Runtime/` 구현을 추가하거나 수정한다.
4. 인터페이스만으로는 객체를 생성할 수 없다면 새 생성 진입점을 문서화한다.
5. `internal` 멤버가 아니라 공개 계약을 대상으로 테스트를 추가한다.
6. 샘플과 perf는 공개 계약과 공개 팩토리만 통해 갱신한다.
7. 프레임워크 어댑터가 reflection이나 `InternalsVisibleTo`로 바인딩의
   private 멤버에 접근하지 않는지 확인한다.

기존 .NET 코드를 리팩터링할 때:

1. 사용자 노출 선언은 대응되는 `Contracts/` 카테고리로 옮긴다.
2. 네이티브 기반 구현, 핸들 ownership, request 진행, 마샬링, 옵션 검증은
   `Runtime/`으로 옮긴다.
3. P/Invoke 선언과 네이티브 struct mirror는 `Runtime/Native/`에 유지한다.
4. 호출자의 복잡도를 줄여 주지 않으면서 옛 형태만 보존하는 중복 공개 진입점은
   제거한다.
5. 샘플, perf, 프레임워크 어댑터는 공개 계약과 문서화된 생성 진입점을 통해서만
   갱신한다.
6. 공개 `Systems.Zlink` 표면을 통해 테스트를 추가하거나 갱신한다.

아래 .NET 고유 단축 경로가 모두 사라졌을 때 리팩터링이 완료된 것으로 본다.

- 공개 계약은 P/Invoke, `SafeHandle`, 네이티브 struct, raw 옵션 id, 콜백
  userdata, request 펌프 상태, part 루프 헬퍼를 언급하지 않는다.
- 런타임 클래스는 `Contracts/`에서 찾을 수 없는 공개 동작을 도입하지 않는다.
- 프레임워크 어댑터, 샘플, perf, 테스트는 reflection, `NonPublic` 조회,
  private 런타임 단축 경로를 사용하지 않는다.
- 호환성 래퍼를 옛 공개 형태를 보존할 목적으로만 유지하지 않는다.

## 라이브러리 형태

.NET 바인딩은 contract/runtime 분할을 사용한다.

- 동작 계약은 `Contracts/`에 있는 공개 `I*` 인터페이스다.
  operation builder 계약은 그 패키지에서 정해진 공개 형태에 따라
  `SendOperation`이나 `RequestOperation` 같은 도메인 이름을 사용할 수 있다.
- 호출자가 `Context`, `DealerSocket`, `RouterSocket`, `SpotNode`, `Poller`,
  `Timer` 같은 공개 팩토리를 통해 리소스를 생성해야 할 때, 네이티브 기반 구현은
  `Runtime/`의 internal sealed 클래스다.
- 인스턴스화 불가한 추상 기반 클래스는 위의 런타임 구현 클래스들을 위한 구현 지원
  용도로만 `Runtime/`에 둘 수 있다. 이들은 생성 진입점이 아니며, 그 공개 동작은
  반드시 `Contracts/` 인터페이스나 값 타입으로 포괄된다.
- DTO, value, result, option, enum, exception 타입은 구체 타입으로 유지한다.
  통상의 .NET 관례대로 `record`, `sealed class`, `readonly struct`, `enum`을
  사용한다. 메시지 part를 소유해 dispose해야 하는 envelope는 `record`가 아니라
  `sealed class`로 둔다.
- operation builder는 단계별 네이티브 request 상태와 multipart 누적을 숨기기
  위해 인터페이스로 둔다.
- 공개 static facade, 확장 메서드, builder convenience 헬퍼는 호출자가 직접
  호출할 수 있을 때 계약의 일부다. 구현이 런타임 코드로 위임되더라도 정의는
  소유 `Contracts/` 카테고리 아래에 둔다.
- 네이티브 핸들, request 펌프, 콜백 브리지 상태, part 루프 시퀀싱, raw 옵션
  id는 `Runtime/`이나 `internal` 구현 타입에 유지한다.
- 폐기 가능한 네이티브 리소스는 `IDisposable`과 `IAsyncDisposable`을 모두
  구현한다.

대칭성을 위해 `Message`, `RoutingId`, `Received`, `TopicMessage` 같은 DTO를
인터페이스로 만들지 않는다. 이들은 ownership과 할당 동작이 분명한 구체
도메인 값이다. `Received`는 호출자가 제공해 재사용하는 recv 저장소이므로
`Received.Create()`로 만든다.

다른 래퍼 바인딩 문서가 따르는 표준 인터페이스 분류는 아래 .NET 타입들이
정의한다.

- Core 리소스: `IContext`.
- Socket 리소스 역할: `ISocket`, `IMessageSocket`, routed socket 계약,
  pub/sub socket 계약, pair, dealer, router, pub, sub, xpub, xsub, stream
  socket family 인터페이스. 단, 해당 family가 네이티브 기반 동작을 가질 때만
  family 인터페이스를 둔다.
- Eventing 리소스 역할: monitor socket 계약, `IPoller`, poll event source
  계약, `IZlinkTimer`.
  `ISpotNode`, `ISpot`, 그리고 Actor handle이 노출될 때의 `IActor` 또는
  동등한 actor 리소스 계약.
- Operation builder 역할: send, routed send, request, reply, publish,
  channel send/request, SPOT send/request/reply, actor create, actor join,
  actor join reply operation.
- Callback 역할: stream packet handler, monitor handler, poll handler,
  SPOT dispatch handler, route handler, admission handler, reply callback.

### RoutingId 문자열 및 바이너리 헬퍼

`RoutingId`는 바이너리 안전한 값 타입을 유지한다. 공개 .NET 헬퍼의 의미는
다음과 같다.

- `RoutingId.From(string value)`는 사용자 routing id 문자열을 UTF-8로
  인코딩한다.
- `RoutingId.From(byte[] value)`와 `RoutingId.From(ReadOnlySpan<byte> value)`는
  routing id의 raw 바이트를 그대로 보존한다.
- `RoutingId.FromHex(value)`는 `ToHex()`로 출력했던 바이트를 복원한다.
- `RoutingId.From(uint value)`는 4바이트 big-endian `uint32` routing id를
  기록한다.
- `RoutingId.From(Guid value)`는 16바이트 UUID routing id를 기록한다.
- `ToString()`은 표시 용도다: 인쇄 가능한 UTF-8 텍스트, 그 다음 `uint32`,
  그 다음 UUID, 더 명확한 표현이 없으면 `hex:` 접두사와 raw hex.

내구성 있는 raw-byte round trip에는 `ToHex()` / `FromHex(value)`를 사용한다.

`RoutingId` 캐싱은 오직 내부 최적화일 뿐이다. 바인딩은 해시나 짧은 수명의
receive-path 값을 캐시할 수 있지만, equality와 공개 동작은 오직 불변 바이트
값으로만 정의된다.

## Contract / Runtime 배치 규칙

- 공개 인터페이스, 구체 DTO/value 타입, enum, 공개 예외 도메인은 `Contracts/`에
  둔다.
- 공개 static facade, 확장 메서드, 모듈 스타일 헬퍼, builder convenience
  헬퍼는 `Contracts/`에 둔다.
- 런타임 구현, socket 커널, request 펌프, 콜백 브리지 상태, 생명주기 소유자는
  `Runtime/`에 둔다.
- P/Invoke 선언, `SafeHandle` 구현, 네이티브 struct mirror, 마샬링 헬퍼,
  플랫폼 로딩 코드는 `Runtime/Native/`에 둔다.
- `Contracts/`의 공개 시그니처는 `Runtime/Native/` 타입을 언급하지 않는다.
- 런타임 클래스를 직접 생성용으로 의도적으로 노출하는 경우라도 그 공개
  동작은 여전히 `Contracts/`로 기술된다. 이는 예외이며 기본 형태가 아니다.

## 표준 인터페이스 규칙

- 데이터 플레인 `Recv`, routed recv, `Subscribe`, subscription 이벤트 수신은
  호출자가 제공한 `Received`, `TopicMessage`, `SubscriptionEvent` 인스턴스를
  채우고 `bool`을 반환한다.
- .NET 호출자는 재사용 가능한 수신 저장소를 `Received.Create()`로 만든다.
  `Received`에는 공개 생성자가 없다.
- `Send`, routed send, `Publish`, `Request`, `Reply`, SPOT 연산, Actor
  location/세션 연산은 fluent operation builder를 반환한다.
- PUB/XPUB `Publish(topic)`의 terminal은 동기 `PublishSubmitOperation.Submit()
  -> void`다. 기본 PUB 의미론은 lossy이므로 publisher는 HWM에서 대기하지
  않는다. `NODROP`에서 가득 찬 subscriber는 그 자리에서
  `ZlinkSubmitException`(`Result == Backpressured`)으로 표면화되고, 재시도
  정책은 어플리케이션이 소유한다. `TryPublish(topic)`는 같은 backpressure를
  예외 없이 `false`로 관찰하는 별도 표면이다.
- back-pressure 해소 콜백(`OnSendReady`)은 공개 계약이 아니다. `send_ready`
  readiness-hint 시맨틱이 Core 0.13.0에서 폐지되었기 때문이다.
- builder의 시작 메서드는 target identity, topic, channel, routing id,
  request 시퀀스만 받는다. payload, flag, timeout, callback, 비동기 submit
  선택은 builder 단계에서 처리한다.
- reply builder는 send flag 단계를 갖지 않는다. core reply 함수는 send flag
  인자를 받지 않으므로, .NET binding은 no-op `Flags(...)`를 public 계약으로
  노출하지 않는다.
- Raw ROUTER/`Received` reply의 terminal은
  `ReplySubmitOperation.Submit() -> void`인 동기 one-shot이다. Terminal reply와 error
  reply를 HWM 없는 completion lane에 native 호출 한 번으로 제출한다. HWM backpressure는
  reply 결과가 아니며 `NotConnected`, `Terminated`, `InvalidArgument`와 그 밖의 non-HWM
  submit 실패는 즉시 `ZlinkSubmitException`으로 전달한다.
- operation 시작 메서드와 같은 이름을 가진 single-payload 단축 오버로드를
  추가하지 않는다. `Send(Message)`, `Send(RoutingId, Message)`,
  `Publish(string, Message)`, `SendToChannel(string, Message)`,
  `SendToSpot(..., Message)`는 공개 계약 멤버가 아니다. 호출자는 역할별
  builder terminal을 사용하며, DEALER/ROUTER routed send는
  `Send(...).Message(message).Async()`가 canonical이다.
- multipart payload는 `Message(...)` 호출을 반복해 누적한다.
  `Messages(...)` 스타일의 convenience 메서드는 허용되지만, 이들은 공개
  builder 계약 멤버이므로 `Contracts/`에 둔다.
- `IDealerSocket`은 `RequestFrame(...)`이나 `Reply(requestToken, parts)`처럼
  프로토콜 envelope 헬퍼를 노출하지 않는다. dealer는 `Request()`로 request를
  시작할 수 있지만, API 레벨의 peer routing id를 갖지 않으므로 임의 token에
  대해 reply하지는 못한다. Reply는 수신된 request context나, 대상 context가
  명시적인 router/SPOT reply 표면에서 시작한다.
- 메시지 payload 팩토리는 `Message.From(...)` 오버로드를 사용한다. `FromBytes`
  같은 소스 타입 접미사나 `Of` 같은 값 스타일 팩토리는 공개 계약의 일부가
  아니다.
- `SendNoWait`, `PublishWithFlags`, `RequestAsync` 같은 operation-start
  메서드 군을 추가하지 않는다. 하나의 operation 이름을 유지하고 변형은
  builder가 흡수한다. HWM-managed DEALER/ROUTER routed send/request의 awaitable
  terminal builder 메서드는 `Async(...)`로 통일한다.

### DEALER/ROUTER routed 비동기 terminal

- `IDealerSocket.Send()`와 `IRouterSocket.Send(RoutingId)`는
  `RoutedSendOperation`을 반환한다. `RoutedSendSubmitOperation`의 terminal은
  `Task Async(CancellationToken)`뿐이다. 두 socket은
  `IReceivingMessageSocket`과 connect 역할만 상속하며, 공통 역할에는 동기
  `SendOperation`이 없다. 따라서 base interface로 변환해 `Flags(...)`나
  `Submit()`을 호출할 수 없다.
- `IDealerSocket.Request()`와 `IRouterSocket.Request(RoutingId)`의
  `RequestSubmitOperation` terminal은
  `Task<IReadOnlyList<Message>> Async(CancellationToken)`뿐이다. 이 두 routed
  builder에는 blocking submit, polling 결과, `Flags(...)`, callback
  `Submit(...)`을 추가하지 않는다.
- multipart는 반복 `Message(...)` 또는 `Messages(...)`로 누적하고 마지막에
  한 번 `Async(...)`를 호출한다. `Async(...)`는 payload ownership을 operation으로
  옮기고, 호출자 thread를 blocking하지 않은 채 Task를 반환한다.
- 처음 선택된 target은 operation이 끝날 때까지 유지되며, HWM 대기 중 다른
  target으로 reroute하지 않는다.
- HWM credit을 기다리는 동안 호출자 thread나 worker thread를 점유하지 않으며,
  같은 socket의 다른 send를 막는 socket-wide lock도 점유하지 않는다.
- 한 target의 HWM 대기는 다른 target의 admission을 지연시키지 않는다. 이
  독립성은 특정 routing id에 대한 공개 strict-FIFO 보장을 뜻하지 않는다.
- request는 builder `Timeout(...)`을 Core의 per-request deadline으로 넘기며,
  만료는 Core가 `ZLINK_REQUEST_TIMED_OUT`으로 통지한다. routed send의
  admission deadline은 Core-side per-operation 옵션이다. cancellation, close,
  disconnect, target 연결 종료, timeout, 빠른 reply가 경합해도 Task terminal은
  한 번만 결정된다.
- `CancellationToken`이 취소되면 Task는 취소 상태로 끝나며 await는
  `OperationCanceledException`을 발생시킨다. Routed send가 Core deadline까지
  admission되지 않으면 `ZlinkSubmitException`(`Result == Backpressured`)으로,
  request가 `Timeout(...)`까지 끝나지 않으면
  `ZlinkRequestException`(`Result == TimedOut`)으로 끝난다.
- 호출자가 반환된 Task를 await하지 않거나 참조를 보관하지 않아도 operation 자체는
  취소되지 않는다. payload ownership과 terminal 정리는 위 조건 중 하나로
  operation이 끝날 때까지 유지된다.
- 공개 계약에는 별도 queue capacity나 queue-full 결과가 없으며,
  Framework retry/polling을 요구하지 않는다.
- PAIR, STREAM 등 unrelated 공통 sync data-plane builder는 별도 계약으로 남지만
  DEALER/ROUTER의 HWM-managed routed 경로에서 canonical terminal로 취급하지
  않는다.
- routed send의 완료는 Core send-completion 통지가 구동한다. socket runtime은 첫
  `Async(...)` 시점에 `zlink_send_complete_handler`를 한 번 등록하고, 이후 모든
  operation은 complete record 하나를 `zlink_send_async`로 넘긴다. 바인딩은 park
  queue, readiness 재시도 pump, deadline timer, dispatcher thread를 두지
  않는다 — `send_ready` readiness-hint 시맨틱은 폐지되었다.
- 완료 콜백은 completion 전달만 한다. 콜백 안에서 어떤 socket의 send/publish/
  request도 호출하지 않는다(Core는 그런 호출을 `EDEADLK`로 거부한다).
- Core에 넘긴 완료 delegate 인스턴스는 socket 수명 동안 살아 있어야 한다.
  runtime은 이를 필드와 명시적 `GCHandle` 두 경로로 뿌리내리고, native socket이
  닫힌 뒤에만 해제한다. 수집되면 Core가 해제된 reverse-P/Invoke stub을 호출하게
  된다.
- operation 상태는 `zlink_send_async`의 `userdata`로 전달되는 `GCHandle`이
  살려 두며, 정확히 한 번 도착하는 완료가 그 handle을 해제한다. `SUBMIT_OK`가
  아니면 완료가 없으므로 payload ownership은 즉시 호출자에게 되돌아간다.
- 즉시 admission되는 record는 `zlink_send_async` 안에서 완료가 inline으로
  실행되므로 호출자는 suspend하지 않는다.
- `CancellationToken`은 `zlink_send_async_cancel`로 매핑한다. 취소된 operation도
  정확히 한 번 완료하며(`TERMINAL`/`ECANCELED`), 이미 admission이 확정된
  operation은 취소되지 않고 `ADMITTED`로 완료한다.
- per-operation deadline은 Core-side 옵션(`zlink_send_async_options_t.timeout_ms`)
  이다. 바인딩은 자체 deadline timer를 두지 않는다.
- PUB/XPUB의 `publish`는 이 표면에 포함되지 않는다. Core의 `zlink_send_async`는
  PUB/XPUB에서 `ENOTSUP`을 반환하며, publish는 동기 `Submit()`이 terminal이다.

## Contract 폴더 레이아웃

`Contracts/`는 공개 API 맵으로 읽을 수 있어야 한다.

- `Core/`: context, context option, routing id, 유틸리티 리소스 계약.
- `Messaging/`: message, received metadata, topic message, subscription
  event, 공통 send/request/reply operation 계약, message 도메인 convenience
  헬퍼.
- `Sockets/`: socket 동작 계약, socket 능력 인터페이스, 타입화된 옵션 facade.
- `Eventing/`: monitor, monitor snapshot/event, poller, timer, poll event
  계약. 공개되는 경우 static poll 헬퍼도 여기에 포함한다.
- `Service/`: SPOT node, SPOT handle, topology 모델,
  actor ref, actor 생명주기, service 전용 operation builder.
- `Errors/`: 예외 계층과 에러 도메인 매핑.

각 카테고리 안의 파일은 구현 순서가 아니라 사용자 노출 개념에 따라 나뉜다.

- 공통 messaging 연산은 send, request, reply로 나뉘고, service topology 모델은 SPOT node 모델과 공유 topology enum으로 나뉜다.
- Request result는 socket enum 파일이 아니라 messaging request 계약에 속한다.
- 수신 메시지 종류는 받은 메시지 metadata와 함께 둔다.
- SPOT node 모드, socket snapshot, Spot snapshot, actor snapshot은 SPOT node 모델에 속한다.

SPOT은 `ISpot`이라는 단일 핸들 계약으로 유지한다. 호출자가 실제로 그 역할들을
개별로 받아야 하는 경우가 아니라면 역할별 인터페이스로 쪼개지 않는다.

- `ISpotNode`는 node 설정, peer 연결, Spot 생성, Actor 작업, topology 조회 역할을 별도 인터페이스로 나누어 조합할 수 있다. 그래도 기본 생성 경로와 사용자-facing 반환 타입은 `ISpotNode`이며, 역할 인터페이스가 런타임 구현 타입을 노출하면 안 된다.
- SPOT 콜백 등록에는 명명된 콜백 delegate를 사용해 공개 시그니처가 래퍼 context 객체를 추가하지 않고도 콜백의 의미를 기술하도록 한다.
- 등록 메서드는 현재 핸들러가 저장되거나 교체되기 때문에 `Set...Handler` 이름을 쓴다. `On...` 이름은 이벤트가 발생할 때 호출되는 메서드 전용이다.
- 이 delegate들은 SPOT 핸들 계약에서만 사용되므로 `ISpot` 옆에 선언한다.
- Lifecycle data type은 actor 모델과 함께 둔다. 메시지 part를 소유하는 lifecycle event envelope는 복제 가능한 record가 아니라 sealed class로 둔다.
- Actor operation 계약은 join, management, session binding으로 나눈다.

사용자 또는 프레임워크 어댑터가 공개 API를 필요로 한다면, 그 API는
P/Invoke나 런타임 브리지 코드를 읽지 않고도 이 폴더에서 발견 가능해야 한다.

## Runtime 폴더 레이아웃

`Runtime/`은 같은 표준 맵을 따라가지만 구현만을 담는다.

- `Core/`: context 생명주기, counter, stopwatch, thread 헬퍼,
  runtime version/capability 조회.
- `Handles/`: 네이티브 리소스 ownership, close 상태, lifetime 검사,
  reference tracking.
- `Messaging/`: multipart 메시지 materialize, request/reply 진행, request
  상태, received 핸들러, topic 인코딩.
- `Sockets/`: socket 기반 클래스, socket 커널, socket 구현, 콜백 어댑터,
  옵션 접근자, 수신 헬퍼, operation 구현 클래스.
- `Eventing/`: poller, timer, monitor 상태, 콜백 전달, 이벤트 materialize
  헬퍼.
- `Service/`: SPOT node, Spot, Actor, topology 변환기,
  service 옵션 지원, service operation 구현.
- `Errors/`: 경계 검증, 네이티브 result 매핑, errno 변환.
- `Buffers/`: routing-id 코덱, payload buffer ownership, copy/borrow 정책,
  snapshot buffer 헬퍼.
- `Options/`: context/socket 옵션 상수, 검증, 런타임 옵션 변환.
- `Native/`: P/Invoke 선언, 플랫폼 로딩, 네이티브 타입 mirror, 마샬링 헬퍼.

런타임 코드는 공개 계약 타입에 의존할 수 있다. 계약 파일은 공개 팩토리/static
facade 연결을 위해 내부적으로 런타임 코드에 위임할 수 있지만, 공개 시그니처가
런타임 구현 세부를 노출해서는 안 된다.

## 생성 진입점

인터페이스는 동작을 정의하고, 생성은 공개 팩토리가 제공한다.

- `Zlink.CreateContext()`는 런타임 context 구현을 만든다.
- `Zlink.CreateAtomicCounter()`, `CreateStopwatch()`, `CreateThread(...)`는
  공개 계약을 통해 유틸리티 리소스를 만든다.
- `IContext.CreatePairSocket()`, `CreateDealerSocket()`,
  `CreateRouterSocket()`, `CreatePubSocket()`, `CreateSubSocket()`,
  `CreateXPubSocket()`, `CreateXSubSocket()`, `CreateStreamSocket()`은
  런타임 socket 구현을 만든다.
- `IContext.CreateSpotNode()`와 `CreateSpotNode(SpotNodeMode)`는 서비스 계층
  구현을 만든다.
- `Spot` 핸들은 `ISpotNode.CreateSpot()`, `ISpotNode.EntrySpot()`,
  `ISpotNode.GetOrCreateSpot(...)`, `ISpotNode.SpotLookup(...)`을 통해 얻는다.
  `Spot`을 직접 생성하는 것은 공개되지 않는다. `GetOrCreateSpot(...)`은
  `zlink_spot_node_spot_get_or_new(...)`로 직접 사상되며, lookup과 create를
  관리 코드에서 조합해 구현하지 않는다.
- `Actor` 핸들은 `ISpotNode.CreateActor(...)`로 만든다. Actor를 직접 생성하는
  것은 공개되지 않는다.
- `Zlink.CreatePoller()`, `Zlink.CreateTimer()`, `Zlink.CreateTimer(ISpot)`은
  eventing 리소스를 만든다.
- `Zlink.Version()`, `Zlink.Has(...)`, `Zlink.Strerror(...)`, `Zlink.Proxy(...)`,
  `Zlink.Sleep(...)`, `Zlink.MultipartClose(...)`,
  `ZlinkPoll.Poll(...)`은 공개 static facade다. 네이티브 호출이 `Runtime/`에
  남아 있더라도 이들의 호출 가능 동작은 계약 표면의 일부다.

팩토리 반환 타입은 호출자가 구체 런타임 타입을 필요로 하지 않는 곳에서는
공개 계약을 우선한다.

## 필수 기능 커버리지

.NET 공개 계약은 안정 상태의 사용자 노출 코어 기능 전부를 포괄한다. 형태는
C보다 좁거나 더 관용적일 수 있지만, 의미는 동일하게 유지한다.

- context 생명주기, 옵션, shutdown, auto-HWM 재계산, version, 역할,
  strerror 헬퍼.
- 메시지 ownership, multipart payload, routing id, received metadata, topic
  메시지, subscription 이벤트.
- pair, dealer, router, pub, sub, xpub, xsub, stream socket.
- 공통 옵션, 타입화된 socket 옵션, TLS, bind/connect/disconnect, routing id,
  channel name, request/reply, publish/subscribe, 콜백 표면.
- socket monitor, monitor event/snapshot, poller, poll event, timer,
  SPOT과의 timer 통합.
- SPOT node, SPOT handle, topology snapshot, actor ref,
  actor 연산, actor 생명주기, stream actor binding.
- submit, request, recv, handler, close, bind, connect, config 실패에 대한
  타입화된 예외.

part 루프, 콜백 userdata, interop 마샬링, request 진행을 보조하기 위해서만
존재하는 네이티브 헬퍼 함수는 internal로 유지한다.

`Message` receive wrapper는 owner `Received` 또는 `TopicMessage`가 part reference를
제거하고 `Dispose()`를 완료한 뒤 bounded thread-local pool에 반환될 수 있다. Deterministic
cleanup이 반환되면 그 `Message` reference는 무효다. 반복 `Dispose()`, payload 접근,
object identity 기반 dictionary 조회를 포함해 다시 사용하면 안 된다. 반환되지 않은 wrapper와
호출자가 직접 생성한 owned `Message`는 다른 ownership에 재사용하지 않는다.

## Receive 및 Subscribe 형태

.NET의 recv 계열 데이터 플레인 API는 할당 없는 drain을 위해 호출자가 제공한
출력 저장소를 사용한다.

- message/routed receive는 `Received.Create()`로 만들어진 호출자 제공
  `Received` 객체를 채우고 `bool`을 반환한다.
- raw `SUB` / `XSUB`와 SPOT subscribe는 호출자 제공 `TopicMessage`나
  `SubscriptionEvent` 객체를 채우고 `bool`을 반환한다.
- `TopicMessage.ReleaseForReuse()`는 현재 part와 metadata를 해제하고 내부 topic 수신
  버퍼를 유지해 다음 `Subscribe`에서 재사용하게 한다. `TopicMessage`가 열린 상태에서만
  사용할 수 있으며 terminal `Dispose()` 뒤에는 `ObjectDisposedException`을 던지고 객체를
  다시 열지 않는다.
- 일반 `Recv(Received, ...)`와 `Subscribe(TopicMessage, ...)`는 기존처럼 dequeue 시
  Core queue credit을 즉시 반환한다.
- Framework backend는 `RecvRetained(Received, ...)`와
  `SubscribeRetained(TopicMessage, ...)`를 명시적으로 선택한다. 이 경로는 기존 framing과
  metadata를 유지한 채 caller-visible physical part마다 Core retained-credit lease를
  내부에서 함께 인수한다. `Received`는 `Dispose()` 또는 같은 저장소로 다음 receive를
  시작할 때, `TopicMessage`는 `Dispose()`, `ReleaseForReuse()` 또는 같은 저장소로 다음
  subscribe를 시작할 때 현재 part와 모든 lease를 정확히 한 번 해제한다. 내부 finalizer는
  누락 방지용 fallback일 뿐이며 정상 경로는 deterministic cleanup을 먼저 수행한다.
  Payload를 복사해 보관한 값은 lease를 소유하지 않는다.
- Retained receive는 `Received`와 `TopicMessage` aggregate 경로의 내부 수명 동작이다.
  Part 단위 primitive와 일반 aggregate receive는 기존 동작을 유지하고, public lease handle이나
  별도 application capacity를 추가하지 않는다. Dealer message type과 request sequence,
  Router source RID와 request sequence, SUB topic과 source RID는 기존 typed receive 결과를
  그대로 보존한다.
- `false`는 `RecvFlags.DontWait`를 사용한 nonblocking receive에서만 데이터
  없음을 의미한다.
- 실제 receive 실패(데이터 없음이 아닌 실패)는 `ZlinkRecvException`을 던진다.
- monitor recv나 timer recv 같은 제어 플레인 API는 데이터 없음이 자연스러운
  값 형태인 경우 nullable 반환 형태를 유지할 수 있다.
- `RecvActorJoin(...)` 같은 서비스 제어/admission API도 nullable 반환 형태를
  유지할 수 있다. 이들은 데이터 플레인 drain API는 아니지만, 데이터 없음과
  실제 receive 실패(데이터 없음이 아닌 실패)는 구분해 유지한다.

SPOT의 `SubscribeReadable`과 `RoutedReadable` dispatch 이벤트는 readiness
알림이다. 호출자는 일치하는 receive API를 데이터 없음이 보고될 때까지 drain한다.

## Service 및 SPOT 형태

SPOT은 서비스 계층 API이며, raw socket의 누출이 아니다.

- `ISpotNode`는 node 생명주기, route identity, peer 연결,
  route bridge/channel coordination, 외부 pub ingress 부착, topology snapshot, spot 생성,
  actor 생성을 소유한다.
- `ISpot`은 SPOT topic publish/subscribe, routed send/request/reply,
  routed receive, dispatch 이벤트, actor join receive/reply, actor 생명주기
  콜백을 소유한다.
- `Spot.Publish(topic)`는 소유 node의 SPOT topic 플레인에 들어간다. raw `PUB`
  socket을 노출하거나 선택하지 않는다.
- `Spot.Publish(topic)`는 수신자가 이미 publish 가능한 `Spot`이므로 짧은
  publish 이름을 유지한다. 바인딩 계약에서 `PublishSpot`이나 `PublishToTopic`
  으로 이름을 바꾸지 않는다.
- channel을 대상으로 하는 SPOT 연산은 `SendToChannel(...)`과
  `RequestToChannel(...)`을 사용해 목적지를 가진 send/request 이름이
  `SendToSpot(...)`, `RequestToSpot(...)`, `RequestToRouter(...)`와 정렬되도록
  한다.
- Actor location과 stream session binding은 서로 독립적이다. actor가 사용자
  Spot에 join하기 위해 bound stream session이 반드시 필요하지는 않다.

## Byte HWM 및 monitoring ABI v4

- HWM은 queue의 message 수가 아니라 Core가 계산한 accounted byte의 상한이다.
- 공개 타입은 Core의 `uint64_t` 범위를 줄이지 않는 `ulong`이다.
- `0`은 무제한이고 수동 기본값은 `4_096_000 bytes`다.
- Binding은 정확히 8-byte 값으로 Core를 호출한다.
- 이전 `int` overload, alias 또는 count 단위 adapter는 제공하지 않는다.

```csharp
public interface IContextOptions
{
    ulong CoreHwmMemoryLimitBytes { get; set; }
    ulong CoreHwmBudgetBytes { get; set; }
    CoreHwmProfile CoreHwmProfile { get; set; }
}

public interface IContext
{
    CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot();
    void ResetCoreHwmBudgetMetrics();
}

public partial class CommonSocketOptions
{
    public ulong SendHighWaterMark { get; set; }    // outbound accounted byte 상한이다.
    public ulong ReceiveHighWaterMark { get; set; } // inbound accounted byte 상한이다.
}
```

입력 우선순위는 수동 Core budget, 명시 memory limit, .NET GC가 보고한 사용 가능
memory limit hint, Core fallback 순서다. 앞의 두 값을 지정하면 GC hint를 자동 감지하지
않는다. Binding은 hint와 Core hard limit을 직접 결합하지 않는다. 명시 입력이 Core가
감지한 finite hard limit보다 크면 `EINVAL`에 대응하는 기존 config exception을 그대로
전달하고 clamp하지 않는다.

Core는 memory limit에 profile 비율을 정확히 한 번 적용하거나 명시 Core budget을 그대로
사용해 physical directional queue별 planned byte HWM을 계산한다. Caller가
`SendHighWaterMark`나 `ReceiveHighWaterMark`를 설정한 방향은 수동 override가
되며 이후 Auto-HWM 재계산이 그 값을 변경하지 않는다.

.NET 바인딩은 queue의 message나 payload를 다시 세지 않는다. Core pipe의 실제
accounted byte가 applied HWM에 도달하면 native submit 결과가 backpressure를
나타내고, .NET operation은 기존 result·timeout 계약에 따라 이를 전달한다.
`0UL`은 무제한이다.

`MonitorOpen(events, monitorHwmBytes)`는 monitor queue의 정확한 `ulong` byte 값을
받는다. `0UL`은 Core monitor 기본값을 선택하고, 양수는 변환 없이 전달한다.
Message-count overload나 alias는 없다.

- `MonitorStatus`는 native `zlink_monitor_status_t` ABI version 4와 같은 field를 제공한다.
- Planned, applied, deferred HWM과 in-flight 사용량은 모두 `ulong` byte 값이다.
- Deferred 값은 대응하는 `AutoHwmDeferredSendHighWaterMarkValid` 또는 `AutoHwmDeferredReceiveHighWaterMarkValid`가 `true`일 때만 유효하다.
- Pending message 값은 `SndPendingMsgs`와 `RcvPendingMsgs`라는 count 진단값으로 유지하며 byte field와 이름을 공유하지 않는다.
- Pending byte는 `SndPendingBytes`와 `RcvPendingBytes`로 별도 노출한다.
- Snapshot의 `AbiVersion`이 `4`가 아니거나 `StructSize`가 binding layout과 다르면 `NotSupportedException`을 발생시킨다. 이전 monitoring layout은 받지 않는다.

`CoreHwmBudgetSnapshot`은 ABI version/size, configured/runtime/resolved memory limit,
configured/effective budget, planned/applied/manual-reserved HWM, Core queue/application/current/
peak/provisional accounted byte, completion current/peak/pending과 total messaging byte,
monitor/instance aggregate, application/completion queue count,
`OutstandingApplicationLeaseCount`, `RetiredQueueCount`, `DeferredOriginCreditBytes`,
oversize·blocked·aggregate flag, `BudgetGeneration`과 `MeasurementEpoch`을 단위 변환 없이
제공한다. Reset은 current·pending·queue count와 위 세 owner-lifecycle gauge를 유지하고 두
peak를 current로 재기준화하며 epoch counter를 0으로 만든 뒤 `MeasurementEpoch`을
증가시킨다. ABI version/size가 맞지 않으면 `NotSupportedException`이다.

Request/reply API는 HWM 값을 인자로 받지 않는다. `Async(...)`는 선택한 exact
target의 HWM credit을 기다리는 동안 request의 원래 deadline을 유지한다.
호출자는 별도 retry나 polling을 구현하지 않는다.

## Receive flow state

이 바인딩은 Core의 receive-flow 상태를 `ReceiveFlowState` enum으로 노출한다.
`Running = 0`, `Paused = 1`이며 설정은 `ISocket.SetReceiveFlowState(ReceiveFlowState)`다.
반환형은 `void`이고 .NET 에러 정책을 따른다. 0이 아닌 native `zlink_config_result_t`는
해당 `ConfigResult`를 `ErrorCode`로 담은 `ZlinkConfigException`으로 던지므로, completion
lane이 없는 socket은 `ConfigResult.NotSupported`를 담은 `ZlinkConfigException`을 발생시킨다.
이미 유지하는 상태를 다시 설정하면 정상 반환한다.

관측 표면은 C 계약을 따르며 상수와 metric 이름은 C 계층이 확정한다. Monitor event
`SEND_FLOW_PAUSED`, `SEND_FLOW_RESUMED`, `FLOW_STATE_STALE`(`1 << 16`, `1 << 17`,
`1 << 18`, 전체 mask `0x7FFFF`), event flag `SEND_FLOW_WRITABLE`(`1 << 1`),
`FLOW_STATE_STALE_GENERATION`(`1 << 2`), `FLOW_STATE_STALE_EPOCH`(`1 << 3`), status detail
bit `FLOW_STATE`(`1 << 5`), status field 5개 `flow_paused_connections`,
`flow_pause_applied_total`, `flow_resume_applied_total`, `flow_state_stale_total`,
`flow_pause_duration_ms`를 이 언어의 이름 규칙으로 투영한다.

Flow-state frame은 Core 안에 머문다. 바인딩은 setter를 호출하고 monitor event와 snapshot
field를 읽을 뿐, flow-state frame을 직접 encode, decode, 송신 또는 수신하지 않는다.

## 에러 및 검증 정책

- 고정 크기 네이티브 경계 값은 core를 호출하기 전에 검증한다.
- 잘못된 routing id, actor id, endpoint, channel name, topic은 truncation이
  발생하기 전에 .NET argument/config 예외를 발생시킨다.
- submit, request, recv, handler, close, bind, connect, config 에러는
  타입화된 zlink 예외로 사상된다.
- 타입화된 zlink 예외의 공개 생성자는 성공 값인 `Ok`를 받으면 안 된다. `Ok`
  enum 멤버는 native result mirror로 남기되, public constructor는 실패 코드만
  받는다. native errno를 함께 받는 생성자는 runtime 내부 변환용이며 public
  surface가 아니다.
- 데이터 없음과 일시적 backpressure는 일반 예외로 보고하지 않는다.
- 공개 API는 호출자가 네이티브 errno를 직접 검사하도록 요구하지 않는다.

## 성능 정책

- hot path에서는 reflection, dynamic invocation, 반복 boxing, 피할 수 있는
  할당, 피할 수 있는 버퍼 복사, 숨은 sleep, busy wait, thread join, 광범위한
  락을 사용하지 않는다.
- 네이티브 interop은 core part 기판에서 직접 관리되는 `Message`, `Received`,
  `TopicMessage` 값을 만든다. 공개 호출자 소유 `Received` 버퍼는
  `Received.Create()`로 만든다.
- 반복 publish를 drain하는 호출자는 현재 소비자가 작업을 마친 뒤
  `TopicMessage.ReleaseForReuse()`를 호출할 수 있다. 이 방식은 현재 message part의
  ownership을 바꾸지 않으면서 topic 수신 버퍼의 재할당을 줄인다.
- request 진행은 가능한 경우 핸들 단위로 공유한다. request마다 polling
  thread나 timer를 새로 만들지 않는다.
- perf, 샘플, 프레임워크 어댑터는 공개 계약과 생성 진입점만 사용한다.

## 구현 체크리스트

.NET 바인딩이 정렬되었다고 선언하기 전에:

- `Contracts/`는 사용자와 프레임워크 어댑터가 필요로 하는 모든 공개 동작을
  노출한다.
- `Runtime/`은 숨겨진 사용자 노출 API를 추가하지 않고 그 계약을 구현한다.
- 구체 값 타입은 구체로 유지된다.
- 기본 생성 경로는 문서화되고 테스트된다.
- 공개 static facade, 확장 헬퍼, builder convenience 메서드는 `Contracts/`
  에서 발견 가능하다.
- recv/sub API는 호출자 제공 저장소 형태를 사용한다.
- 서비스 제어/admission receive 예외는 데이터 플레인의 호출자 제공 저장소와
  다른 부분이 문서화된다.
- perf 의미는 `bindings/c/perf`와 일치한다. private 런타임 단축 경로를 써서
  측정의 의미를 바꾸지 않는다.
- `Contracts/`의 공개 시그니처는 `Runtime/Native/`, raw handle, 네이티브
  struct mirror, request 진행 타입, 런타임 구현 클래스를 노출하지 않는다.
  static facade에서 런타임 코드로의 내부 위임은 허용된다.
- 런타임 클래스가 제2의 계약 표면이 되지 않는다.
- 프레임워크 어댑터는 공개 바인딩 API를 직접 호출한다.
- 옛 alias, 중복된 operation-start 이름, 호환성만을 위해 보존되는 deprecated
  래퍼가 남아 있지 않다.

.NET 바인딩 변경 후 필수 검증. 다음 명령들을 `bindings/dotnet/`에서 실행한다.

- `dotnet test Zlink.sln` 또는 저장소의 현재 .NET 바인딩 테스트 솔루션을
  실행한다.
- `./tests/run_tests.sh`를 실행한다.
- 공개 예제나 생성 경로가 바뀐 경우 `./samples/run_samples.sh`를 실행한다.
- hot path, receive, send, request, poller, timer, service 동작이 바뀐 경우
  `./perf/run_benchmarks.sh`와 `./perf/run_benchmarks_multi.sh`를 smoke
  게이트로 실행한다.
- 프레임워크 어댑터, 샘플, perf, 테스트에서 reflection, `NonPublic`,
  `InternalsVisibleTo`, `Runtime.Native`, raw handle 사용, 직접적인 request
  펌프 접근이 있는지 검색한다.

## Actor 및 Spot Route 결과

`.NET`은 route lookup 결과를 공개 계약 record로 노출한다.

- `ActorRoute`는 resolve된 `ActorRef`, `Actor.NodeRid`, `CurrentSpotRid`,
  `CurrentSpotKind`를 보존한다.
- `SpotRoute`는 `SpotRid`, `OwnerNodeRid`, `SpotKind`를 보존한다.
- `SpotKind`는 Entry Spot과 사용자 Spot을 구분한다. 잘못된 kind는 성공한
  route 결과가 아니다.
- `SpotNodeSpotEntry`와 `SpotNodeActorEntry`는 코어 snapshot과 동일한 Spot
  kind/현재 Spot 필드를 노출한다.

- 바인딩은 resolve된 Actor ref를 인자로 받는 `ISpotNode.SendToActor(ActorRef)`와 `ISpotNode.RequestToActor(ActorRef)`를 노출한다.
- `SendToActor`는 submit이 성공하면 하나 이상의 message part 소유권을 넘기고, Actor 소유자 mailbox가 인계를 받으면 완료된다.
- `RequestToActor`는 submit이 성공하면 요청 part의 소유권을 넘기고, Actor handler가 만든 reply part를 task 또는 callback으로 전달한다.
- 바인딩은 제거된 Discovery route table이나 resolver API를 compatibility helper로 되살리면 안 된다.
