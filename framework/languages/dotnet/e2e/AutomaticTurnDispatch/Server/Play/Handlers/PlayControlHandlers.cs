using Systems.Zlink;
using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Spots;

namespace AutomaticTurnDispatch.Server.Play.Handlers;

internal sealed class BindAwaitActorsControlHandler(
    IZLinkActorManager actors,
    IZLinkSpotManager spots,
    EvidenceStore evidence,
    NodeOptions node)
    : IZLinkRouteRequestHandler<BindAwaitActorsReq, BindAwaitActorsRes>
{
    public async ValueTask<BindAwaitActorsRes> HandleAsync(
        BindAwaitActorsReq request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        await spots.GetOrCreate(request.SpotRid, request.SpotType)
            .Async(cancellationToken);
        var bindings = new List<AwaitActorBinding>();
        foreach (var actorId in request.ActorIds)
        {
            var actor = (await actors.GetOrCreate(actorId, AutomaticTurnDispatchNames.ActorType)
                .Async(cancellationToken)) switch
            {
                ZLinkActorCreateResult.Existing value => value.Actor,
                ZLinkActorCreateResult.Created value => value.Actor,
                _ => throw new InvalidOperationException("Actor creation was rejected.")
            };
            evidence.Add(
                $"bind-actor|rid={node.Rid}|spot={request.SpotRid}|actor={actor.ActorId}"
                + $"|generation={actor.ObjectGeneration}");
            bindings.Add(new AwaitActorBinding(
                actor.ActorId,
                actor.NodeRid.ToString(),
                actor.ObjectGeneration));
        }

        return new BindAwaitActorsRes(request.SpotRid, bindings.ToArray());
    }
}

internal sealed class EnsureSpotControlHandler(IZLinkSpotManager spots)
    : IZLinkRouteRequestHandler<EnsureSpotReq, EnsureSpotRes>
{
    public async ValueTask<EnsureSpotRes> HandleAsync(
        EnsureSpotReq request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        var result = await spots.GetOrCreate(
                request.SpotRid,
                request.SpotType)
            .Async(cancellationToken);
        return new EnsureSpotRes(
            result.Spot.SpotId,
            result.Spot.NodeRid.ToString());
    }
}

internal sealed class AwaitEvidenceControlHandler(EvidenceStore evidence)
    : IZLinkRouteRequestHandler<AwaitEvidenceReq, AwaitEvidenceRes>
{
    public ValueTask<AwaitEvidenceRes> HandleAsync(
        AwaitEvidenceReq request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new AwaitEvidenceRes(request.RequestId, evidence.Snapshot(request.RequestId)));
    }
}

internal sealed class AwaitEvidenceWaitControlHandler(EvidenceStore evidence)
    : IZLinkRouteRequestHandler<AwaitEvidenceWaitReq, AwaitEvidenceRes>
{
    public async ValueTask<AwaitEvidenceRes> HandleAsync(
        AwaitEvidenceWaitReq request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
        var snapshot = await evidence.WaitUntilAsync(
            entries => entries.Count(line =>
                line.Contains($"request={request.RequestId}", StringComparison.Ordinal)
                && line.Contains(request.Marker, StringComparison.Ordinal)) >= Math.Max(1, request.MinimumCount),
            timeout,
            cancellationToken);
        return new AwaitEvidenceRes(
            request.RequestId,
            snapshot
                .Where(entry => entry.Contains(
                    $"request={request.RequestId}",
                    StringComparison.Ordinal))
                .ToArray());
    }
}
