using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Channels;
using ZoneWorld.Server.Configuration;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;
using ZoneWorld.Shared.Contracts;
using ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Actors;
using ZoneWorld.Server.ZoneNode.Ports;

namespace ZoneWorld.Server.ZoneNode.Infrastructure.ZLink.Spots.Handlers;

/// <summary>The 100ms world tick (§2.5).</summary>
internal sealed class ZoneTickHandler(
    ZoneNodeSettings settings,
    IOpsReportPort ops) : IZLinkSpotTimerHandler<ZoneSpot>
{
    private static int _faultsInjected;

    public async ValueTask HandleAsync(
        ZoneSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        try
        {
            // Fault injection for ZW-C4. The scenario has to see a real spot runtime event —
            // a timer handler that throws — and the only way to get one is to make a timer
            // handler throw. It fires once so the world keeps running afterwards.
            var faultZone = settings.FaultTickZone;
            if (faultZone == spot.ZoneId && Interlocked.Exchange(ref _faultsInjected, 1) == 0)
                throw new InvalidOperationException(
                    $"injected tick failure for ZW-C4. zone={spot.ZoneId}");

            await spot.TickAsync(cancellationToken);
        }
        catch (Exception error) when (error is not OperationCanceledException)
        {
            try
            {
                await ops.ReportSpotEventAsync(
                    NodeAlertKinds.TimerHandlerFailed,
                    $"spot={spot.ZoneId}; timer=zone-tick-{spot.ZoneId}; detail={error.Message}",
                    cancellationToken);
            }
            catch (Exception reportError)
            {
                // The Framework still owns timer failure logging. A report transport failure
                // must not replace the original handler exception or change timer policy.
                Console.Error.WriteLine(
                    $"zone spot event report failed. zone={spot.ZoneId} error={reportError.Message}");
            }

            throw;
        }
    }
}

/// <summary>The 500ms bot tick (§2.7). Separate from the world tick because bots move on
/// their own cadence.</summary>
internal sealed class BotTickHandler : IZLinkSpotTimerHandler<ZoneSpot>
{
    public ValueTask HandleAsync(ZoneSpot spot, ZLinkTimerTick tick, CancellationToken cancellationToken) =>
        spot.BotTickAsync(cancellationToken);
}

/// <summary>
/// The announcement arriving from this node's fanout subscriber, through the spot bridge
/// (§8.2). The subscriber sends only to the zones its own node hosts: a spot publish
/// would reach the whole mesh and every zone spot would receive the announcement once
/// per node.
/// </summary>
[ZLinkSpotPacketHandler(nameof(DeliverAnnounceMsg))]
internal sealed class DeliverAnnounceHandler : IZLinkSpotPacketHandler<ZoneSpot, DeliverAnnounceMsg>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        DeliverAnnounceMsg message,
        CancellationToken cancellationToken) =>
        spot.DeliverAnnounceAsync(message, cancellationToken);
}

/// <summary>
/// Applies the actor's same-zone coordinate update to the Spot projection. The actor
/// remains the authority; this handler only updates the copy used for rendering and
/// border snapshots.
/// </summary>
[ZLinkSpotPacketHandler(nameof(UpdatePositionMsg))]
internal sealed class UpdatePositionHandler : IZLinkSpotPacketHandler<ZoneSpot, UpdatePositionMsg>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        UpdatePositionMsg message,
        CancellationToken cancellationToken)
    {
        spot.ApplyPositionUpdate(message);
        return ValueTask.CompletedTask;
    }
}

/// <summary>
/// A border snapshot from an adjacent zone (§4.1). Each ZoneSpot registers this handler only
/// for the two topics whose destination is that spot, so another local zone cannot consume
/// and discard its snapshot.
/// </summary>
internal sealed class ZoneBorderSubscriptionHandler : IZLinkSpotSubscriptionHandler<ZoneSpot, ZoneBorderEvent>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        ZoneBorderEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        if (string.Equals(message.ToZoneId, spot.ZoneId, StringComparison.Ordinal))
            spot.ApplyBorderSnapshot(message);
        return ValueTask.CompletedTask;
    }
}

/// <summary>
/// Handles a reconnect after the player has already joined a zone. Rebinding the
/// session must not reset the actor's authoritative coordinate or move it back to
/// the spawn zone.
/// </summary>
[ZLinkSpotActorRequestHandler(nameof(JoinWorldReq))]
internal sealed class RejoinWorldHandler :
    IZLinkSpotActorRequestHandler<ZoneSpot, PlayerActor, JoinWorldReq, JoinWorldRes>
{
    public ValueTask<JoinWorldRes> HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinWorldReq message,
        CancellationToken cancellationToken) =>
        ValueTask.FromResult(spot.Rejoin(actor));
}
