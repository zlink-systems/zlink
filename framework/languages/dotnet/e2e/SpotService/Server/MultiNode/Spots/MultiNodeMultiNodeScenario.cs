using SpotService.Server.MultiNode.Handlers;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.MultiNode.Spots;

internal sealed class MultiNodeCreateSpotAHandler(
    IZLinkSpotManager spots,
    IZLinkSpotClient routes,
    EvidenceStore evidence)
    : IZLinkRouteRequestHandler<MultiNodeCreateSpotReq, MultiNodeCreateSpotRes>
{
    public async ValueTask<MultiNodeCreateSpotRes> HandleAsync(
        MultiNodeCreateSpotReq request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        var result = await spots
            .GetOrCreate(request.SpotRid, SpotServiceNames.MultiSpotTypeA)
            .Async(cancellationToken);
        var state = await MultiNodeScenario.RequestStateAsync(
            routes,
            request.SpotRid,
            request.Delta,
            cancellationToken);
        evidence.Add(
            $"multi-create-spot|node={SpotServiceNames.MultiSpotNodeA}|spot={result.Spot.SpotId}|state={result.State}");
        return new MultiNodeCreateSpotRes(
            result.Spot.SpotId,
            SpotServiceNames.MultiSpotNodeA,
            result.State.ToString(),
            state.Value);
    }
}

internal sealed class MultiNodeCreateSpotBHandler(
    IZLinkSpotManager spots,
    IZLinkSpotClient routes,
    EvidenceStore evidence)
    : IZLinkRouteRequestHandler<MultiNodeCreateSpotReq, MultiNodeCreateSpotRes>
{
    public async ValueTask<MultiNodeCreateSpotRes> HandleAsync(
        MultiNodeCreateSpotReq request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        var result = await spots
            .GetOrCreate(request.SpotRid, SpotServiceNames.MultiSpotTypeB)
            .Async(cancellationToken);
        var state = await MultiNodeScenario.RequestStateAsync(
            routes,
            request.SpotRid,
            request.Delta,
            cancellationToken);
        evidence.Add(
            $"multi-create-spot|node={SpotServiceNames.MultiSpotNodeB}|spot={result.Spot.SpotId}|state={result.State}");
        return new MultiNodeCreateSpotRes(
            result.Spot.SpotId,
            SpotServiceNames.MultiSpotNodeB,
            result.State.ToString(),
            state.Value);
    }
}

internal static class MultiNodeScenario
{
    public static async Task<StateRes> RequestStateAsync(
        IZLinkSpotClient routes,
        string spotRid,
        int delta,
        CancellationToken cancellationToken)
        => await routes.RequestToSpot(spotRid,
                new StateReq("add", delta))
            .Timeout(TimeSpan.FromSeconds(2))
            .Async<StateRes>(cancellationToken);
}

internal sealed class MultiNodeSpotA(IZLinkSpotContext context, EvidenceStore evidence) : IZLinkSpot
{
    private int _value;

    public IZLinkSpotContext Context { get; } = context;

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"multi-spot-initialize|node={SpotServiceNames.MultiSpotNodeA}|spot={Context.SpotId}");
        return ValueTask.CompletedTask;
    }

    public int Add(int delta)
    {
        _value += delta;
        return _value;
    }
}

internal sealed class MultiNodeSpotB(IZLinkSpotContext context, EvidenceStore evidence) : IZLinkSpot
{
    private int _value;

    public IZLinkSpotContext Context { get; } = context;

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"multi-spot-initialize|node={SpotServiceNames.MultiSpotNodeB}|spot={Context.SpotId}");
        return ValueTask.CompletedTask;
    }

    public int Add(int delta)
    {
        _value += delta;
        return _value;
    }
}

internal sealed class ScenarioStage(ScenarioUserSpot spot)
{
    public StateRes Apply(StageProbeReq request, EvidenceStore evidence)
    {
        var value = spot.Add(request.Delta);
        evidence.Add(
            $"stage-request|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|marker={request.Marker}|value={value}");
        return new StateRes(
            spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(),
            value);
    }

    public async ValueTask StartTimerAsync(
        StageTimerStartMsg command,
        CancellationToken cancellationToken)
    {
        await spot.Context.AddTimer<StageTimerHandler>(
            command.Name,
            TimeSpan.FromMilliseconds(command.PeriodMs),
            cancellationToken: cancellationToken);
    }
}
