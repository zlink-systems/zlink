using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Contracts.Timers;
using Zlink.NetworkSync.Simulation;

namespace Zlink.NetworkSync.Server;

/// <summary>
/// Hosts one match in a user Spot. Register the concrete Spot with SpotWide execution and
/// relocation disabled. Relay client packets with <see cref="NetworkSyncSession.TryRelayAsync"/>.
/// </summary>
public abstract class NetworkSyncSpot<TSpot, TActor, TInput, TAction, TState> : IZLinkSpot<TActor>
    where TSpot : NetworkSyncSpot<TSpot, TActor, TInput, TAction, TState>
    where TActor : class, IZLinkActor
{
    private readonly MatchClock _clock;
    private readonly ILogger _logger;
    private readonly SimulationSettings _settings;
    private readonly Func<TActor, IZLinkBoundSession> _boundSession;
    private readonly Dictionary<string, SnapshotRecipient<TState>> _recipients = new(StringComparer.Ordinal);
    private NetworkSyncMatch<TInput, TAction> _match = null!;

    protected NetworkSyncSpot(IZLinkSpotContext context, ILogger logger,
        int? tickRate = null, int? snapshotRate = null, TimeSpan? wakeupBudget = null,
        TimeProvider? clock = null)
        : this(context, logger, static actor => actor.Context.BoundSession, tickRate, snapshotRate, wakeupBudget, clock)
    {
    }

    internal NetworkSyncSpot(IZLinkSpotContext context, ILogger logger,
        Func<TActor, IZLinkBoundSession> boundSession, int? tickRate = null,
        int? snapshotRate = null, TimeSpan? wakeupBudget = null, TimeProvider? clock = null)
    {
        Context = context;
        _logger = logger;
        _boundSession = boundSession;
        _clock = new(clock ?? TimeProvider.System);
        var defaults = new SimulationSettings();
        _settings = defaults with
        {
            TickRate = tickRate ?? defaults.TickRate,
            SnapshotRate = snapshotRate ?? defaults.SnapshotRate,
            WakeupBudget = wakeupBudget,
        };
        ArgumentOutOfRangeException.ThrowIfGreaterThan(_settings.TickRate, 1_000_000, nameof(tickRate));
    }

    public IZLinkSpotContext Context { get; }

    protected virtual void ConfigureMatch() { }

    protected virtual ValueTask OnMatchInitializeAsync(CancellationToken ct) => ValueTask.CompletedTask;

    protected abstract ValueTask<ZLinkSpotActorJoinResult> AdmitPlayerAsync(
        string actorId, ZLinkMessage request, CancellationToken ct);

    protected virtual ValueTask OnPlayerJoinedAsync(TActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    protected virtual ValueTask OnPlayerLeftAsync(TActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    protected virtual ValueTask OnMatchClosingAsync(ZLinkSpotClosingContext context, CancellationToken ct) =>
        ValueTask.CompletedTask;

    protected abstract TInput GetMissingInput(string actorId);

    protected abstract void Step(long epoch, long tick, NetworkSyncInputs<TInput, TAction> inputs);

    /// <summary>Returns a full state value that stays immutable after capture.</summary>
    protected abstract TState CaptureSnapshot();

    void IZLinkSpot.Configure()
    {
        Context.Handlers.AddActorPacket<InputHandler, TActor>(NetworkSyncSession.InputPacket);
        Context.Handlers.AddActorPacket<PingHandler, TActor>(NetworkSyncSession.PingPacket);
        ConfigureMatch();
    }

    async ValueTask IZLinkSpot.OnInitializeAsync(CancellationToken cancellationToken)
    {
        await OnMatchInitializeAsync(cancellationToken);
        _match = new(Context.SpotId, _clock, new Simulation(this), _settings);
        await Context.AddTimer<TickHandler>("network-sync-tick",
            TimeSpan.FromSeconds(1d / _settings.TickRate),
            new ZLinkTimerOptions { StopOnUnhandledException = true }, cancellationToken);
    }

    ValueTask<ZLinkSpotActorJoinResult> IZLinkUserSpotActorLifecycle<TActor>.OnActorJoinAsync(
        string actorId, ZLinkMessage request, CancellationToken cancellationToken) =>
        AdmitPlayerAsync(actorId, request, cancellationToken);

    ValueTask IZLinkSpotActorMembershipLifecycle<TActor>.OnJoinedActorAsync(TActor actor, CancellationToken cancellationToken)
    {
        var actorId = actor.Context.ActorId;
        if (_match.AddPlayer(actorId))
            _recipients.Add(actorId, new(actorId, _boundSession(actor), _logger));

        return OnPlayerJoinedAsync(actor, cancellationToken);
    }

    ValueTask IZLinkSpotActorMembershipLifecycle<TActor>.OnLeaveActorAsync(TActor actor, CancellationToken cancellationToken)
    {
        var actorId = actor.Context.ActorId;
        _match.RemovePlayer(actorId);
        if (_recipients.Remove(actorId, out var recipient))
            recipient.Retire();

        return OnPlayerLeftAsync(actor, cancellationToken);
    }

    ValueTask IZLinkSpot.OnClosingAsync(ZLinkSpotClosingContext context, CancellationToken cleanupCancellationToken)
    {
        foreach (var recipient in _recipients.Values)
            recipient.Retire();
        _recipients.Clear();
        return OnMatchClosingAsync(context, cleanupCancellationToken);
    }

    private ulong TickUs => (1_000_000UL + (ulong)_settings.TickRate - 1) / (ulong)_settings.TickRate;

    private void OnTimer()
    {
        try
        {
            var result = _match.Wakeup();
            if (result.SnapshotDue && _recipients.Count != 0)
            {
                var snapshot = new FullSnapshot<TState>(result.LatestTick, CaptureSnapshot());
                foreach (var recipient in _recipients.Values)
                {
                    if (result.SkippedTicks != 0)
                        recipient.ResetEpoch();
                    recipient.Replace(snapshot);
                }
            }
        }
        catch
        {
            // The Framework completes self-close after the timer turn returns.
            _ = ObserveCloseAsync(Context, _logger);
            throw;
        }

        foreach (var recipient in _recipients.Values)
            recipient.Flush(Context.SpotId, TickUs, _clock);
    }

    private static async Task ObserveCloseAsync(IZLinkSpotContext context, ILogger logger)
    {
        try
        {
            await context.CloseAsync().ConfigureAwait(false);
        }
        catch (Exception error)
        {
            logger.LogError(error, "Failed to close network sync Spot {SpotId} after a simulation error.", context.SpotId);
        }
    }

    private void ReceiveInput(TActor actor, NetworkSyncInputV1<TInput, TAction> message)
    {
        var actorId = actor.Context.ActorId;
        if (!_recipients.TryGetValue(actorId, out var recipient))
            return;

        ReadOnlySpan<SimulationAction<TAction>> actions = message.Actions.Length == 0
            ? []
            : Array.ConvertAll(message.Actions, static action => new SimulationAction<TAction>(action.Id, action.Payload));

        // Session ingress admits the original payload by byte length. Typed Actor
        // dispatch no longer exposes those bytes, so the core sees an admitted size.
        const int admittedSize = 1;
        var result = _match.SubmitInput(message.MatchId, unchecked((long)message.Epoch), actorId,
            unchecked((long)message.TargetTick), admittedSize, message.Input, actions);
        // Use the same timestamp the core used to decide the arrival margin.
        recipient.Feedback = new(message.TargetTick, message.T1Us, _clock.Sample, result);
    }

    private void ReceivePing(TActor actor, NetworkSyncPingV1 message)
    {
        var receivedAt = _clock.GetTimestamp();
        if (message.MatchId == Context.SpotId
            && _recipients.TryGetValue(actor.Context.ActorId, out var recipient))
            recipient.ReplyToPing(message, _match.LatestTick, TickUs, receivedAt, _clock);
    }

    private sealed class MatchClock(TimeProvider clock) : TimeProvider
    {
        public long Sample { get; private set; }

        public override long TimestampFrequency => clock.TimestampFrequency;

        public override long GetTimestamp() => Sample = clock.GetTimestamp();
    }

    private sealed class Simulation(NetworkSyncSpot<TSpot, TActor, TInput, TAction, TState> spot)
        : INetworkSyncSimulation<TInput, TAction>
    {
        public TInput GetMissingInput(string actorId) => spot.GetMissingInput(actorId);

        public void Step(TickContext tick, ReadOnlySpan<PlayerTickInput<TInput, TAction>> inputs)
        {
            spot.Step(tick.Epoch, tick.Tick, new(inputs));
            foreach (ref readonly var input in inputs)
                spot._recipients[input.ActorId].RecordAppliedInput(input.LastAppliedInputTick, input.UsedFallbackInput);
        }
    }

    private sealed class TickHandler : IZLinkSpotTimerHandler<TSpot>
    {
        public ValueTask HandleAsync(TSpot spot, ZLinkTimerTick tick, CancellationToken cancellationToken)
        {
            spot.OnTimer();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class InputHandler : IZLinkSpotActorSendHandler<TSpot, TActor, NetworkSyncInputV1<TInput, TAction>>
    {
        public ValueTask HandleAsync(TSpot spot, TActor actor, IZLinkMessageContext context,
            NetworkSyncInputV1<TInput, TAction> message, CancellationToken cancellationToken)
        {
            spot.ReceiveInput(actor, message);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class PingHandler : IZLinkSpotActorSendHandler<TSpot, TActor, NetworkSyncPingV1>
    {
        public ValueTask HandleAsync(TSpot spot, TActor actor, IZLinkMessageContext context,
            NetworkSyncPingV1 message, CancellationToken cancellationToken)
        {
            spot.ReceivePing(actor, message);
            return ValueTask.CompletedTask;
        }
    }
}
