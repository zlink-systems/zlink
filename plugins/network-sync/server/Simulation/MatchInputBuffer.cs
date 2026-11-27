using System.Runtime.InteropServices;

namespace Zlink.NetworkSync.Simulation;

internal sealed class MatchInputBuffer<TInput, TAction>(
    string matchId,
    MatchTimeline timeline,
    SimulationSettings settings)
{
    private readonly Dictionary<string, Player> _players = new(StringComparer.Ordinal);
    // Action IDs increase per Actor within an epoch (STREAM delivers one Actor's inputs in order),
    // so the last accepted ID is the whole duplicate history.
    private readonly Dictionary<string, ulong> _lastActionIds = new(StringComparer.Ordinal);
    private readonly List<PlayerTickInput<TInput, TAction>> _tickInputs = [];
    private readonly int _futureTicks = settings.MaxFutureTicks ?? settings.TickRate;

    public bool AddPlayer(string actorId)
    {
        ArgumentException.ThrowIfNullOrEmpty(actorId);
        return _players.TryAdd(actorId, new());
    }

    public bool RemovePlayer(string actorId) => _players.Remove(actorId);

    public InputFeedback Submit(
        string inputMatchId,
        long epoch,
        string actorId,
        long targetTick,
        long receivedAt,
        int encodedBytes,
        TInput input,
        ReadOnlySpan<SimulationAction<TAction>> actions)
    {
        if (!string.Equals(matchId, inputMatchId, StringComparison.Ordinal))
            return new(InputDisposition.WrongMatch);
        if (epoch != timeline.LatestTick.Epoch)
            return new(InputDisposition.WrongEpoch);
        if (!_players.TryGetValue(actorId, out var player))
            return new(InputDisposition.UnknownActor);
        if (encodedBytes <= 0 || encodedBytes > settings.MaxInputBytes)
            return new(InputDisposition.InvalidSize);
        if (targetTick <= 0)
            return new(InputDisposition.InvalidTick);
        if (!player.RecordArrival(receivedAt, timeline.TimestampFrequency, settings.MaxInputsPerSecond))
            return new(InputDisposition.RateLimited);

        var clockMargin = timeline.ClockTicksUntil(targetTick, receivedAt);
        if (clockMargin <= 0)
            return new(InputDisposition.Late, timeline.Duration(clockMargin));
        // Bound retained input even when the logical clock advances without a wakeup.
        if (targetTick - timeline.LatestTick.Tick > _futureTicks)
            return new(InputDisposition.TooFarAhead);

        var margin = timeline.Duration(clockMargin);
        if (player.Pending.ContainsKey(targetTick))
            return new(InputDisposition.DuplicateTick, margin);

        if (!actions.IsEmpty)
        {
            ulong? previous = _lastActionIds.TryGetValue(actorId, out var last) ? last : null;
            foreach (var action in actions)
            {
                if (previous is { } floor && action.Id <= floor)
                    return new(InputDisposition.DuplicateAction, margin);
                previous = action.Id;
            }

            _lastActionIds[actorId] = previous!.Value;
        }

        player.Pending.Add(targetTick, new(input, actions.ToArray()));
        return new(InputDisposition.Accepted, margin);
    }

    public void Step(TickContext tick, INetworkSyncSimulation<TInput, TAction> simulation)
    {
        try
        {
            foreach (var (actorId, player) in _players)
            {
                if (player.Pending.TryGetValue(tick.Tick, out var pending))
                {
                    _tickInputs.Add(new(actorId, pending.Input, pending.Actions, tick.Tick, false));
                    continue;
                }

                var input = player.LastAppliedTick is { } applied && tick.Tick - applied <= settings.MaxHeldTicks
                    ? player.LastInput
                    : simulation.GetMissingInput(actorId);
                _tickInputs.Add(new(actorId, input, default, player.LastAppliedTick, true));
            }

            simulation.Step(tick, CollectionsMarshal.AsSpan(_tickInputs));

            foreach (var input in _tickInputs)
            {
                if (input.UsedFallbackInput)
                    continue;

                var player = _players[input.ActorId];
                player.Pending.Remove(tick.Tick);
                player.LastInput = input.Input;
                player.LastAppliedTick = tick.Tick;
            }
        }
        finally
        {
            _tickInputs.Clear();
        }
    }

    public void ResetEpoch()
    {
        _lastActionIds.Clear();
        foreach (var player in _players.Values)
        {
            player.Pending.Clear();
            player.LastInput = default!;
            player.LastAppliedTick = null;
        }
    }

    private readonly record struct PendingInput(TInput Input, SimulationAction<TAction>[] Actions);

    private sealed class Player
    {
        private readonly Queue<long> _arrivals = new();

        public Dictionary<long, PendingInput> Pending { get; } = [];
        public TInput LastInput { get; set; } = default!;
        public long? LastAppliedTick { get; set; }

        public bool RecordArrival(long timestamp, long window, int limit)
        {
            while (_arrivals.TryPeek(out var oldest) && unchecked(timestamp - oldest) >= window)
                _arrivals.Dequeue();

            if (_arrivals.Count >= limit)
                return false;

            _arrivals.Enqueue(timestamp);
            return true;
        }
    }
}
