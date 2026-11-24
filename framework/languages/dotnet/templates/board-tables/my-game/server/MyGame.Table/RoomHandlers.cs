using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

[ZLinkSpotRequestHandler(nameof(PrepareRoom))]
public sealed class PrepareRoomHandler
    : IZLinkSpotRequestHandler<TableSpot, PrepareRoom, RoomPrepared>
{
    public ValueTask<RoomPrepared> HandleAsync(
        TableSpot spot,
        IZLinkMessageContext context,
        PrepareRoom request,
        CancellationToken ct
    ) => ValueTask.FromResult(spot.Prepared(request));
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
