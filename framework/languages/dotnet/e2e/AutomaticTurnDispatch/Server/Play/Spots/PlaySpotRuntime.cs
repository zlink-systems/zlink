using AutomaticTurnDispatch.Shared;
using AutomaticTurnDispatch.Server.Play.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace AutomaticTurnDispatch.Server.Play.Spots;

internal class AwaitProbeSpot(
    IZLinkSpotContext context,
    EvidenceStore evidence) : IZLinkSpot<AwaitActor>
{
    private readonly object _timerGate = new();
    private readonly Dictionary<string, AwaitTimerState> _timers = new(StringComparer.Ordinal);
    private readonly Dictionary<string, AwaitActor> _actors = new(StringComparer.Ordinal);
    private int _counter;

    public IZLinkSpotContext Context { get; } = context;

    public virtual void Configure()
    {
    }

    public int ReadCounter() => _counter;

    public void WriteCounter(int value) => _counter = value;

    public void ResetCounter() => _counter = 0;

    public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!request.IsEmpty)
        {
            var delay = request.Decode<JoinDelayReq>();
            if (delay.DelayMs > 0) await Task.Delay(TimeSpan.FromMilliseconds(delay.DelayMs), cancellationToken);
        }

        evidence.Add($"actor-admitted|rid={evidence.Rid}|spot={Context.SpotId}|actor={actorId}");
        return ZLinkSpotActorJoinResult.Accept(request);
    }

    public ValueTask OnJoinedActorAsync(AwaitActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_actors) _actors[actor.ActorId] = actor;
        evidence.Add($"actor-joined|rid={evidence.Rid}|spot={Context.SpotId}|actor={actor.ActorId}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(AwaitActor actor, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_actors) _actors.Remove(actor.ActorId);
        evidence.Add($"actor-left|rid={evidence.Rid}|spot={Context.SpotId}|actor={actor.ActorId}");
        return ValueTask.CompletedTask;
    }

    public AwaitActor? FindActor(string actorId)
    {
        lock (_actors)
            return _actors.TryGetValue(actorId, out var actor) ? actor : null;
    }

    public bool TryAddTimerState(AwaitTimerState state)
    {
        lock (_timerGate)
        {
            if (_timers.ContainsKey(state.TimerName)) return false;

            _timers[state.TimerName] = state;
            return true;
        }
    }

    public AwaitTimerState? FindTimerState(string timerName)
    {
        lock (_timerGate)
        {
            return _timers.TryGetValue(timerName, out var state) ? state : null;
        }
    }

    public async ValueTask StopScenarioTimersAsync(string requestId)
    {
        List<AwaitTimerState> states;
        lock (_timerGate)
        {
            states = _timers.Values
                .Where(state => string.Equals(state.RequestId, requestId, StringComparison.Ordinal))
                .ToList();
            foreach (var state in states) _timers.Remove(state.TimerName);
        }

        foreach (var state in states)
            if (state.Timer is not null)
                await state.Timer.CancelAsync();
    }
}

internal sealed class PerActorAwaitSpot : AwaitProbeSpot
{
    public PerActorAwaitSpot(
        IZLinkSpotContext context,
        EvidenceStore evidence) : base(context, evidence)
    {
    }

    public override void Configure()
    {
        Context.Handlers.AddActorPacket<PerActorAwaitHandler, AwaitActor>("PerActorAwaitReq");
        Context.Handlers.AddActorPacket<PerActorFastHandler, AwaitActor>("PerActorFastReq");
    }
}

internal sealed class AwaitTimerState(
    string requestId,
    string timerName,
    string mode,
    int delayMs)
{
    private int _tickCount;

    public string RequestId { get; } = requestId;

    public string TimerName { get; } = timerName;

    public string Mode { get; } = mode;

    public int DelayMs { get; } = delayMs;

    public IZLinkTimer? Timer { get; set; }

    public int NextTick()
    {
        return Interlocked.Increment(ref _tickCount);
    }
}
