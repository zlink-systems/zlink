---
title: "1. 개요 · C#/.NET"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/01-overview.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 1. 개요

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [다음: .NET Quickstart — 설치부터 첫 요청까지](../../quickstart.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/01-overview.ko.md) · **C#/.NET** · [Java](../../../java/guide/server/01-overview.ko.md) · [Kotlin](../../../kotlin/guide/server/01-overview.ko.md) · [Node/TypeScript](../../../node/guide/server/01-overview.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    ZLink Framework가 해결하는 문제와 주요 메시징 표면을 구분할 수 있다.
    이 장의 코드는 [언어별 예제 저장소](https://github.com/zlink-systems/zlink-dotnet-examples)에서 가져왔다.

> 이 문서는 `.NET` 가이드의 진입점이다. 가이드는 `ASP.NET Core` 개발자가
> ZLink Framework의 기능을 **읽고 바로 따라 사용할 수 있도록** 개념과 사용법을
> 직접 설명한다. 개념만 먼저 확인하려면 [핵심 개념](03-concepts.ko.md)으로 간다.

## 1. 한 줄 정의

`ZLink Framework`는 **기존 메이저 프레임워크와 통합되는 실시간 메시징
프레임워크**다. Spring 위에 Spring MVC가 웹 계층으로 올라가듯, `ASP.NET Core` 위에
ZLink Framework가 **실시간 메시징 계층**으로 올라간다. 별도 런타임이나 전용 서버로
갈아타는 것이 아니라, 쓰던 DI·hosted service·설정·로깅 모델 안에 그대로 들어온다.

이 계층은 서버 간 호출, pub/sub, 그리고 실시간 상태 단위를 제공한다. 서버 간
호출과 pub/sub는 별도 **gateway나 전용 로드밸런서 없이** 논리 `channel name`만으로
대상을 찾는다. 실시간 상태 단위는 `SPOT`(room · stage · zone), actor(연결·사용자
하나를 대표하는 상태 객체), `STREAM`(외부 client 연결)이다(용어가 낯설면
[03-concepts](03-concepts.ko.md)의 개념 설명을 먼저 본다). 개발자는 HTTP/gRPC를
쓰던 감각으로 **handler, client, filter**를 작성하고, 연결·위치 조회·라우팅·재연결·
correlation은 framework가 처리한다.

> **ZLink는 여러 언어에서 같은 계약으로 쓰는 framework다.** 같은 계층이 Spring
> (Java/Kotlin)과 NestJS(Node) 위에도 똑같이 올라가고, 호출 계약이 언어 중립 wire
> protocol(ZMP) + codec + 논리 channel/packet이라 서로 다른 언어로 구현된 서비스가
> 같은 channel 위에서 상호 호출한다(예: room 서버 C++, API 서버 .NET·Java). 이
> 각 언어의 가이드는 같은 공개 계약을 설명한다. 언어별 사용 예제는 해당 언어의 실제 API를 따른다.

## 2. 도입 판단

ZLink의 적용 상황과 기술 선택 기준은 [적용 범위](17-alternative.ko.md)에서 확인한다.
이 장은 ZLink를 선택한 뒤 사용하는 주요 표면과 구조를 설명한다.

## 3. 표면과 구조

### 3.1 호출 단위 — MeshName과 ChannelName

ZLink Framework의 서버 간 호출은 **`MeshName`과 `ChannelName`**으로 대상을 고른다.
application에서는 "`services` mesh의 `orders` channel로 요청을 보낸다"처럼 사용한다.
어느 노드가 그 channel을 처리하는지는 location store에 등록된 membership을 framework가
확인해 선택한다.

서버 하나를 만들 때 직접 작성해야 했던 것들을 framework가 처리한다.

| 직접 만들어야 했던 것 | framework가 처리하는 방식 |
| --- | --- |
| endpoint 개설·peer 연결 관리 | MeshNode와 STREAM node를 선언하면 hosted service가 연결 |
| 메시지 직렬화·역직렬화 | codec 등록과 handler 계약에 맞춰 DTO를 그대로 주고받음 |
| 요청 routing·dispatch | `ChannelName`의 typed handler 등록으로 메시지가 알맞은 handler에 도착 |
| 로깅·검증·권한 확인 같은 공통 처리 반복 | HTTP route는 middleware, ZLink handler는 `IZLinkHandlerFilter`로 분리 |
| 동시 요청의 상태 보호 | SPOT의 직렬 실행으로 lock 없이 상태 관리 |
| 서비스 생성·의존성 관리 | ASP.NET Core DI에서 handler, client, filter를 생성 |
| 서버 주소 관리·연결 결정 | location store를 통해 현재 활성 endpoint 추적 |
| 설정·로그·모니터링 | ASP.NET Core 설정·logging·hosted service와 통합 |

### 3.2 계층 구조와 등록 지점

<iframe class="zlink-diagram" src="/common/diagrams/01-layers.html" title="계층 구조 — host 위에 ZLink, 그 위에 비즈니스 로직" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-layers.html" target="_blank">↗ 크게 보기</a></p>

기존 host framework(ASP.NET Core · Spring Boot · NestJS · C++ host)에 ZLink Framework를
등록하면 별도 런타임 없이 해당 framework 안에서 실행된다. application은 그 위에
**비즈니스 로직**(Spot · Actor · handler)을 작성한다. Framework는 **DI · hosted service ·
handler · attribute**를 통해 기능을 제공한다.

application이 이 스택과 만나는 지점은 **등록 코드 한 곳**이다. 여기서 location
store, MeshNode, fanout과 STREAM node를 선언한다. 아래는 tutorial의 실제 등록
코드를 그대로 이어 붙인 것이다 — mesh·channel·fanout 이름은 `"services"`·
`"orders"`·`"events"`가 아니라 tutorial의 `"game"`·`"profile"`·`"broadcast"`다.
먼저 location store를 등록한다.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:location-store"
```

다음으로 mesh와 channel을 등록한다.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:mesh-register"
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:channel-register"
```

그다음 fanout을 구독한다.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:fanout-subscribe"
```

마지막으로 STREAM node를 등록한다.

```csharp
--8<-- "framework/languages/dotnet/tutorial/Server/Program.cs:stream-register"
```

gRPC+LB, broker, WebSocket 서버로 각각 직접 구성하던 토폴로지들이 **같은 선언 모델
하나**로 내려온다. location store를 등록했으므로 서버가 늘어나거나 줄어들 때
connection도 자동으로 새로 연결되거나 정리된다 — 설정 파일을 고치거나 LB를
재구성할 일이 없다.
([05](20-channel-messaging.ko.md)·[06](21-spot.ko.md)·[09](23-stream.ko.md)·[10](25-location.ko.md))

무엇을 어디서 선언하는지는 다음 세 자리로 정리된다.

| 표면 | 역할 | 다루는 장 |
| --- | --- | --- |
| `builder.Services.AddZLinkFramework(...)` | channel·SPOT·STREAM 선언 | [5장](20-channel-messaging.ko.md)~[9장](23-stream.ko.md) |
| `options.AddRouteMesh(...)` / `AddFanoutChannel(...)` | RouteMesh·fanout 선언 | [5장](20-channel-messaging.ko.md) |
| `IZLink*Runtime` status | 상태 관측과 진단 | [11장](26-monitoring.ko.md) |

## 4. 통합 4축 요약

<iframe class="zlink-diagram" src="/common/diagrams/01-lang-dotnet.html" title="ZLink 계층 — .NET" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-lang-dotnet.html" target="_blank">↗ 크게 보기</a></p>

| 축 | 사용자에게 보이는 것 | 가이드 챕터 |
| --- | --- | --- |
| channel messaging | `IZLinkRequestHandler`, `IZLinkSendHandler`, `IZLinkRouteClient`, `IZLinkHandlerFilter` | [05-channel-messaging](20-channel-messaging.ko.md) |
| fanout | `AddFanoutChannel`, `IZLinkFanoutHandler` | [05-channel-messaging](20-channel-messaging.ko.md) |
| SPOT | typed spot factory, Spot context outbound, timer | [06-spot](21-spot.ko.md) |
| actor / session | actor factory, Entry Spot, `IZLinkBoundSession`, session actor dispatch | [07-actor-spot](22-actor.ko.md) · [08-actor-session](24-actor-session.ko.md) |
| STREAM | framework session packet, Stream Connector | [09-stream](23-stream.ko.md) |
| 인프라 | Location 기반 자동 연결·운영 조회, runtime monitoring | [10-location](25-location.ko.md), [11-monitoring](26-monitoring.ko.md) |
| 운영 | 런타임 메트릭(`AddMeter` 한 줄), graceful drain, readiness probe | [12-operations](12-operations.ko.md) |

## 5. 전체 topology

각 기능이 어떻게 맞물리는지 보여주는 예시다. 이 지도를 각 기능 장이 확대해 들어간다.

<iframe class="zlink-diagram" src="/common/diagrams/01-topology.html" title="전체 topology" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-topology.html" target="_blank">↗ 크게 보기</a></p>

- **API 서버** - HTTP 요청을 받아 도메인 서버로 넘긴다. 넘기는 길은 client 종류에 따라 달라진다.
    - **channel client** — 이름으로 호출한다. handler 하나가 처리하면 되는 요청은
      **ClientServer channel**로 node handler를 부르고, 상태 단위가 받아야 하는 요청은
      **RouteMesh channel**로 Instance Spot에 보낸다.
    - **spot client** — id로 Spot을 호출한다.
    - **actor client** — id로 Actor를 호출한다.
- **세션 서버** - client의 실시간 연결을 받는다. STREAM node가 받은 메시지를 session relay가
  **RouteMesh channel**로 넘기면, entry spot이 배정한 user spot의 actor가 처리한다.
- **도메인 서버** - node handler와 spot이 상태를 쥐고 요청을 직렬로 처리한다.
- **Location store** - 서버 주소 정보를 관리한다. 점선은 store 조회로 endpoint를 찾는 연결이며
  데이터 경로가 아니다.

**STREAM node를 도메인 서버에 함께 둘 수는 있으나 일반적인 모양은 아니다.** 연결 수와 상태
처리량이 따로 늘기 때문에, HTTP 입구처럼 별도 서버로 두는 구성이 흔하다.

## 6. 다음 문서

설치와 첫 호출은 [퀵스타트](../../quickstart.ko.md), 용어와 계층은
[핵심 개념](03-concepts.ko.md)으로 이어 간다. 전체 학습 순서와 장별 역할은
[서버 가이드 목차](README.ko.md)가 소유한다.
