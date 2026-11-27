namespace Zlink.NetworkSync.Simulation;

/// <summary>Runs one match from serialized owner callbacks using one monotonic clock.</summary>
internal sealed class NetworkSyncMatch<TInput, TAction>
{
    private readonly TimeProvider _clock;
    private readonly INetworkSyncSimulation<TInput, TAction> _simulation;
    private readonly SimulationSettings _settings;
    private readonly MatchTimeline _timeline;
    private readonly MatchInputBuffer<TInput, TAction> _inputs;
    private readonly Int128 _wakeupBudget;
    private readonly Int128 _maxBacklogTicks;

    public NetworkSyncMatch(
        string matchId,
        TimeProvider clock,
        INetworkSyncSimulation<TInput, TAction> simulation,
        SimulationSettings? settings = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(matchId);
        ArgumentNullException.ThrowIfNull(clock);
        ArgumentNullException.ThrowIfNull(simulation);
        _settings = settings ?? new();
        _settings.Validate(clock.TimestampFrequency);

        _clock = clock;
        _simulation = simulation;
        _timeline = new(clock.TimestampFrequency, _settings.TickRate, clock.GetTimestamp());
        _inputs = new(matchId, _timeline, _settings);
        _wakeupBudget = _timeline.BudgetClockTicks(_settings.WakeupBudget);
        _maxBacklogTicks = (Int128)_settings.MaxBacklog.Ticks * _settings.TickRate / TimeSpan.TicksPerSecond;
    }

    public TickContext LatestTick => _timeline.LatestTick;

    public bool AddPlayer(string actorId) => _inputs.AddPlayer(actorId);

    public bool RemovePlayer(string actorId) => _inputs.RemovePlayer(actorId);

    /// <param name="matchId">Match identifier from the input message.</param>
    /// <param name="epoch">Epoch from the input message.</param>
    /// <param name="actorId">Current actor identity supplied by the authenticated caller.</param>
    /// <param name="targetTick">Tick requested by the input message.</param>
    /// <param name="encodedBytes">Actual encoded message size measured by the caller.</param>
    /// <param name="input">Immutable hold input for the target tick.</param>
    /// <param name="actions">Actions with immutable payloads. The buffer copies the batch on acceptance.</param>
    public InputFeedback SubmitInput(
        string matchId,
        long epoch,
        string actorId,
        long targetTick,
        int encodedBytes,
        TInput input,
        ReadOnlySpan<SimulationAction<TAction>> actions = default) =>
        _inputs.Submit(matchId, epoch, actorId, targetTick, _clock.GetTimestamp(), encodedBytes, input, actions);

    public WakeupResult Wakeup()
    {
        var startedAt = _clock.GetTimestamp();
        var previousTick = LatestTick.Tick;
        var backlog = _timeline.TickAt(startedAt) - previousTick;
        if (backlog > _maxBacklogTicks)
            return HandleOverload(startedAt, backlog);

        var limit = Math.Min(backlog, _settings.MaxTicksPerWakeup);
        var elapsed = unchecked(_clock.GetTimestamp() - startedAt);
        var executed = 0;
        // Step is synchronous. Check the budget before starting another step.
        while (executed < limit && elapsed < _wakeupBudget)
        {
            var tick = _timeline.At(LatestTick.Tick + 1);
            _inputs.Step(tick, _simulation);
            _timeline.Complete(tick);
            executed++;
            elapsed = unchecked(_clock.GetTimestamp() - startedAt);
        }

        var snapshotDue = SnapshotSlot(LatestTick.Tick) > SnapshotSlot(previousTick);
        return new(LatestTick, backlog, executed, 0, _timeline.Duration(elapsed), snapshotDue);
    }

    private WakeupResult HandleOverload(long startedAt, long backlog)
    {
        switch (_settings.OverloadPolicy)
        {
            case OverloadPolicy.Resync:
                _timeline.Resync(startedAt);
                _inputs.ResetEpoch();
                var elapsed = _timeline.Duration(unchecked(_clock.GetTimestamp() - startedAt));
                return new(LatestTick, backlog, 0, backlog, elapsed, true);
            default:
                throw new InvalidOperationException("Unsupported overload policy.");
        }
    }

    private Int128 SnapshotSlot(long tick) => (Int128)tick * _settings.SnapshotRate / _settings.TickRate;
}
