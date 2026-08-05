<!-- framework-adapter-nav:start -->
[가이드 홈](../index.ko.md) | [다음: ZLink Framework for .NET — 개요](guide/server/01-overview.ko.md)
<!-- framework-adapter-nav:end -->

[가이드 홈](../index.ko.md) | [공통 스펙](../common/README.ko.md)

[공통 스펙](../common/README.ko.md) | [비동기 실행](../common/spec/05-async-execution-policy.ko.md) | [Exact interface](../common/spec/server/languages/dotnet/interfaces/README.ko.md) | [Stream Connector](../common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md) | [Unity 가이드](guide/stream-connector/02-unity.ko.md) | [공통 내부 구조](../common/internals/README.ko.md) | [Regression Matrix](internals/regression-test-matrix.ko.md) | [Backend Policy](internals/backend-dependency-policy.ko.md)

# ZLink Framework for .NET

> **이 디렉토리가 `.NET` 문서의 진입점이다.** 세 패키지의 사용 안내를 한자리에 둔다 —
> **계약은 패키지로 나누고, 사용 안내는 언어로 나눈다.**
>
> | 디렉토리 | 무엇 |
> |---|---|
> | [`guide/server/`](guide/server/01-overview.ko.md) | **framework(서버)** 사용 가이드 |
> | [`guide/http-client/`](guide/http-client/README.ko.md) | **HTTP client** 사용 가이드 |
> | [`guide/stream-connector/`](guide/stream-connector/README.ko.md) | **Stream connector** 사용 가이드(Unity·Godot 포함) |
> | [`internals/`](internals/regression-test-matrix.ko.md) | 구현·검증 기준 |
>
> **공개 계약은 여기 없다.** [spec 트리](../common/spec/README.ko.md)가 소유한다 —
> [server/languages/dotnet](../common/spec/server/languages/dotnet/README.ko.md) ·
> [http-client/languages/dotnet](../common/spec/http-client/languages/dotnet/dotnet-http-client.ko.md) ·
> [stream-connector/languages/dotnet](../common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md).
> 가이드와 계약이 어긋나면 **계약이 이긴다.**

## 1. 목적

이 문서는 `.NET` 바인딩 위에 올려 둘 `ZLink Framework`의 `.NET` 표면을 정리한다.
다루는 축은 다음 세 가지를 우선으로 한다.

- channel 이름을 기준으로 한 request / send 와 event messaging[^channel-messaging]
- `SPOT`[^spot]을 `ASP.NET Core` 애플리케이션에서 다루는 방법
- location store[^location-store] 를 등록해 peer, spot, actor 위치를 공유하고 운영 상태를 조회하는 방법

Framework는 Channel, Spot, Actor와 STREAM service runtime을 직접 구현한다. `.NET` 바인딩에서는
`DealerSocket`, `RouterSocket` 같은 public raw socket API만 사용한다. 사용자에게는 DI,
hosted service[^hosted-service], handler 모델과 Location Store 기반 자동 연결을 노출하며 raw socket
배선은 Framework 내부에서 처리한다.

현재 구현 backend 는 `bindings/dotnet` 을 그대로 쓴다. 다만 framework 가
사용자에게 보여 주는 public contract 는 backend 구현체와 분리해서 유지하는 것을
원칙으로 둔다. 자세한 기준은
[backend-dependency-policy.ko.md](internals/backend-dependency-policy.ko.md) 에서 다룬다.

Sample과 E2E의 설정 파일, 환경 변수 금지와 Options binding 기준은
[Sample/E2E 설정 정책](../common/sample-e2e-configuration-policy.ko.md)을 따른다.

## 1.1 지원 버전 기준

이 `.NET` 문서는 다음과 같이 버전 기준을 먼저 고정한다.

- 최소 지원 런타임: `.NET 8` (`net8.0`)
- 주 개발 기준: `.NET 10` (`net10.0`)
- 최소 지원 언어 버전: `C# 12`

따라서 이 디렉토리의 문서와 샘플은 위 최소 지원 기준에서 바로 컴파일·실행이
가능한 표면을 우선해서 설명한다. `C# 13`, `C# 14`, `preview`, `latest` 전용
문법이나 API 는 공개 framework 계약의 전제 조건으로 삼지 않는다.

## 1.1.1 CI 플랫폼 기준

이 문서의 CI[^ci] 기준은 특정 OS 하나를 대표 플랫폼으로 두지 않는다. 대신 저장소
안의 `bindings/dotnet/runtimes/` 와 `.github/workflows/build.yml` 이 이미 함께
관리하고 있는 native runtime 범위를 framework 쪽에서도 그대로 따른다.

현재 기준으로 반드시 지원해야 하는 runtime RID[^rid] 는 다음 여섯 가지다.

- `win-x64`
- `win-arm64`
- `linux-x64`
- `linux-arm64`
- `osx-x64`
- `osx-arm64`

따라서 `.NET` framework 의 regression 테스트와 release gate[^release-gate] 도
위 여섯 플랫폼을 모두 통과하는 것을 기본 조건으로 본다.

## 1.2 공통 정책 적용

이 디렉토리의 모든 문서는
[Framework Adapter 정책](../common/README.ko.md) 과 그 하위 문서를 그대로
따른다. 즉 `.NET` 상세 문서는 공통 의미를 새로 정의하지 않는다. 이미 정해진
의미를 `.NET` 과 `ASP.NET Core` 표면에서 어떻게 구체화할지만 다룬다.

특히 다음 항목은 이 디렉토리 전체에 공통으로 적용된다.

- 네이밍 규칙은
  [공통 스펙 README §6.2.1](../common/README.ko.md#621-네이밍-규칙)
  의 `Naming Policy`를 그대로 따른다. `.NET`에서는 public API 전체를
  `PascalCase`로 적고, 단어 구성 자체를 임의로 바꾸지 않는다.
- `Zlink` 와 `ZLink` 의 casing 의도는 다음과 같이 본다.
  - native binding 패키지(`bindings/dotnet/src/Zlink/...`)와 그 안에서
    export 하는 raw transport[^raw-transport] 타입(예: `DealerSocket`,
    `RouterSocket`)은 `Zlink` namespace 아래에 둔다. 즉
    wire/transport[^wire-transport] 레벨이다.
  - framework adapter 표면 타입은 `ZLink` prefix로 통일한다. 예를 들어
    `IZLinkSession`, `IZLinkActorContext`, `IZLinkBoundSession` 같은 형태다.
    즉 framework가 사용자에게 노출하는 모든
    interface, record, enum, exception 은 `ZLink`를 쓴다.
  - 패키지 id 와 namespace 단어(`Systems.Zlink.*`)는 native binding 규칙을
    따른다. 타입 이름의 casing 의도와 namespace 이름의 casing 의도는 서로
    별개다.
- `zlink.systems` 도메인을 기반으로 한 package 와 namespace는 역순 도메인
  규칙[^reverse-dns]을 따른다. `.NET`의 NuGet[^nuget] package id 와 namespace는
  `Systems.Zlink.*`를 사용한다. 예를 들어 framework는
  `Systems.Zlink.Framework`, Stream Connector는 `Systems.Zlink.Stream.Connector`가 된다.
- 수동 연결은 MeshNode의 `PeerConnections`와 fanout subscriber 연결처럼 기능별
  public 표면으로 설명한다. 같은 MeshNode에서는 location store 기반 자동 연결과
  manual peer 연결을 섞지 않는다.
- send 는 기본적으로 async submit 으로 설명한다. backpressure[^backpressure]는
  public no-wait 옵션을 따로 두지 않고, nonblocking send 와 pending queue,
  ready notification 을 활용해서 framework 내부에서 처리한다.
- `CancellationToken` 은 실제로 기다릴 수 있고 그 대기를 취소할 수 있는 public
  async 경계에만 둔다. request / actor join / channel submit / SPOT submit /
  stream connector write 처럼 queue, retry, transport write, reply 대기가 있는
  API 는 token 을 받는다. 반대로 session reply frame 작성, session close, timer
  cancel 처럼 현재 구현이 즉시 완료되거나 자체 종료 토큰으로 정리되는 API 는
  token 을 받지 않는다. token 을 받는다면 시작 전 검사만 하지 말고 실제 대기
  지점에 이어 주어야 한다.
- `SPOT` 을 다루는 문서는 stable type 기준 factory 등록, 전역 `SpotId` 기준 생성과 조회,
  lifecycle timer, 외부 spot publish 표면을 공통
  정책과 맞춰 설명해야 한다.
- Spot·Actor 메시지는 global ID로 보낸다. 현재 owner의 NodeRid와 generation은 Framework가
  Location Store에서 조회한다. Actor join, actor factory 등록과 stream-to-actor
  bridge[^stream-actor-bridge]도 같은 위치 계약을 따른다.
- session actor dispatch[^session-actor-dispatch] 는 단일 gateway feature switch
  하나를 켜고 끄는 형태가 아니다. 대신
  `AddStreamNode` 뒤의 `AddSession<TSession>()`, actor factory, actor
  logical actor binding, actor-session binding, `IZLinkBoundSession` 의
  조합으로 설명한다. session 위치 조회를 위한 별도의 public API 는 두지 않는다.

## 2. 문서 구조와 역할 분담

문서는 **가이드**, **기준 문서**, **주제 문서**, **샘플 문서** 네 가지로 나눈다.
처음 접한다면 가이드부터 읽는다. 정식 계약은 기준/주제 문서가, 실행 코드는
샘플 문서가 다룬다.

### 2.0 가이드 (시작하기)

`guide/server/`는 `.NET`/`ASP.NET Core` 개발자가 각 기능을 **읽고 바로 따라 쓸 수
있도록** 개념과 사용법을 직접 설명한다. 개념의 정식 의미는 공통 스펙이, 정식
계약은 spec 문서가 다루며, 가이드는 그 의미를 실사용 코드로 풀어 준다. 실행
가능한 전체 샘플의 업무 흐름은 [공통 sample](../common/sample/README.ko.md)이 정의한다.

이 가이드 문서를 작성·수정할 때는
[사용자 가이드 문서 작성 가이드](../../../../doc/principal/documentation/guide-writing-guide.ko.md)를
따른다.

읽는 순서와 장별 내용은 [가이드 읽는 순서](guide/server/README.ko.md)가 제시한다.

### 2.1 기준 문서 (interface catalog)

| 문서 | 역할 |
|------|------|
| [interfaces/README.ko.md](../common/spec/server/languages/dotnet/interfaces/README.ko.md) | public interface를 common runtime, host, channel, Spot, Actor, STREAM, location, maintenance와 monitoring으로 나눈 정식 목차 |

### 2.2 주제 문서 (programming model)

각 주제 문서는 프로그래밍 모델과 사용 방향을 설명한다. 인터페이스 전체 정의는 다시 나열하지 않고 exact
interface 목차의 대응 category를 참조한다.

| 문서 | 다루는 범위 |
|------|------------|
| [configuration-host.ko.md](../common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md) | ASP.NET Core host 등록·부트스트랩·DI·lifecycle과 startup validation |
| [interfaces/README.ko.md](../common/spec/server/languages/dotnet/interfaces/README.ko.md) | 전체 public interface·context·handler·client·provider·관측 category 목차 |
| [32-stream-connector.ko.md](../common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md) | 별도 client connector의 lifecycle, dispatch, codec, transport, 종료 사유 |
| [public contract](../common/spec/server/languages/dotnet/README.ko.md) | 문서 계약과 실제 assembly·NuGet 산출물의 exact 검증 절차 |

**기능의 의미와 동작 규칙은 [공통 스펙](../common/spec/README.ko.md)이 소유한다.** 언어별 문서는
그 의미가 `.NET`에서 어떤 모양인지만 고정한다.

### 2.3 유지보수 문서

다음 문서는 public API 사용법이 아니라 backend 경계, 내부 lifecycle과 회귀 검증을
설명한다. 공개 오류와 허용 조합은 각 기능 spec을 따른다.

| 문서 | 다루는 범위 |
|------|------------|
| [공통 내부 구조](../common/internals/README.ko.md) | 네 언어가 공유하는 runtime 아키텍처 결정 |
| [regression-test-matrix.ko.md](internals/regression-test-matrix.ko.md) | 항상 유지해야 할 회귀 테스트 항목, CI 계층, release gate |
| [backend-dependency-policy.ko.md](internals/backend-dependency-policy.ko.md) | backend 의존 관계와 저수준 라이브러리 교체 경계 |
| [public-symbol-delta-v11.ko.md](internals/public-symbol-delta-v11.ko.md) | 내부 이관 0건과 maintenance 최소 public delta 분류 |

### 2.4 샘플 문서

샘플 문서는 등록 코드부터 handler, client 호출까지 한 번에 보여 주는 실행 가능한
코드를 모아 둔다. 인터페이스 정의를 다시 나열하지는 않는다. 기능 선택 기준은
guide가 맡고, sample 문서는 공통 정본 시나리오의 실제 등록·실행 흐름을 보여 준다.

| 문서 | 다루는 범위 |
|------|------------|

### 2.5 범위 원칙

| 개념 | 다루는 곳 | 다른 문서에서는 |
|------|----------|---------------|
| 인터페이스, attribute, context 전체 정의 | [exact interface](../common/spec/server/languages/dotnet/interfaces/README.ko.md) | 교차 참조 |
| channel 등록(AddZLinkFramework), lifecycle | [configuration과 host](../common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md) | 필요할 때 링크만 |
| handler / client 사용 예시, dispatch 흐름 | aspnet-core-channel-messaging, 샘플 | |
| SPOT 개념, 등록, lifecycle | [Spots](../common/spec/server/languages/dotnet/interfaces/05-spots.ko.md) | 필요할 때 링크만 |
| Actor 라이프사이클, session bind, user Spot join, session actor dispatch | [Actors](../common/spec/server/languages/dotnet/interfaces/06-actors.ko.md) | 필요할 때 링크만 |
| location store 등록, 자동 연결, 운영 조회 | [Location](../common/spec/server/languages/dotnet/interfaces/08-location-maintenance.ko.md) | 필요할 때 링크만 |

## 3. 핵심 방향

- `ASP.NET Core` 의 DI 와 hosted service 모델을 따른다.
- handler, client, filter 의 생성도 동일한 `.NET DI` 컨테이너를 기준으로 맞춘다.
- 업무 메시지는 ChannelName, SpotId 또는 ActorId로 대상을 지정한다. Framework가 현재 위치와
  전송 경로를 결정하므로 application은 특정 NodeRid를 선택하지 않는다.
- `(MeshName, target NodeRid)`를 받는 node direct 호출은 Admin·Ops처럼 물리 node 자체가 대상인
  작업에만 사용한다. Actor·Spot 생성과 업무 메시지에는 사용하지 않는다.
- 별도의 gateway나 전용 load balancer를 두지 않고, MeshNode descriptor를 사용하는
  location store 자동 연결로 직접 호출한다.
- ChannelName messaging handler는 해당 membership의 builder에 typed handler로 등록한다. Admin·Ops
  node direct handler는 MeshNode builder에 등록하므로 두 namespace가 섞이지 않는다.
- `[ZLinkRequest]`, `[ZLinkSend]`, `[ZLinkPublish]` 는 channel 이름을 인자로 받지
  않는다. channel 이름은 배포 환경과 topology 의 값이므로, handler attribute 가
  소유하지 않고 channel registration 이 소유한다.
- `SPOT` 도 별도의 low-level runtime 으로 떼어 두지 않고, framework lifecycle
  안에서 다룰 수 있어야 한다.
- 일반 channel messaging은 `IZLinkRouteClient`가 ChannelName에 등록된 process-local 송신 경로를
  사용한다. Ready 상태이고 weight가 양수인 server 중 하나를 선택한다. Admin·Ops node direct
  호출은 같은 client에 MeshName과 target NodeRid를 지정한다.
- `SPOT`의 high-level 표면은 Logical Multicast publish, 전역 `SpotId` 기반 send/request와 actor
  메시징을 다룬다. 호출자가 현재 owner의 NodeRid나 별도 ingress channel을 조합하지 않는다.
- `IZLinkRouteClient`와 `IZLinkSpotOutbound`는 모두 typed payload를 받으며, 주소 해석과
  wire 구성은 framework가 내부에서 처리한다. 호출자는 raw frame이나 transport 종류를
  선택하지 않는다.

## 4. 회귀 테스트

이 묶음의 모든 세부 문서는 회귀 테스트 기준을 함께 설명해야 한다. 그래서 문서가
추가되거나 이름이 바뀌면, 아래 테스트가 다음 세 가지를 함께 갱신했는지 확인한다.

- 문서 목록
- 각 문서의 회귀 테스트 단락
- 대표 테스트 케이스 연결

| 테스트 케이스 | 확인 기준 |
|---------------|-----------|
| `RegressionTests.DotNetContractDocuments_AllExposeRegressionTestSection` | `.NET` 계약·sample·internals 문서마다 `회귀 테스트` 단락이 존재한다. |
| `RegressionTests.DotNetRegressionMatrix_References_AllContractDocuments` | `regression-test-matrix.ko.md`가 현재 검증 대상 문서 파일명을 모두 참조한다. |
| `ScaffoldSmokeTests.FrameworkRoot_IsDiscoverable_FromTestRuntime` | 테스트 runtime 에서 framework 루트를 찾을 수 있어, 문서 회귀 테스트가 저장소 기준으로 실행된다. |

[^public-contract]: public contract 는 외부 사용자에게 공개되어 변경 시 호환성을 책임져야 하는 API 표면을 뜻한다.
[^channel-messaging]: channel messaging 은 채널 이름을 키로 삼아 메시지를 주고받는 방식이다. request / send 는 요청-응답과 단방향 전달, event messaging 은 publish / subscribe 형태의 이벤트 전달을 가리킨다.
[^spot]: `SPOT` 은 동적으로 생성·소멸되는 논리적 실행 단위다. room, stage와 zone이 대표 예다.
[^topology]: topology 는 어떤 노드(channel, spot, location row 등)가 어디에 있는지, 그리고 서로 어떻게 연결되어 있는지를 나타내는 구성 정보다.
[^location-store]: location store 는 서버가 자기 endpoint, routing id와 ChannelName membership을 descriptor row로 적고, 다른 서버가 그 row를 읽어 연결 대상을 찾는 공유 저장소다. Production 구성은 공식 Redis extension이나 `IZLinkLocationStore` 구현체를 명시적으로 등록한다.
[^hosted-service]: hosted service 는 `ASP.NET Core` 호스트가 시작·종료될 때 함께 시작·종료되는 백그라운드 컴포넌트를 뜻한다(`IHostedService`).
[^ci]: CI(Continuous Integration) 는 코드 변경이 들어올 때마다 자동으로 빌드와 테스트를 실행해 회귀를 빠르게 잡아내는 파이프라인을 가리킨다.
[^rid]: RID(Runtime Identifier) 는 `.NET` 이 OS·CPU 조합을 식별하는 문자열이다. 예: `win-x64`, `linux-arm64`.
[^release-gate]: release gate 는 새 버전을 배포하기 전에 반드시 통과해야 하는 검증 단계(테스트, 빌드, 점검)의 묶음을 가리킨다.
[^raw-transport]: raw transport 는 framework 추상화를 거치지 않은 저수준 소켓 계층의 송수신을 뜻한다.
[^wire-transport]: wire / transport 레벨은 실제 네트워크 위에서 바이트가 흘러가는 계층을 가리키며, 그 위에 framework 의 추상화가 쌓인다.
[^reverse-dns]: 역순 도메인 규칙(reverse-DNS) 은 도메인 이름을 거꾸로 뒤집어 namespace 충돌을 피하는 관례다. `zlink.systems` 도메인이면 `Systems.Zlink.*` 가 된다.
[^nuget]: NuGet 은 `.NET` 의 표준 패키지 매니저로, 라이브러리를 package id 단위로 배포·설치한다.
[^capability]: **역할**은 어떤 노드(channel, spot 등)가 외부에 노출하는 기능 단위(예: server, subscriber, publisher)를 가리킨다.
[^backpressure]: backpressure 는 송신 측이 수신 측의 처리 속도를 넘어 메시지를 밀어 넣지 못하도록 흐름을 조절하는 메커니즘이다.
[^stream-actor-bridge]: stream-to-actor bridge 는 STREAM 으로 들어온 외부 트래픽을 framework 내부의 actor 메시지로 이어 주는 연결 지점을 가리킨다.
[^session-actor-dispatch]: session actor dispatch 는 클라이언트 session에서 들어온 요청을, 그 session과 묶인 actor 로 자동 전달하는 패턴이다.
[^fail-fast]: fail-fast 는 잘못된 설정이나 상태를 발견하면 즉시 예외를 던지고 실행을 멈추는 전략이다. 늦게 발견되어 더 큰 문제로 번지는 것을 막는다.
[^attribute-scan]: attribute scan 은 어셈블리에 정의된 타입과 메서드를 훑어 보면서 특정 attribute 가 붙은 항목을 찾아 등록하는 방식이다.

---
<!-- framework-adapter-nav:bottom:start -->
[가이드 홈](../index.ko.md) | [다음: ZLink Framework for .NET — 개요](guide/server/01-overview.ko.md)
<!-- framework-adapter-nav:bottom:end -->
