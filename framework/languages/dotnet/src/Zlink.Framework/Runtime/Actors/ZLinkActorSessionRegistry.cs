using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorSessionRegistry(
    IServiceProvider? services = null,
    Action<string>? handoffDiagnostic = null,
    TimeSpan? sessionBindingTombstoneRetention = null)
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Dictionary<ZLinkActorId, ZLinkActorRuntimeState> _states = [];

    public ZLinkActorRuntimeState GetOrCreate(ZLinkActorId actorId)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
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
        }));
    }

    public bool TryGet(ZLinkActorId actorId, out ZLinkActorRuntimeState state)
    {
        var lookup = AwaitStateLane(_lane.RunAsync(() =>
        {
            var found = _states.TryGetValue(actorId, out var current);
            return (found, current);
        }));
        state = lookup.current!;
        return lookup.found;
    }

    public void TryRemove(ZLinkActorId actorId, ZLinkActorRuntimeState state)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_states.TryGetValue(actorId, out var existing)
                && ReferenceEquals(existing, state)
                && state.SessionId is null
                && state.Activation is null)
                _states.Remove(actorId);
        }));
    }

    public void RemoveIfCurrent(ZLinkActorId actorId, ZLinkActorRuntimeState state)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_states.TryGetValue(actorId, out var existing)
                && ReferenceEquals(existing, state))
                _states.Remove(actorId);
        }));
    }

    public async ValueTask ResetGenerationAsync(
        CancellationToken cancellationToken = default,
        Action<Exception>? detachedCleanupFailure = null)
    {
        // The reset call itself is the force-stop admission boundary. Fence
        // the snapshot before clearing it so the registry cannot publish a
        // successor while the previous runtime generation is still usable.
        var states = AwaitStateLane(_lane.RunAsync(() =>
        {
            var snapshot = _states.Values.ToArray();
            foreach (var state in snapshot)
                state.FenceRuntimeGeneration();
            _states.Clear();
            return snapshot;
        }));
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
        return AwaitStateLane(_lane.RunAsync(() => _states.Values.ToArray()));
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();
}
