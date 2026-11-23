[English](./framework-dotnet-0.34.0.md) | [한국어](./framework-dotnet-0.34.0.ko.md)

# ZLink .NET Framework 0.34.0 릴리스 노트

Framework 0.34.0은 .NET binding 1.26.0과 Core 1.26.0을 사용합니다.

## 호환성 변경

- 0.34.0은 relocation 진행 상태를 authority record의 `aggregate.relocation` field에 기록합니다. 0.34.0으로 올리기 전에 진행 중인 relocation을 끝내고, 이 field가 남은 record가 있는 동안에는 0.33.x로 되돌리지 마십시오. 0.33.x는 이 field를 읽지 않습니다. (Location runtime §3.4, #4718, #4730)
- 0.34.0은 언어 간 aggregate relocation(SpotWide User Spot과 marker가 없는 Direct Join)을 지원하지 않습니다. 해당 relocation은 같은 언어의 노드 사이에서만 사용하십시오. 네 언어가 공유하는 §8.3 저장 형식으로의 전환은 0.35.0에 예정되어 있습니다. (#5354, #5422)

### Spot handler 마이그레이션

`IZLinkSpotPacketHandler`와 `IZLinkSpotRequestHandler`를 구현할 때 Spot 인자 바로 뒤에
`IZLinkMessageContext context`를 추가합니다. 송신자가 설정한 읽기 전용 metadata snapshot은
`context.Metadata`에서 읽습니다. (#5224, #5239)

변경 전

```csharp
ValueTask HandleAsync(RoomSpot spot, RoomEvent message, CancellationToken cancellationToken);
ValueTask<RoomReply> HandleAsync(RoomSpot spot, RoomRequest request, CancellationToken cancellationToken);
```

변경 후

```csharp
ValueTask HandleAsync(RoomSpot spot, IZLinkMessageContext context, RoomEvent message, CancellationToken cancellationToken);
ValueTask<RoomReply> HandleAsync(RoomSpot spot, IZLinkMessageContext context, RoomRequest request, CancellationToken cancellationToken);
```

## 동작 변경

- 만료된 owner의 release wait를 authority record마다 따로 재지 않고 owner token마다 한 번 측정합니다. (#5302, #5349)
- 단일 Actor relocation의 진행 상태를 authority record의 `aggregate.relocation`에 기록합니다. (#4718, #5383)

## 결함 수정

- Spot이 Close commit 뒤 요청을 거절하면 message flow에 Close 진입 경로와 이후 Close 실패를 기록합니다. (#5331, #5396)
- 같은 RID와 endpoint를 쓰는 peer가 재시작하면 RouteMesh가 admission 기대값을 갱신합니다. (#5320, #5347)
- STREAM bind에서 Actor ID는 같지만 ObjectGeneration이 다르면 InvalidOperation을 반환합니다. route가 없거나 오래된 경우에는 Unavailable을 반환합니다. (#4774, #5003)
- relocation target은 target을 준비하고 소유권을 확정하기 전에 PREPARE coordinator fence를 현재 authority와 비교합니다. (#4762, #4779)
- Actor와 User Spot 생성 기한이 만료되면 DeadlineExceeded를 반환하며 호출자 취소와 구분합니다. (#5194, #5210)
- STREAM close는 같은 binding에 먼저 제출된 reply와 push가 끝난 뒤 수행합니다. (#4889, #5295)
- 요청 재제출 시 backpressure를 결과값으로 처리하고 timer wake가 semaphore를 넘치지 않도록 수정했습니다. (#5211, #5220)

## 성능 개선

- reply frame을 직접 작성하고 고정 header bytes, deadline 문자열과 heartbeat frame을 재사용해 중간 할당을 줄였습니다. (#4872, #5305)
- chunk가 하나인 relocation stream을 임시 파일 없이 메모리에서 처리합니다. (#5290, #5335)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.34.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.34.0" />
```

릴리스 태그는 [`framework-dotnet/v0.34.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.34.0)입니다.
