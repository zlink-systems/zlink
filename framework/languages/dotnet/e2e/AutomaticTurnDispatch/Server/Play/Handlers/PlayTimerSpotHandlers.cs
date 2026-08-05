using AutomaticTurnDispatch.Server.Play.Spots;
using AutomaticTurnDispatch.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace AutomaticTurnDispatch.Server.Play.Handlers;

[ZLinkSpotRequestHandler("TimerStartReq")]
internal sealed class TimerStartHandler(EvidenceStore evidence)
    : IZLinkSpotRequestHandler<AwaitProbeSpot, TimerStartReq, AutomaticTurnDispatchRes>
{
    public async ValueTask<AutomaticTurnDispatchRes> HandleAsync(
        AwaitProbeSpot spot,
        TimerStartReq request,
        CancellationToken cancellationToken)
    {
        var state = new AwaitTimerState(
            request.RequestId,
            request.TimerName,
            request.Mode,
            request.DelayMs);
        if (!spot.TryAddTimerState(state))
        {
            evidence.Add(
                $"timer-start-duplicate-ignored|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={request.RequestId}|timer={request.TimerName}|mode={request.Mode}");
            return AwaitReplies.Reply("probe-C", request.RequestId, spot, "timer-started");
        }

        state.Timer = await spot.Context.AddTimer<AwaitTimerHandler>(
            request.TimerName,
            TimeSpan.FromMilliseconds(request.PeriodMs),
            new ZLinkTimerOptions { OverrunPolicy = ZLinkTimerOverrunPolicy.DelayNextTick },
            cancellationToken);
        evidence.Add(
            $"timer-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|timer={request.TimerName}|mode={request.Mode}");
        return AwaitReplies.Reply("probe-C", request.RequestId, spot, "timer-started");
    }
}

[ZLinkSpotPacketHandler("TimerStartMsg")]
internal sealed class TimerStartCommandHandler(EvidenceStore evidence)
    : IZLinkSpotPacketHandler<AwaitProbeSpot, TimerStartMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        TimerStartMsg request,
        CancellationToken cancellationToken)
    {
        var state = new AwaitTimerState(
            request.RequestId,
            request.TimerName,
            request.Mode,
            request.DelayMs);
        if (!spot.TryAddTimerState(state))
        {
            evidence.Add(
                $"timer-start-duplicate-ignored|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={request.RequestId}|timer={request.TimerName}|mode={request.Mode}");
            return;
        }

        state.Timer = await spot.Context.AddTimer<AwaitTimerHandler>(
            request.TimerName,
            TimeSpan.FromMilliseconds(request.PeriodMs),
            new ZLinkTimerOptions { OverrunPolicy = ZLinkTimerOverrunPolicy.DelayNextTick },
            cancellationToken);
        evidence.Add(
            $"timer-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
            + $"|request={request.RequestId}|timer={request.TimerName}|mode={request.Mode}");
    }
}

[ZLinkSpotRequestHandler("TimerStopReq")]
internal sealed class TimerStopHandler
    : IZLinkSpotRequestHandler<AwaitProbeSpot, TimerStopReq, AutomaticTurnDispatchRes>
{
    public async ValueTask<AutomaticTurnDispatchRes> HandleAsync(
        AwaitProbeSpot spot,
        TimerStopReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        await spot.StopScenarioTimersAsync(request.RequestId);
        return AwaitReplies.Reply("probe-C", request.RequestId, spot, "timer-stopped");
    }
}

[ZLinkSpotPacketHandler("TimerStopMsg")]
internal sealed class TimerStopCommandHandler
    : IZLinkSpotPacketHandler<AwaitProbeSpot, TimerStopMsg>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        TimerStopMsg request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        await spot.StopScenarioTimersAsync(request.RequestId);
    }
}

internal sealed class AwaitTimerHandler(
    EvidenceStore evidence,
    IZLinkRouteClient routeClient)
    : IZLinkSpotTimerHandler<AwaitProbeSpot>
{
    public async ValueTask HandleAsync(
        AwaitProbeSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        var state = spot.FindTimerState(tick.Name);
        if (state is null) return;

        var tickNumber = state.NextTick();
        if (string.Equals(state.Mode, "fast", StringComparison.Ordinal))
        {
            evidence.Add(
                $"timer-fast-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={state.RequestId}|timer={state.TimerName}|tick={tickNumber}|handler=timer");
            evidence.Add(
                $"timer-fast-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={state.RequestId}|timer={state.TimerName}|tick={tickNumber}|handler=timer");
            return;
        }

        if (tickNumber == 1
            && (string.Equals(state.Mode, "async-on-first", StringComparison.Ordinal)
                || string.Equals(state.Mode, "yield-on-first", StringComparison.Ordinal)))
        {
            var terminator = state.Mode.StartsWith("yield", StringComparison.Ordinal) ? "yield" : "async";
            evidence.Add(
                $"timer-{terminator}-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={state.RequestId}|timer={state.TimerName}|tick={tickNumber}|handler=timer");
            var call = routeClient.RequestToChannel(
                    AutomaticTurnDispatchNames.DelayChannel,
                    new DelayReq(state.RequestId, state.DelayMs, state.TimerName))
                .Timeout(TimeSpan.FromSeconds(5));
            evidence.Add(
                $"timer-{terminator}-{(terminator == "yield" ? "released" : "held")}|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={state.RequestId}|timer={state.TimerName}|tick={tickNumber}|handler=timer");
            await TurnTerminator.Complete<DelayRes>(call, terminator, cancellationToken);
            evidence.Add(
                $"timer-{terminator}-resumed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={state.RequestId}|timer={state.TimerName}|tick={tickNumber}|handler=timer");
            evidence.Add(
                $"timer-{terminator}-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={state.RequestId}|timer={state.TimerName}|tick={tickNumber}|handler=timer");
            return;
        }

        if ((string.Equals(state.Mode, "async-on-first", StringComparison.Ordinal)
             || string.Equals(state.Mode, "yield-on-first", StringComparison.Ordinal))
            && tickNumber == 2)
        {
            evidence.Add(
                $"timer-next-started|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={state.RequestId}|timer={state.TimerName}|tick={tickNumber}|handler=timer");
            evidence.Add(
                $"timer-next-completed|rid={evidence.Rid}|spot={spot.Context.SpotId}"
                + $"|request={state.RequestId}|timer={state.TimerName}|tick={tickNumber}|handler=timer");
        }
    }
}
