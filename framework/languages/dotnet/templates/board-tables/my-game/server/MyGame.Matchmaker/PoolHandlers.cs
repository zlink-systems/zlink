using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace MyGame.Matchmaker;

[ZLinkSpotRequestHandler(nameof(MatchRequest))]
public sealed class MatchRequestHandler
    : IZLinkSpotRequestHandler<PoolSpot, MatchRequest, MatchQueued>
{
    public ValueTask<MatchQueued> HandleAsync(
        PoolSpot spot,
        IZLinkMessageContext context,
        MatchRequest request,
        CancellationToken ct
    ) => spot.EnqueueAsync(request);
}

[ZLinkSpotRequestHandler(nameof(CancelTicket))]
public sealed class CancelTicketHandler
    : IZLinkSpotRequestHandler<PoolSpot, CancelTicket, MatchCancelled>
{
    public ValueTask<MatchCancelled> HandleAsync(
        PoolSpot spot,
        IZLinkMessageContext context,
        CancelTicket request,
        CancellationToken ct
    ) => spot.CancelAsync(request);
}

[ZLinkSpotPacketHandler(nameof(EnterReport))]
public sealed class EnterReportHandler : IZLinkSpotPacketHandler<PoolSpot, EnterReport>
{
    public ValueTask HandleAsync(
        PoolSpot spot,
        IZLinkMessageContext context,
        EnterReport report,
        CancellationToken ct
    ) => spot.ReportAsync(report);
}

public sealed class PoolTick : IZLinkSpotTimerHandler<PoolSpot>
{
    public ValueTask HandleAsync(PoolSpot spot, ZLinkTimerTick tick, CancellationToken ct) =>
        spot.AdvanceAsync();
}

[ZLinkSpotPacketHandler(nameof(RoomPrepared))]
public sealed class RoomPreparedHandler : IZLinkSpotPacketHandler<PoolSpot, RoomPrepared>
{
    public ValueTask HandleAsync(
        PoolSpot spot,
        IZLinkMessageContext context,
        RoomPrepared message,
        CancellationToken ct
    ) => spot.RoomPreparedAsync(message);
}
