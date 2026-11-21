[English](./framework-dotnet-0.34.0.md) | [한국어](./framework-dotnet-0.34.0.ko.md)

# ZLink .NET Framework 0.34.0 마이그레이션 노트

`IZLinkSpotPacketHandler`와 `IZLinkSpotRequestHandler`를 구현할 때 Spot 인자 바로 뒤에
`IZLinkMessageContext context`를 추가합니다. 송신자가 설정한 읽기 전용 metadata snapshot은
`context.Metadata`에서 읽습니다.

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
