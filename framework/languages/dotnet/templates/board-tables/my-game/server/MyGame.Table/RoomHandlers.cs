using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

[ZLinkSpotPacketHandler(nameof(PrepareRoom))]
public sealed class PrepareRoomHandler : IZLinkSpotPacketHandler<RoomProvisioner, PrepareRoom>
{
    public ValueTask HandleAsync(
        RoomProvisioner spot,
        IZLinkMessageContext context,
        PrepareRoom request,
        CancellationToken ct
    ) => spot.PrepareAsync(request, ct);
}

[ZLinkSpotPacketHandler(nameof(WithdrawRoom))]
public sealed class WithdrawRoomRequestHandler
    : IZLinkSpotPacketHandler<RoomProvisioner, WithdrawRoom>
{
    public ValueTask HandleAsync(
        RoomProvisioner spot,
        IZLinkMessageContext context,
        WithdrawRoom request,
        CancellationToken ct
    ) => spot.WithdrawAsync(request, ct);
}

[ZLinkSpotPacketHandler(nameof(CommitRoom))]
public sealed class CommitRoomHandler : IZLinkSpotPacketHandler<TableSpot, CommitRoom>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        IZLinkMessageContext context,
        CommitRoom request,
        CancellationToken ct
    ) => spot.CommitAsync(request.AssignmentId, ct);
}

[ZLinkSpotPacketHandler(nameof(WithdrawRoom))]
public sealed class WithdrawRoomHandler : IZLinkSpotPacketHandler<TableSpot, WithdrawRoom>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        IZLinkMessageContext context,
        WithdrawRoom request,
        CancellationToken ct
    )
    {
        spot.Withdraw(request.AssignmentId);
        return ValueTask.CompletedTask;
    }
}
