[English](./framework-dotnet-0.34.0.md) | [한국어](./framework-dotnet-0.34.0.ko.md)

# ZLink .NET Framework 0.34.0 Migration Notes

Add `IZLinkMessageContext context` immediately after the Spot parameter in each
`IZLinkSpotPacketHandler` and `IZLinkSpotRequestHandler` implementation. Read the
sender's immutable metadata snapshot through `context.Metadata`.

Before

```csharp
ValueTask HandleAsync(RoomSpot spot, RoomEvent message, CancellationToken cancellationToken);
ValueTask<RoomReply> HandleAsync(RoomSpot spot, RoomRequest request, CancellationToken cancellationToken);
```

After

```csharp
ValueTask HandleAsync(RoomSpot spot, IZLinkMessageContext context, RoomEvent message, CancellationToken cancellationToken);
ValueTask<RoomReply> HandleAsync(RoomSpot spot, IZLinkMessageContext context, RoomRequest request, CancellationToken cancellationToken);
```
