using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Runtime.Locations;

internal sealed partial class ZLinkLocationRuntime
{
    internal Func<IReadOnlySet<object>, ValueTask>? DiscardExecutionsAsync { get; set; }

    internal Func<
        object,
        ZLinkAuthoritySnapshot,
        ValueTask
    >? ForgetDiscardedAuthorityAsync { get; set; }

    internal Action<Exception>? DiscardExecutionFailed { get; set; }

    private readonly Dictionary<object, DiscardedExecution> _discardedExecutions = [];

    private sealed record DiscardedExecution(
        ZLinkAuthorityKey? Key,
        object? Parent,
        Task<ZLinkAuthoritySnapshot?> Snapshot,
        Task LocalCompletion
    );

    internal async ValueTask DiscardExecutionsAndWaitAsync(IReadOnlySet<object> executions)
    {
        var completions = await _lane
            .RunAsync(() =>
            {
                DiscardExecutionsOnLane(executions);
                return executions
                    .Where(_discardedExecutions.ContainsKey)
                    .Select(item => _discardedExecutions[item].LocalCompletion)
                    .ToArray();
            })
            .ConfigureAwait(false);
        await Task.WhenAll(completions).ConfigureAwait(false);
    }

    private (
        object Execution,
        object? Parent,
        ZLinkAuthorityKey? Key,
        Task<ZLinkAuthoritySnapshot?> Snapshot
    ) CaptureDiscardedExecutionOnLane(object execution) =>
        (
            execution,
            (execution as ZLinkActorRuntimeState)?.Activation,
            execution switch
            {
                ZLinkActorRuntimeState actor => ZLinkActorAuthorityPayloadCodec.AuthorityKey(
                    actor.ActorId
                ),
                ZLinkSpotActivation spot when spot.SpotKind != ZLinkSpotKind.Entry =>
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(spot.SpotId),
                _ => (ZLinkAuthorityKey?)null,
            },
            ReadPreservedAuthorityAsync?.Invoke(execution).AsTask()
                ?? Task.FromResult<ZLinkAuthoritySnapshot?>(null)
        );

    internal async ValueTask CaptureDiscardedMembersAsync(
        object parent,
        IReadOnlySet<object> members
    )
    {
        var snapshots = await _lane
            .RunAsync(() =>
            {
                var completion = _discardedExecutions[parent].LocalCompletion;
                var captured = members
                    .Where(item => !_discardedExecutions.ContainsKey(item))
                    .Select(CaptureDiscardedExecutionOnLane)
                    .ToArray();
                foreach (var item in captured)
                    _discardedExecutions.Add(
                        item.Execution,
                        new DiscardedExecution(item.Key, item.Parent, item.Snapshot, completion)
                    );
                ExecutionsDiscarded?.Invoke(members);
                return captured.Select(item => item.Snapshot).ToArray();
            })
            .ConfigureAwait(false);
        await Task.WhenAll(snapshots).ConfigureAwait(false);
    }

    private void DiscardExecutionsOnLane(IReadOnlySet<object> executions)
    {
        var discarded = new HashSet<object>(executions);
        foreach (
            var member in (
                CaptureExecutions?.Invoke() ?? new HashSet<object>()
            ).OfType<ZLinkActorRuntimeState>()
        )
            if (member.Activation is { } parent && executions.Contains(parent))
                discarded.Add(member);
        var snapshots = discarded
            .Where(item => !_discardedExecutions.ContainsKey(item))
            .Select(CaptureDiscardedExecutionOnLane)
            .ToArray();
        if (snapshots.Length == 0)
            return;
        // Publish the terminal fence immediately; local resources remain until the captured
        // authority and the existing disposal continuation have both completed.
        ExecutionsDiscarded?.Invoke(discarded);
        var localCompletion = ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(() =>
            Task.Run(DiscardLocalAsync)
        );
        _ = localCompletion.ContinueWith(
            task =>
            {
                var error = task.Exception!;
                RecordStoreFailure(error.Message);
                DiscardExecutionFailed?.Invoke(error);
            },
            CancellationToken.None,
            TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default
        );
        foreach (var item in snapshots)
            _discardedExecutions.Add(
                item.Execution,
                new DiscardedExecution(item.Key, item.Parent, item.Snapshot, localCompletion)
            );

        async Task DiscardLocalAsync()
        {
            await Task.WhenAll(snapshots.Select(item => item.Snapshot)).ConfigureAwait(false);
            if (DiscardExecutionsAsync is { } dispose)
                await dispose(discarded).ConfigureAwait(false);
        }
    }

    internal async ValueTask DrainDiscardedLocalExecutionsAsync(CancellationToken cancellationToken)
    {
        var completions = await _lane
            .RunAsync(() =>
                _discardedExecutions
                    .Values.Select(item => item.LocalCompletion)
                    .Distinct()
                    .ToArray()
            )
            .ConfigureAwait(false);
        await Task.WhenAll(completions).WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    private async ValueTask CleanupDiscardedExecutionsAsync(CancellationToken cancellationToken)
    {
        var pending = await _lane
            .RunAsync(() => _discardedExecutions.ToArray())
            .ConfigureAwait(false);
        foreach (
            var (execution, discarded) in pending.OrderBy(item => item.Key is ZLinkSpotActivation)
        )
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!discarded.LocalCompletion.IsCompletedSuccessfully)
                continue;
            if (
                execution is ZLinkSpotActivation
                && await _lane
                    .RunAsync(() =>
                        _discardedExecutions.Values.Any(item =>
                            ReferenceEquals(item.Parent, execution)
                        )
                    )
                    .ConfigureAwait(false)
            )
                continue;
            var snapshot = await discarded.Snapshot.ConfigureAwait(false);
            try
            {
                if (
                    snapshot is not null
                    && discarded.Key is { } key
                    && !await _store
                        .CleanupDiscardedAuthorityAsync(
                            key,
                            snapshot,
                            await GetOwnerTokenAsync().ConfigureAwait(false),
                            cancellationToken
                        )
                        .AsTask()
                        .WaitAsync(cancellationToken)
                        .ConfigureAwait(false)
                )
                    continue;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception error)
                when (error is TimeoutException or OperationCanceledException
                    || error
                        is ZLinkFrameworkException { Kind: ZLinkFrameworkErrorKind.Unavailable }
                )
            {
                continue;
            }
            if (snapshot is not null && ForgetDiscardedAuthorityAsync is { } forget)
                await forget(execution, snapshot).ConfigureAwait(false);
            await _lane
                .RunAsync(() =>
                {
                    _discardedExecutions.Remove(execution);
                    GetHealthSnapshot().StoreOutage?.CompleteExecution(execution);
                })
                .ConfigureAwait(false);
        }
    }
}
