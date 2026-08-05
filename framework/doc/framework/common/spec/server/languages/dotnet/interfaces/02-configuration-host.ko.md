# .NET 시스템 구조와 host 등록

[.NET exact interface 목차](README.ko.md) · [Topology configuration](03-configuration-topology.ko.md)

## 1. 범위

이 문서는 ZLink Framework를 ASP.NET Core host와 DI에 등록하는 계약을 정의한다. RouteMesh builder,
ChannelName membership, manual peer와 runtime option의 정확한 시그니처는
[Topology configuration](03-configuration-topology.ko.md)이 소유한다. Handler, context와 messaging client의
시그니처는 [exact interface 목차](README.ko.md)의 기능별 문서가 소유한다.

## 2. Package 경계

| package | 책임 |
|---|---|
| `Zlink.Framework` | handler, context, call을 포함한 Server application contract와 [RouteMesh](../../../../01-glossary.ko.md#routemesh), Spot, Actor, STREAM session, location runtime |
| `Zlink.Framework.Contracts` | Server와 HTTP client가 함께 사용하는 codec·오류 contract |
| `Zlink.Framework.AspNetCore` | `IServiceCollection` 등록과 host lifecycle 연결 |
| `Zlink.Framework.Codecs.Protobuf` | 선택 Protobuf codec extension |
| `Zlink.Framework.Codecs.MessagePack` | 선택 MessagePack codec extension |
| `Zlink.Framework.Locations.Redis` | Redis location store extension |

## 3. Host 등록

ASP.NET Core 진입점은 다음 시그니처다.

```csharp
public static class ServiceCollectionExtensions
{
    public static IServiceCollection AddZLinkFramework(
        this IServiceCollection services,
        Action<IZLinkFrameworkOptions> configure);
    public static IHealthChecksBuilder AddZLinkDrainHealthCheck(
        this IHealthChecksBuilder builder);
    public static IServiceCollection AddZLinkHttpClient(
        this IServiceCollection services,
        string name,
        Action<ZLinkHttpClientBuilder> configure);
}
```

한 `IServiceCollection`에 framework root를 한 번 등록한다. `IZLinkFrameworkOptions`의 정확한 멤버는
[Topology configuration §2](03-configuration-topology.ko.md#2-등록-인터페이스)가 소유한다.

Host startup은 구성 검증과 public listener 준비가 완료되어 application callback을 받을 수 있을 때
정상 완료한다. Application callback은 handler와 owner queue가 준비된 뒤에만 실행한다. Hosting stop은
`IZLinkFrameworkRuntime.ShutdownAsync(...)`를 호출한다. Application이 logical continuity를 요구하면 stop 전에
`RelocateAsync(...)`의 `Relocated` 결과를 확인한 뒤 `ShutdownAsync(...)`를 호출한다.

Application version을 유지하는 계획 점검에서는 `PlannedMaintenance`를 사용한다. 이 mode는 source와
version이 정확히 같은 node만 target으로 선택한다. 준비한 새 version으로 전환할 때는
`RollingUpdate`와 source보다 큰 exact target version을 함께 지정한다. 요청한 version의 eligible node가
없으면 Framework는 deadline까지 기다린 뒤 `Blocked/TargetUnavailable`을 반환하며 다른 version으로
자동 전환하지 않는다. 정확한 option과 결과 타입은
[Host monitoring](10-topology-monitoring.ko.md)이 소유한다.

## 4. DI public service

Framework를 등록하면 다음 service가 public DI surface로 제공된다.

| service | lifetime | 책임 |
|---|---|---|
| `IZLinkRouteClient` | singleton | Node direct와 [ChannelName](../../../../01-glossary.ko.md#channelname) send/request |
| `IZLinkSpotClient` | singleton | global SpotId direct send/request와 명시적 Instance cold activation |
| `IZLinkSpotManager` | singleton | User Spot 생성, resolve와 exact 종료 |
| `IZLinkSpotPublisherClient` | singleton | [Spot](../../../../01-glossary.ko.md#spot) Logical Multicast publish |
| `IZLinkFanoutClient` | singleton | classic fanout ChannelName에 typed event publish |
| `IZLinkActorClient` | singleton | global ActorId direct send/request |
| `IZLinkActorManager` | singleton | Actor 생성, resolve와 종료 |
| `IZLinkRouteMeshRuntimeOptions` | singleton | Mesh placement weight와 ChannelName [weight](../../../../01-glossary.ko.md#weight) 조회·설정 |
| `IZLinkFrameworkRuntime` | singleton | host state, readiness, `Relocate`와 `Shutdown` |
| `IZLinkRouteMeshRuntime` | singleton | RouteMesh 운영 status |
| `IZLinkClientServerRuntime` | singleton | ClientServer Channel 운영 status |
| `IZLinkFanoutRuntime` | singleton | automatic fanout Channel 운영 status |

등록되지 않은 MeshName이나 runtime capability를 조회하면 `ZLinkConfigurationException`이 발생한다.
Channel send/request의 등록되지 않은 ChannelName은 `NotFound`로 완료한다.
Spot handler는 Spot activation scope에서, Actor handler는 Actor activation scope에서
생성한다. Handler type을 DI에서 직접 resolve하지 않으며 constructor dependency만 해당
scope에서 resolve한다. Handler가 service를 사용할 때는 context를 service locator로
사용하지 않고 constructor injection을 사용한다. 자세한 수명은
[Spot interface](05-spots.ko.md)를 따른다.

## 5. Location Store 등록

자동 discovery, 분산 Spot·Actor address, Instance Spot activation 또는 Actor relocation을 사용하는 host는
`IZLinkLocationStore` 구현을 root에 명시적으로 등록한다. 아래 코드는 Framework가 제공하는 공식 Redis
provider를 사용하는 예다. Application은 같은 public interface를 구현하는 다른 provider도 등록할 수 있다.

```csharp
services.AddZLinkFramework(options =>
{
    options.AddLocationStore(
        new ZLinkRedisLocationStore(redisOptions)); // Opaque location record의 atomic batch를 제공한다.
    options.AddRelocationStore(
        new ZLinkRedisRelocationStore(relocationOptions)); // immutable relocation payload를 별도 capability로 보관한다.
});
```

Redis 전용 registration helper는 제공하지 않는다. Root의 `AddLocationStore(...)`와 `AddRelocationStore(...)`는
각 interface instance를 하나씩 받는다. Location instance는 exact read, conditional atomic batch와 bounded
snapshot scan을 제공한다. Relocation instance는 Framework가 발급한 reference에 immutable payload를 저장한다.
한 instance가 두 capability를 함께 구현하는 것을 공식 Redis 계약으로 제공하지 않는다.

Manual peer만 사용하고 분산 location 기능을 사용하지 않는 [MeshNode](../../../../01-glossary.ko.md#meshnode)는 [location store](../../../../01-glossary.ko.md#location-store) 없이 시작할 수 있다.
Manual peer도 [MeshName](../../../../01-glossary.ko.md#meshname), RID, lifecycle generation, ChannelName set과 security identity admission을 통과한다.

## 6. Codec

Typed handler와 client는 업무 객체를 주고받는다. Framework는 JSON serializer를 기본으로 제공하므로 JSON을
사용하기 위한 message-specific registration API는 없다. Protobuf, MessagePack과 사용자 codec은 root의
codec registry에 extension 단위로 한 번 등록한다.

Codec은 payload와 업무 객체의 변환만 담당한다. Packet name, metadata, routing과 reply correlation은
Framework가 소유한다. [Packet name](../../../../01-glossary.ko.md#packet-name)은 handler registration descriptor에서 결정하며 codec을 바꾸어도
dispatch key는 바뀌지 않는다.

## 7. Startup validation

Host는 network bind 전에 다음 조건을 검증한다.

- framework root와 MeshName의 중복
- MeshNode의 [routing ID](../../../../01-glossary.ko.md#routing-id)와 listener 설정. Server [membership](../../../../01-glossary.ko.md#membership)은 0개일 수 있다
- ChannelName의 `Client()`·`Server()` 역할과 process-local topology 중복
- ClientServer automatic discovery에 필요한 location store
- wildcard BindHost를 사용할 때 connect 가능한 AdvertiseHost
- 같은 owner namespace의 handler key 중복
- Spot, Actor와 STREAM factory의 owner 관계
- Object role의 중복 선택, Client·Server role의 Location Store 등록과 None role의 [factory](../../../../01-glossary.ko.md#factory) 부재
- Actor·User Spot·[Instance Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot)의 stable type·구현 class 중복, explicit relocation policy와 type별 capacity
- Node placement weight와 active/pending capacity
- Host `ApplicationVersion` 범위와 `MaintenanceWave` 형식
- `PreserveStateWith`의 Actor·Spot adapter type과 factory 대상의 일치 여부
- `RecreateOnRelocation` 또는 `PreserveStateWith` factory가 하나라도 있거나 Instance Spot factory가 하나라도 있을 때 Relocation
  Store가 정확히 하나 등록되었는지 여부
- 자동 discovery 또는 분산 location 기능에 필요한 store instance
- [automatic discovery](../../../../01-glossary.ko.md#automatic-discovery)·object role과 fixed routing ID의 잘못된 조합, RID prefix 형식
- TLS certificate, key와 trust 설정

검증 실패는 `ZLinkConfigurationException`으로 host startup을 실패시킨다. Runtime을 first call에서 만들지
않으므로 구성 오류가 message 처리 중에 처음 나타나지 않는다.

## 8. Runtime option

Mesh placement weight와 Channel weight의 public runtime option은
[Topology configuration §5](03-configuration-topology.ko.md#5-publisher와-runtime-option)가 소유한다. 실행 중에는
MeshName으로 node placement weight를, ChannelName으로 local Server의 `Weight`를 설정할 수 있다. 두 값은
서로 다른 selection에 적용한다. `MaxMessageSize`를 포함한 transport
option은 startup 전에만 설정하며 runtime setter를 제공하지 않는다.

Framework service liveness는 application traffic과 무관하게 5초마다 probe를 보내고 같은 current connection의
matching ACK를 15초 안에 받아야 하는 profile로 고정한다. 다른 inbound frame은 ACK deadline을 충족하지
않는다. 이 값을 바꾸는 C# public option은 제공하지 않으며 owner lease renew interval과 같은 설정으로
취급하지 않는다.

[Logical Multicast](../../../../01-glossary.ko.md#logical-multicast) publisher는 publish 전용 전달 정책 option을 제공하지 않는다. 각 remote target은
MeshNode ROUTER의 HWM과 send timeout을 따르고, local Spot queue는 독립적으로 수락하거나 drop한다.
