using ObservabilityOps.Server.Play.Spots;
using ObservabilityOps.Server.Play.Support;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace ObservabilityOps.Server.Play.Handlers;

[ZLinkSpotTimerHandler("observability-tick", 200)]
internal sealed class RoomTimerHandler(EvidenceStore evidence) : IZLinkSpotTimerHandler<RoomSpot>
{
    public async ValueTask HandleAsync(RoomSpot spot, ZLinkTimerTick tick, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"timer-tick|room={spot.Context.SpotId}|sequence={tick.DeliveryIndex}");
        if (spot.AutoCloseAfter is { } closeAfter && DateTimeOffset.UtcNow >= closeAfter)
        {
            spot.AutoCloseAfter = null;
            _ = await spot.Context.CloseAsync(cancellationToken); // The room ends itself after its application lifetime.
        }
    }
}
