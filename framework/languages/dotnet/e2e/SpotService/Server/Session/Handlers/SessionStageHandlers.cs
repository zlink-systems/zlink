using SpotService.Server.Session.Spots;
using SpotService.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace SpotService.Server.Session.Handlers;

[ZLinkSpotSubscriptionHandler(SpotServiceNames.SpotChannel, SpotServiceNames.SpotMsgTopic)]
internal sealed class SpotMsgHandler(EvidenceStore evidence)
    : IZLinkSpotSubscriptionHandler<ScenarioUserSpot, SpotMsg>
{
    public ValueTask HandleAsync(
        ScenarioUserSpot spot,
        SpotMsg message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-msg|rid={evidence.Rid}|spot={spot.Context.SpotId}|marker={message.Marker}");
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotRequestHandler("StageProbeReq")]
internal sealed class StageProbeHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioUserSpot, StageProbeReq, StateRes>
{
    public ValueTask<StateRes> HandleAsync(
        ScenarioUserSpot spot,
        StageProbeReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var stage = new ScenarioStage(spot);
        return ValueTask.FromResult(stage.Apply(request, evidence));
    }
}

[ZLinkSpotPacketHandler("StageTimerStartMsg")]
internal sealed class StageTimerStartHandler
    : IZLinkSpotPacketHandler<ScenarioUserSpot, StageTimerStartMsg>
{
    public async ValueTask HandleAsync(
        ScenarioUserSpot spot,
        StageTimerStartMsg request,
        CancellationToken cancellationToken)
    {
        var stage = new ScenarioStage(spot);
        await stage.StartTimerAsync(request, cancellationToken);
    }
}

internal sealed class StageTimerHandler(EvidenceStore evidence)
    : IZLinkSpotTimerHandler<ScenarioUserSpot>
{
    public ValueTask HandleAsync(
        ScenarioUserSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"stage-timer|rid={evidence.Rid}|spot={spot.Context.SpotId}|name={tick.Name}"
            + $"|delivery={tick.DeliveryIndex}");
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotRequestHandler("StateReq")]
internal sealed class StateReqHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioUserSpot, StateReq, StateRes>
{
    public ValueTask<StateRes> HandleAsync(
        ScenarioUserSpot spot,
        StateReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var delta = string.Equals(request.Operation, "add", StringComparison.Ordinal) ? request.Delta : 0;
        var value = spot.Add(delta);
        evidence.Add($"spot-state-request|rid={evidence.Rid}|spot={spot.Context.SpotId}|value={value}");
        return ValueTask.FromResult(new StateRes(
            spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(),
            value));
    }
}

[ZLinkSpotRequestHandler("StateReq")]
internal sealed class MultiNodeStateAHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<MultiNodeSpotA, StateReq, StateRes>
{
    public ValueTask<StateRes> HandleAsync(
        MultiNodeSpotA spot,
        StateReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var delta = string.Equals(request.Operation, "add", StringComparison.Ordinal) ? request.Delta : 0;
        var value = spot.Add(delta);
        evidence.Add(
            $"multi-state-request|node={SpotServiceNames.MultiSpotNodeA}|spot={spot.Context.SpotId}|value={value}");
        return ValueTask.FromResult(new StateRes(
            spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(),
            value));
    }
}

[ZLinkSpotRequestHandler("StateReq")]
internal sealed class MultiNodeStateBHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<MultiNodeSpotB, StateReq, StateRes>
{
    public ValueTask<StateRes> HandleAsync(
        MultiNodeSpotB spot,
        StateReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var delta = string.Equals(request.Operation, "add", StringComparison.Ordinal) ? request.Delta : 0;
        var value = spot.Add(delta);
        evidence.Add(
            $"multi-state-request|node={SpotServiceNames.MultiSpotNodeB}|spot={spot.Context.SpotId}|value={value}");
        return ValueTask.FromResult(new StateRes(
            spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(),
            value));
    }
}

[ZLinkSpotPacketHandler("StateMsg")]
internal sealed class StateCommandHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<ScenarioUserSpot, StateMsg>
{
    public ValueTask HandleAsync(
        ScenarioUserSpot spot,
        StateMsg message,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"spot-state-command|rid={evidence.Rid}|spot={spot.Context.SpotId}|marker={message.Marker}");
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotRequestHandler("SlowSpotReq")]
internal sealed class SlowSpotHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioUserSpot, SlowSpotReq, SlowSpotRes>
{
    public async ValueTask<SlowSpotRes> HandleAsync(
        ScenarioUserSpot spot,
        SlowSpotReq request,
        CancellationToken cancellationToken)
    {
        await Task.Delay(TimeSpan.FromMilliseconds(request.DelayMs), cancellationToken);
        evidence.Add($"slow-spot-request|rid={evidence.Rid}|spot={spot.Context.SpotId}|marker={request.Marker}");
        return new SlowSpotRes(
            spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(),
            request.Marker);
    }
}

[ZLinkSpotRequestHandler("WorkerStartReq")]
internal sealed class WorkerStartHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<ScenarioUserSpot, WorkerStartReq, WorkerStartRes>
{
    public async ValueTask<WorkerStartRes> HandleAsync(
        ScenarioUserSpot spot,
        WorkerStartReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"worker-start|rid={evidence.Rid}|spot={spot.Context.SpotId}|marker={request.Marker}");
        var marker = await spot.Context.RunCpuWorker(ct =>
            {
                ct.ThrowIfCancellationRequested();
                Thread.Sleep(TimeSpan.FromMilliseconds(request.DelayMs));
                return request.Marker;
            })
            .Yield(cancellationToken);
        spot.Add(100);
        evidence.Add($"worker-complete|rid={evidence.Rid}|spot={spot.Context.SpotId}|marker={marker}");
        return new WorkerStartRes(
            spot.Context.SpotId.ToString(),
            spot.Context.NodeRid.ToString(),
            request.Marker);
    }
}

[ZLinkSpotPacketHandler("OverrunStartMsg")]
internal sealed class OverrunStartHandler
    : IZLinkSpotPacketHandler<ScenarioUserSpot, OverrunStartMsg>
{
    public async ValueTask HandleAsync(
        ScenarioUserSpot spot,
        OverrunStartMsg request,
        CancellationToken cancellationToken)
    {
        var policy = Enum.Parse<ZLinkTimerOverrunPolicy>(request.Policy, false);
        await spot.Context.AddTimer<OverrunTimerHandler>(
            request.Name,
            TimeSpan.FromMilliseconds(request.PeriodMs),
            new ZLinkTimerOptions
            {
                OverrunPolicy = policy,
                MaxCatchUpTicks = 2
            },
            cancellationToken);
    }
}

internal sealed class OverrunTimerHandler(EvidenceStore evidence)
    : IZLinkSpotTimerHandler<ScenarioUserSpot>
{
    public async ValueTask HandleAsync(
        ScenarioUserSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        evidence.Add(
            $"timer-overrun|rid={evidence.Rid}|spot={spot.Context.SpotId}|name={tick.Name}"
            + $"|delivery={tick.DeliveryIndex}|scheduled={tick.ScheduledIndex}|skipped={tick.SkippedTicks}");
        await Task.Delay(TimeSpan.FromMilliseconds(90), cancellationToken);
    }
}

[ZLinkSpotPacketHandler("TimerStartMsg")]
internal sealed class TimerStartHandler
    : IZLinkSpotPacketHandler<ScenarioUserSpot, TimerStartMsg>
{
    public async ValueTask HandleAsync(
        ScenarioUserSpot spot,
        TimerStartMsg request,
        CancellationToken cancellationToken)
    {
        await spot.Context.AddTimer<BasicTimerHandler>(
            request.Name,
            TimeSpan.FromMilliseconds(request.PeriodMs),
            cancellationToken: cancellationToken);
    }
}

internal sealed class BasicTimerHandler(EvidenceStore evidence)
    : IZLinkSpotTimerHandler<ScenarioUserSpot>
{
    public ValueTask HandleAsync(
        ScenarioUserSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"timer-basic|rid={evidence.Rid}|spot={spot.Context.SpotId}|name={tick.Name}"
            + $"|delivery={tick.DeliveryIndex}");
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotPacketHandler("IdleCloseMsg")]
internal sealed class IdleCloseHandler
    : IZLinkSpotPacketHandler<ScenarioUserSpot, IdleCloseMsg>
{
    public async ValueTask HandleAsync(
        ScenarioUserSpot spot,
        IdleCloseMsg request,
        CancellationToken cancellationToken)
    {
        await spot.Context.AddTimer<IdleCloseTimerHandler>(
            request.Name,
            TimeSpan.FromMilliseconds(request.PeriodMs),
            cancellationToken: cancellationToken);
    }
}

internal sealed class IdleCloseTimerHandler(EvidenceStore evidence)
    : IZLinkSpotTimerHandler<ScenarioUserSpot>
{
    public async ValueTask HandleAsync(
        ScenarioUserSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        if (tick.DeliveryIndex > 1) return;

        var closed = await spot.Context.CloseAsync(cancellationToken);
        evidence.Add(
            $"timer-idle-close|rid={evidence.Rid}|spot={spot.Context.SpotId}|name={tick.Name}|closed={closed}");
    }
}
