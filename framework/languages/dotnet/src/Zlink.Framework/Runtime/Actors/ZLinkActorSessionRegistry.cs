namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorSessionRegistry(
    IServiceProvider? services = null,
    Action<string>? handoffDiagnostic = null,
    TimeSpan? sessionBindingTombstoneRetention = null)
{
    private readonly object _gate = new();
    private readonly Dictionary<string, ZLinkActorRuntimeState> _states = new(StringComparer.Ordinal);

    public ZLinkActorRuntimeState GetOrCreate(string actorId)
    {
        lock (_gate)
        {
            if (_states.TryGetValue(actorId, out var existing)) return existing;

            var created = new ZLinkActorRuntimeState(
                actorId,
                handoffDiagnostic: handoffDiagnostic,
                sessionBindingTombstoneRetention:
                    sessionBindingTombstoneRetention,
                services: services);
            _states.Add(actorId, created);
            return created;
        }
    }

    public bool TryGet(string actorId, out ZLinkActorRuntimeState state)
    {
        lock (_gate)
        {
            return _states.TryGetValue(actorId, out state!);
        }
    }

    public void TryRemove(string actorId, ZLinkActorRuntimeState state)
    {
        lock (_gate)
        {
            if (_states.TryGetValue(actorId, out var existing)
                && ReferenceEquals(existing, state)
                && state.SessionId is null
                && state.Activation is null)
                _states.Remove(actorId);
        }
    }

    public void RemoveIfCurrent(string actorId, ZLinkActorRuntimeState state)
    {
        lock (_gate)
        {
            if (_states.TryGetValue(actorId, out var existing)
                && ReferenceEquals(existing, state))
                _states.Remove(actorId);
        }
    }

    public async ValueTask ResetGenerationAsync(
        CancellationToken cancellationToken = default,
        Action<Exception>? detachedCleanupFailure = null)
    {
        ZLinkActorRuntimeState[] states;
        lock (_gate)
        {
            states = _states.Values.ToArray();
            _states.Clear();
        }

        foreach (var state in states)
            state.FenceRuntimeGeneration();
        var cleanup = Task.WhenAll(
            states.Select(
                static state => state
                    .InvalidateRuntimeGenerationAfterDispatchesAsync()
                    .AsTask()));
        if (!cancellationToken.CanBeCanceled)
        {
            await cleanup.ConfigureAwait(false);
            return;
        }

        try
        {
            await cleanup.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
            _ = ObserveDetachedCleanupAsync(
                cleanup,
                detachedCleanupFailure);
            throw;
        }
    }

    private static async Task ObserveDetachedCleanupAsync(
        Task cleanup,
        Action<Exception>? reportFailure)
    {
        try
        {
            await cleanup.ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            reportFailure?.Invoke(exception);
        }
    }

    public ZLinkActorRuntimeState[] Snapshot()
    {
        lock (_gate) return _states.Values.ToArray();
    }
}
