using SpotService.Server.Play.Spots;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.Play.Handlers;

[ZLinkSpotRequestHandler("SpotToSpotReq")]
internal sealed class SpotToSpotHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioUserSpot, SpotToSpotReq, SpotToSpotRes>
{
    public async ValueTask<SpotToSpotRes> HandleAsync(
        ScenarioUserSpot spot,
        SpotToSpotReq request,
        CancellationToken cancellationToken)
    {
        var reply = await spot.Context.Outbound
            .RequestToSpot(request.TargetSpotRid, new StateReq("add", 3))
            .Async<StateRes>(cancellationToken);
        await spot.Context.Outbound.SendToSpot(
                request.TargetSpotRid,
                new StateMsg($"sm-c3-send-{request.Marker}"))
            .Async(cancellationToken);
        await spot.Context.Outbound.Publish(
                SpotServiceNames.SpotChannel,
                SpotServiceNames.SpotMsgTopic,
                new SpotMsg($"sm-c3-publish-{request.Marker}"))
            .Async(cancellationToken);
        evidence.Add(
            $"spot-to-spot|rid={evidence.Rid}|source={spot.Context.SpotId}"
            + $"|target={request.TargetSpotRid}|value={reply.Value}");
        return new SpotToSpotRes(
            spot.Context.SpotId.ToString(),
            request.TargetSpotRid,
            reply.Value);
    }
}

[ZLinkSpotRequestHandler("SpotToSpotTimeoutReq")]
internal sealed class SpotToSpotTimeoutHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioUserSpot, SpotToSpotTimeoutReq, SpotToSpotTimeoutRes>
{
    public async ValueTask<SpotToSpotTimeoutRes> HandleAsync(
        ScenarioUserSpot spot,
        SpotToSpotTimeoutReq request,
        CancellationToken cancellationToken)
    {
        var failed = false;
        try
        {
            await spot.Context.Outbound
                .RequestToSpot(request.TargetSpotRid, new SlowSpotReq(request.Marker, 1500))
                .Timeout(TimeSpan.FromMilliseconds(100))
                .Async<SlowSpotRes>(cancellationToken);
        }
        catch
        {
            failed = true;
        }

        evidence.Add(
            $"spot-to-spot-timeout|rid={evidence.Rid}|source={spot.Context.SpotId}"
            + $"|target={request.TargetSpotRid}|failed={failed}");
        return new SpotToSpotTimeoutRes(
            spot.Context.SpotId.ToString(),
            request.TargetSpotRid,
            failed);
    }
}

[ZLinkSpotRequestHandler("SpotToSpotNegativeReq")]
internal sealed class SpotToSpotNegativeHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioUserSpot, SpotToSpotNegativeReq, SpotToSpotNegativeRes>
{
    public async ValueTask<SpotToSpotNegativeRes> HandleAsync(
        ScenarioUserSpot spot,
        SpotToSpotNegativeReq request,
        CancellationToken cancellationToken)
    {
        // The target Spot exists, but the requested handler does not. Direct
        // routing resolves the global SpotId before target dispatch.
        var requestFailed = false;
        try
        {
            await spot.Context.Outbound
                .RequestToSpot(request.TargetSpotRid, new MissingSpotReq("noop"))
                .Timeout(TimeSpan.FromSeconds(2))
                .Async<StateRes>(cancellationToken);
        }
        catch
        {
            requestFailed = true;
        }

        await spot.Context.Outbound.SendToSpot(
                request.TargetSpotRid,
                new MissingSpotMsg($"missing-{request.Marker}"))
            .Async(cancellationToken);

        evidence.Add(
            $"spot-to-spot-negative|rid={evidence.Rid}|source={spot.Context.SpotId}"
            + $"|target={request.TargetSpotRid}|requestFailed={requestFailed}");
        return new SpotToSpotNegativeRes(
            spot.Context.SpotId.ToString(),
            request.TargetSpotRid,
            requestFailed);
    }
}

[ZLinkSpotPacketHandler("SpotOutboundMsg")]
internal sealed class SpotOutboundHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<ScenarioUserSpot, SpotOutboundMsg>
{
    public async ValueTask HandleAsync(
        ScenarioUserSpot spot,
        SpotOutboundMsg request,
        CancellationToken cancellationToken)
    {
        var echo = await spot.Context.Outbound
            .RequestToChannel(
                SpotServiceNames.ExternalClientChannel,
                new ChannelEchoReq(request.Marker))
            .Async<ChannelEchoRes>(cancellationToken);
        var notifyMarker = $"notify-{request.Marker}";
        await spot.Context.Outbound.SendToChannel(
                SpotServiceNames.ExternalClientChannel,
                new ChannelNotify(notifyMarker))
            .Async(cancellationToken);
        await spot.Context.Outbound.Publish(
                SpotServiceNames.SpotChannel,
                SpotServiceNames.SpotMsgTopic,
                new SpotMsg("sm-c2-publish"))
            .Async(cancellationToken);
        evidence.Add(
            $"spot-outbound|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|echo={echo.Value}|notify={notifyMarker}");
    }
}

[ZLinkSpotPacketHandler("SpotOutboundNegativeMsg")]
internal sealed class SpotOutboundNegativeHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<ScenarioUserSpot, SpotOutboundNegativeMsg>
{
    public async ValueTask HandleAsync(
        ScenarioUserSpot spot,
        SpotOutboundNegativeMsg request,
        CancellationToken cancellationToken)
    {
        var requestFailed = false;
        try
        {
            await spot.Context.Outbound
                .RequestToChannel(
                    SpotServiceNames.ExternalClientChannel,
                    new MissingChannelReq(request.Marker))
                .Timeout(TimeSpan.FromSeconds(2))
                .Async<ChannelEchoRes>(cancellationToken);
        }
        catch
        {
            requestFailed = true;
        }

        await spot.Context.Outbound.SendToChannel(
                SpotServiceNames.ExternalClientChannel,
                new MissingChannelNotify($"missing-{request.Marker}"))
            .Async(cancellationToken);
        evidence.Add(
            $"spot-outbound-negative|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|requestFailed={requestFailed}");
    }
}
