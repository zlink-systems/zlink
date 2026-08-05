using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace AutomaticTurnDispatch.Server.Play.Handlers;

[ZLinkSpotRequestHandler("HoldReq")]
internal sealed class HoldHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotRequestHandler<AwaitProbeSpot, HoldReq, AutomaticTurnDispatchRes>
{
    public async ValueTask<AutomaticTurnDispatchRes> HandleAsync(
        AwaitProbeSpot spot,
        HoldReq request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"hold-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        await routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, "hold"))
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<DelayRes>(cancellationToken);
        evidence.Add(
            $"hold-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        evidence.Add(
            $"hold-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        return AwaitReplies.Reply("probe-A1", request.RequestId, spot, "hold-completed");
    }
}

[ZLinkSpotPacketHandler("HoldMsg")]
internal sealed class HoldCommandHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, HoldMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        HoldMsg request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"hold-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        await routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, "hold"))
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<DelayRes>(cancellationToken);
        evidence.Add(
            $"hold-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        evidence.Add(
            $"hold-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
    }
}

[ZLinkSpotRequestHandler("AwaitReq")]
internal sealed class AwaitHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotRequestHandler<AwaitProbeSpot, AwaitReq, AutomaticTurnDispatchRes>
{
    public async ValueTask<AutomaticTurnDispatchRes> HandleAsync(
        AwaitProbeSpot spot,
        AwaitReq request,
        CancellationToken cancellationToken)
    {
        var prefix = request.Terminator == "yield" ? "yield" : "async";
        evidence.Add(
            $"{prefix}-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|correlation={request.CorrelationId}|handler=spot");
        var call = routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, "await"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"{(prefix == "yield" ? "yield-released" : "await-held")}|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|correlation={request.CorrelationId}|handler=spot");
        await TurnTerminator.Complete<DelayRes>(call, request.Terminator, cancellationToken);
        evidence.Add(
            $"{prefix}-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|correlation={request.CorrelationId}|handler=spot");
        evidence.Add(
            $"{prefix}-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|correlation={request.CorrelationId}|handler=spot");
        return AwaitReplies.Reply(prefix == "yield" ? "TD-B1" : "TD-A2", request.RequestId, spot,
            $"{prefix}-completed");
    }
}

[ZLinkSpotPacketHandler("AwaitMsg")]
internal sealed class AwaitCommandHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, AwaitMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        AwaitMsg request,
        CancellationToken cancellationToken)
    {
        var prefix = request.Terminator == "yield" ? "yield" : "async";
        evidence.Add(
            $"{prefix}-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|correlation={request.CorrelationId}|handler=spot");
        var call = routeClient.RequestToChannel(
                AutomaticTurnDispatchNames.DelayChannel,
                new DelayReq(request.RequestId, request.DelayMs, "await"))
            .Timeout(TimeSpan.FromSeconds(5));
        evidence.Add(
            $"{(prefix == "yield" ? "yield-released" : "await-held")}|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|correlation={request.CorrelationId}|handler=spot");
        await TurnTerminator.Complete<DelayRes>(call, request.Terminator, cancellationToken);
        evidence.Add(
            $"{prefix}-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|correlation={request.CorrelationId}|handler=spot");
        evidence.Add(
            $"{prefix}-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|correlation={request.CorrelationId}|handler=spot");
    }
}

[ZLinkSpotRequestHandler("WorkerAwaitReq")]
internal sealed class WorkerAwaitHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<AwaitProbeSpot, WorkerAwaitReq, AutomaticTurnDispatchRes>
{
    public async ValueTask<AutomaticTurnDispatchRes> HandleAsync(
        AwaitProbeSpot spot,
        WorkerAwaitReq request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"worker-await-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        var call = spot.Context.RunCpuWorker(ct =>
        {
            ct.ThrowIfCancellationRequested();
            Thread.Sleep(TimeSpan.FromMilliseconds(request.DelayMs));
            return request.RequestId;
        });
        evidence.Add(
            $"worker-await-released|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        await call.Async(cancellationToken);
        evidence.Add(
            $"worker-await-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        evidence.Add(
            $"worker-await-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        return AwaitReplies.Reply("probe-A4", request.RequestId, spot, "worker-await-completed");
    }
}

[ZLinkSpotPacketHandler("WorkerAwaitMsg")]
internal sealed class WorkerAwaitCommandHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, WorkerAwaitMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        WorkerAwaitMsg request,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"worker-await-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        var call = spot.Context.RunCpuWorker(ct =>
        {
            ct.ThrowIfCancellationRequested();
            Thread.Sleep(TimeSpan.FromMilliseconds(request.DelayMs));
            return request.RequestId;
        });
        evidence.Add(
            $"worker-await-released|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        await call.Async(cancellationToken);
        evidence.Add(
            $"worker-await-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
        evidence.Add(
            $"worker-await-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}|handler=spot");
    }
}

[ZLinkSpotRequestHandler("ProbeReq")]
internal sealed class ProbeHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<AwaitProbeSpot, ProbeReq, AutomaticTurnDispatchRes>
{
    public ValueTask<AutomaticTurnDispatchRes> HandleAsync(
        AwaitProbeSpot spot,
        ProbeReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"probe-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|marker={request.Marker}|handler=spot");
        evidence.Add(
            $"probe-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|marker={request.Marker}|handler=spot");
        return ValueTask.FromResult(AwaitReplies.Reply("probe-PROBE", request.RequestId, spot, request.Marker));
    }
}

[ZLinkSpotPacketHandler("ProbeMsg")]
internal sealed class ProbeCommandHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, ProbeMsg>
{
    public ValueTask HandleAsync(
        AwaitProbeSpot spot,
        ProbeMsg request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"probe-started|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|marker={request.Marker}|handler=spot");
        evidence.Add(
            $"probe-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}|request={request.RequestId}"
            + $"|marker={request.Marker}|handler=spot");
        return ValueTask.CompletedTask;
    }
}
