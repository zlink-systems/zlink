using Zlink.Framework.Internal.Locations;
using Zlink.Framework.Runtime.Execution;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Locations;

internal readonly record struct ZLinkOwnerLeaseRenewal(
    DateTimeOffset LeaseExpiresAt,
    DateTimeOffset StoreNow
);

internal sealed record ZLinkOwnerAdmissionDeadline(long LeaseOperationStartedAt, TimeSpan Lifetime);

internal sealed record ZLinkStoreOutageEpisode(
    long ObservedAt,
    long LeaseOperationStartedAt,
    TimeSpan Lifetime,
    ZLinkLocationOwnerToken Owner,
    bool Discarded = false
)
{
    internal IReadOnlySet<object> Executions { get; init; } = new HashSet<object>();
}

internal sealed class ZLinkOwnerLeaseClaimRejectedException(ZLinkOwnerLeaseClaimResult result)
    : InvalidOperationException($"Owner lease claim failed with '{result.GetType().Name}'.");

internal sealed class ZLinkOwnerLeaseCancellationCleanupException(
    OperationCanceledException cancellation,
    Exception cleanupFailure
)
    : OperationCanceledException(
        cancellation.Message,
        cleanupFailure,
        cancellation.CancellationToken
    );

/// <summary>
/// Owns this runtime instance's location identity and lifecycle writes.
/// One owner lease heartbeat per interval keeps every row of this owner
/// alive; location rows themselves are written only on lifecycle changes.
/// A write that comes back IgnoredStale means another owner replaced the
/// row: subscribers must stop advertising and deactivate the local
/// instance (ownership-loss rule).
/// </summary>
internal sealed class ZLinkLocationRuntime : IAsyncDisposable
{
    private readonly ZLinkLocationOptions _options;
    private readonly IZLinkLocationRepository _store;
    private readonly TimeProvider _time;
    internal TimeProvider TimeProvider => _time;
    private readonly IReadOnlyList<KeyValuePair<string, string>> _metricScopes;
    private readonly SemaphoreSlim _lifecycleGate = new(1, 1);
    private readonly ZLinkStateLane _lane = new();
    private ZLinkLocationRuntimeHealth _health = new(false, null, null, null);
    private CancellationTokenSource? _heartbeatCts;
    private Task? _heartbeatLoop;
    private RoutingId _nodeRid;
    private string _ownerId = Guid.NewGuid().ToString("n");
    private ZLinkLocationOwnerToken? _ownerToken;
    private ZLinkOwnerAdmissionDeadline? _ownerAdmissionDeadline;
    private (
        ZLinkLocationOwnerToken Token,
        ZLinkOwnerAdmissionDeadline Deadline
    )? _lastSuccessfulLease;
    private ITimer? _storeOutageTimer;
    private bool _started;
    private bool _ownerCleanedForDrain;
    private int _disposeState;
    private Task? _disposeTask;

    internal ZLinkLocationRuntime(
        ZLinkLocationOptions options,
        IZLinkLocationRepository store,
        TimeProvider? timeProvider = null,
        IReadOnlyList<KeyValuePair<string, string>>? metricScopes = null
    )
    {
        _options = options;
        _store = store;
        _time = timeProvider ?? TimeProvider.System;
        _metricScopes = metricScopes ?? [];
    }

    /// <summary>Stable owner id for the current runtime generation. Each
    /// successful restart attempt uses a fresh id so rows left by an older
    /// generation cannot become live again.</summary>
    internal string OwnerId => AwaitStateLane(_lane.RunAsync(() => _ownerId));

    internal ZLinkLocationOwnerToken OwnerToken =>
        AwaitStateLane(_lane.RunAsync(RequireOwnerTokenOnLane));

    internal ZLinkLocationOwnerToken AdmissionOwnerToken
    {
        get
        {
            return AwaitStateLane(
                _lane.RunAsync(() =>
                {
                    EnsureOwnerAdmissionOpenOnLane();
                    return RequireOwnerTokenOnLane();
                })
            );
        }
    }

    internal bool IsOwnerAdmissionOpen => IsOwnerAdmissionOpenOnLane();

    internal void EnsureOwnerAdmissionOpen()
    {
        EnsureOwnerAdmissionOpenOnLane();
    }

    internal IZLinkLocationRepository Store => _store;

    internal string? LastError => Volatile.Read(ref _health).LastError;

    internal ZLinkLocationRuntimeHealth GetHealthSnapshot() => Volatile.Read(ref _health);

    /// <summary>Raised when a write for a row this owner believed it owned
    /// came back IgnoredStale. The argument is the canonical location key.</summary>
    internal event Action<ZLinkLocationKind, string>? OwnershipLost;

    internal void NotifyAuthorityOwnershipLost(ZLinkLocationKind kind, ZLinkAuthorityKey key)
    {
        OwnershipLost?.Invoke(kind, key.Value);
    }

    internal event Action<ZLinkOwnerLeaseRenewal>? OwnerLeaseRenewed;

    internal event Action? OwnerLeaseRenewalFailed;

    internal Func<IReadOnlySet<object>>? CaptureExecutions { get; set; }

    internal event Action<IReadOnlySet<object>>? ExecutionsDiscarded;

    private IReadOnlySet<object> CaptureSteadyExecutionsOnLane()
    {
        var candidates = CaptureExecutions?.Invoke() ?? new HashSet<object>();
        var executions = new HashSet<object>();
        foreach (var spot in candidates.OfType<ZLinkSpotActivation>())
            if (
                !spot.IsDisposed
                && !spot.HasCommittedClose
                && !spot._serial.IsPreparing
                && !spot.HasRelocationBarrier
            )
                executions.Add(spot);
        foreach (var actor in candidates.OfType<ZLinkActorRuntimeState>())
            if (
                IsSteadyActor(actor)
                && (actor.Activation is null || executions.Contains(actor.Activation))
            )
                executions.Add(actor);
        return executions;
    }

    private static bool IsSteadyActor(ZLinkActorRuntimeState actor) =>
        actor.Actor is not null
        && Volatile.Read(ref actor._deferredJoinPending) == 0
        && !actor.ContextInvalidated
        && !actor.IsDispatchBlocked
        && !actor.Handoff.BlocksLocalDispatch;

    internal static bool IsRelocationLifecycleOwned(ZLinkActorRuntimeState actor) =>
        actor.Handoff.IsSourceMigrationInProgress
        || (actor.Actor is null && actor.Handoff.RetainsSourceTombstone);

    internal bool IsBoundExecutionOpen => IsExecutionAllowed(null);

    internal bool IsExecutionAllowed(object? execution)
    {
        var episode = GetHealthSnapshot().StoreOutage;
        if (episode is null)
            return execution is not null || IsOwnerAdmissionOpenOnLane();
        var retained = !episode.Discarded && episode.Executions.Count > 0;
        if (execution is null)
            return retained || IsOwnerAdmissionOpenOnLane();
        var preserved = episode.Executions.Contains(execution);
        if (execution is ZLinkActorRuntimeState { Activation: { } parent })
        {
            if (!episode.Executions.Contains(parent))
                return IsOwnerAdmissionOpenOnLane() && (!preserved || retained);
            if (!retained)
                return false;
        }
        return preserved ? retained : IsOwnerAdmissionOpenOnLane();
    }

    internal void EnsureRelocationAdmissionOpen()
    {
        EnsureOwnerAdmissionOpen();
        if (GetHealthSnapshot().StoreOutage is not null)
            throw new InvalidOperationException(
                "The location Store is unavailable for relocation."
            );
    }

    internal async ValueTask StartAsync(
        RoutingId nodeRid,
        CancellationToken cancellationToken = default,
        CancellationToken? cancellationCleanupToken = null
    )
    {
        ThrowIfDisposingOrDisposed();
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        using var ownerLeaseDeadline = new CancellationTokenSource(
            _options.OwnerLeaseRenewTimeout,
            _time
        );
        try
        {
            var start = await _lane
                .RunAsync(() =>
                {
                    ThrowIfDisposingOrDisposed();
                    if (_started)
                        return false;

                    _nodeRid = nodeRid;
                    _ownerId = Guid.NewGuid().ToString("n");
                    _ownerToken = null;
                    _ownerAdmissionDeadline = null;
                    _lastSuccessfulLease = null;
                    _storeOutageTimer?.Dispose();
                    _storeOutageTimer = null;
                    UpdateHealth(health => health with { StoreOutage = null });
                    _ownerCleanedForDrain = false;
                    return true;
                })
                .ConfigureAwait(false);
            if (!start)
                return;

            // Register liveness before any row write so readers joining rows
            // against the lease never see this owner's rows as stale on start.
            // An unavailable store leaves this generation without an admission
            // deadline; the shared heartbeat path will claim it after recovery.
            // The claim attempt observes caller cancellation and cleans up
            // the claim it started (spec 01-location-runtime §5, startup).
            var leaseOperationStartedAt = _time.GetTimestamp();
            _ = await RenewOwnerLeaseOnceAsync(
                    leaseOperationStartedAt,
                    cancellationToken,
                    ownerLeaseDeadline.Token,
                    cancellationCleanupToken
                )
                .ConfigureAwait(false);

            var heartbeatStarted = await _lane
                .RunAsync(() =>
                {
                    _started = true;
                    return StartHeartbeatOnLane(leaseOperationStartedAt);
                })
                .ConfigureAwait(false);
            heartbeatStarted.TrySetResult();
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            await _lane
                .RunAsync(() =>
                {
                    _ownerToken = null;
                    _ownerAdmissionDeadline = null;
                })
                .ConfigureAwait(false);
            throw;
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask StopAsync(CancellationToken cancellationToken = default)
    {
        await _lifecycleGate.WaitAsync().ConfigureAwait(false);
        try
        {
            var stop = await _lane
                .RunAsync(() =>
                {
                    if (!_started)
                        return (StopState?)null;

                    _started = false;
                    _storeOutageTimer?.Dispose();
                    _storeOutageTimer = null;
                    _lastSuccessfulLease = null;
                    var heartbeat = _heartbeatCts;
                    var heartbeatLoop = _heartbeatLoop;
                    _heartbeatCts = null;
                    _heartbeatLoop = null;
                    return new StopState(
                        heartbeat,
                        heartbeatLoop,
                        _ownerToken,
                        _ownerCleanedForDrain
                    );
                })
                .ConfigureAwait(false);
            if (stop is null)
                return;
            var heartbeat = stop.Value.Heartbeat;
            var heartbeatLoop = stop.Value.HeartbeatLoop;
            if (heartbeat is not null)
                await heartbeat.CancelAsync().ConfigureAwait(false);

            if (heartbeatLoop is not null)
            {
                try
                {
                    await heartbeatLoop.ConfigureAwait(false);
                }
                catch (OperationCanceledException) { }
            }

            heartbeat?.Dispose();
            UpdateHealth(static health => health with { Healthy = false });

            // Keep the lease valid while owned rows are removed. Allocated routing-id slots are
            // released by their lifecycle before this method removes the shared owner lease.
            // That order prevents another runtime from taking the same id while an old socket or
            // location row can still be observed.
            try
            {
                if (!stop.Value.OwnerCleanedForDrain)
                {
                    if (stop.Value.OwnerToken is { } token)
                    {
                        cancellationToken.ThrowIfCancellationRequested();
                        await ObserveStoreAsync(
                                () => _store.RemoveAllByOwnerAsync(token, cancellationToken),
                                "release"
                            )
                            .ConfigureAwait(false);
                        cancellationToken.ThrowIfCancellationRequested();
                        _ = await ObserveStoreAsync(
                                () => _store.ReleaseOwnerLeaseAsync(token, cancellationToken),
                                "release"
                            )
                            .ConfigureAwait(false);
                        cancellationToken.ThrowIfCancellationRequested();
                    }
                    await _lane
                        .RunAsync(() =>
                        {
                            _ownerToken = null;
                            _ownerAdmissionDeadline = null;
                        })
                        .ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception exception)
            {
                // A store outage during shutdown is not fatal: the expired
                // lease makes the leftover rows stale and background cleanup
                // removes them later.
                RecordStoreFailure(exception.Message);
            }
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask CleanupOwnerForDrainAsync(CancellationToken cancellationToken)
    {
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var cleanup = await _lane
                .RunAsync(() =>
                {
                    if (!_started || _ownerCleanedForDrain)
                        return (ZLinkLocationOwnerToken?)null;
                    return RequireOwnerTokenOnLane();
                })
                .ConfigureAwait(false);
            if (cleanup is null)
                return;

            // Keep the owner lease valid while rows are removed. If row
            // cleanup fails, the heartbeat continues and the next retry sees
            // the same live owner rather than stale state.
            await ObserveStoreAsync(
                    () => _store.RemoveAllByOwnerAsync(cleanup.Value, cancellationToken),
                    "release"
                )
                .ConfigureAwait(false);

            var heartbeatState = await _lane
                .RunAsync(() =>
                {
                    var heartbeat = _heartbeatCts;
                    var heartbeatLoop = _heartbeatLoop;
                    _heartbeatCts = null;
                    _heartbeatLoop = null;
                    return (heartbeat, heartbeatLoop);
                })
                .ConfigureAwait(false);
            var heartbeat = heartbeatState.heartbeat;
            var heartbeatLoop = heartbeatState.heartbeatLoop;
            if (heartbeat is not null)
                await heartbeat.CancelAsync().ConfigureAwait(false);
            if (heartbeatLoop is not null)
                try
                {
                    await heartbeatLoop.ConfigureAwait(false);
                }
                catch (OperationCanceledException) { }
            heartbeat?.Dispose();

            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (cleanup.Value is { } token)
                    _ = await ObserveStoreAsync(
                            () => _store.ReleaseOwnerLeaseAsync(token, cancellationToken),
                            "release"
                        )
                        .ConfigureAwait(false);
                cancellationToken.ThrowIfCancellationRequested();
                await _lane
                    .RunAsync(() =>
                    {
                        _ownerToken = null;
                        _ownerAdmissionDeadline = null;
                    })
                    .ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                RecordStoreFailure(exception.Message);
                if (!cancellationToken.IsCancellationRequested)
                    StartHeartbeatAfterCleanupFailure();
                throw;
            }
            await _lane.RunAsync(() => _ownerCleanedForDrain = true).ConfigureAwait(false);
            UpdateHealth(static health => health with { Healthy = false });
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask RemoveOwnedRowsBeforeRoutingIdReleaseAsync(
        CancellationToken cancellationToken
    )
    {
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var token = await _lane
                .RunAsync(() =>
                {
                    if (!_started || _ownerCleanedForDrain)
                        return null;
                    return _ownerToken;
                })
                .ConfigureAwait(false);
            if (token is null)
                return;
            _ = await ObserveStoreAsync(
                    () => _store.RemoveAllByOwnerAsync(token.Value, cancellationToken),
                    "release"
                )
                .ConfigureAwait(false);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    private ZLinkLocationOwnerToken RequireOwnerTokenOnLane() =>
        _ownerToken
        ?? throw new InvalidOperationException(
            "The location runtime has not claimed its owner lease."
        );

    private void StartHeartbeatAfterCleanupFailure()
    {
        var heartbeatStarted = AwaitStateLane(
            _lane.RunAsync(() =>
            {
                if (!_started || _heartbeatCts is not null)
                    return (TaskCompletionSource?)null;
                return StartHeartbeatOnLane(
                    _ownerAdmissionDeadline?.LeaseOperationStartedAt ?? _time.GetTimestamp()
                );
            })
        );
        heartbeatStarted?.TrySetResult();
    }

    public ValueTask DisposeAsync()
    {
        return new ValueTask(
            AwaitStateLane(
                _lane.RunAsync(() =>
                {
                    if (_disposeTask is not null)
                        return _disposeTask;
                    Volatile.Write(ref _disposeState, 1);
                    _disposeTask = StartDisposeCore();
                    return _disposeTask;
                })
            )
        );
    }

    private Task StartDisposeCore()
    {
        if (ExecutionContext.IsFlowSuppressed())
            return Task.Run(DisposeCoreAsync, CancellationToken.None);

        using (ExecutionContext.SuppressFlow())
            return Task.Run(DisposeCoreAsync, CancellationToken.None);
    }

    private async Task DisposeCoreAsync()
    {
        try
        {
            await StopAsync().ConfigureAwait(false);
        }
        finally
        {
            await _lane
                .RunAsync(() =>
                {
                    _storeOutageTimer?.Dispose();
                    _storeOutageTimer = null;
                })
                .ConfigureAwait(false);
            Volatile.Write(ref _disposeState, 2);
        }
    }

    private void ThrowIfDisposingOrDisposed()
    {
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposeState) != 0, this);
    }

    internal async ValueTask<ZLinkLocationWriteResult> WriteDescriptorAsync(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent,
        CancellationToken cancellationToken = default
    )
    {
        var ownerToken = AdmissionOwnerToken;
        var stamped = descriptor with
        {
            OwnerId = ownerToken.OwnerId,
            LeaseGeneration = ownerToken.LeaseGeneration,
            // Stamp the canonical descriptor before the provider persists it
            // so Redis receives the write timestamp.
            UpdatedAt = _time.GetUtcNow(),
        };
        var result = await ExecuteLocationWriteAsync(
                () =>
                    ZLinkDescriptorPublicationUnconfirmedException.WriteAsync(
                        token => _store.UpdateMeshNodeAsync(stamped, intent, token),
                        _options.OwnerLeaseRenewTimeout,
                        cancellationToken,
                        _time
                    ),
                ZLinkLocationKind.MeshNode,
                ZLinkLocationKeyCodec.EncodeMeshNodeKey(
                    new ZLinkMeshNodeDescriptorKey(descriptor.MeshName, descriptor.Rid)
                )
            )
            .ConfigureAwait(false);
        return result;
    }

    internal async ValueTask<ZLinkLocationWriteResult> RemoveDescriptorAsync(
        ZLinkMeshNodeDescriptorKey key,
        CancellationToken cancellationToken = default
    )
    {
        return await ExecuteRemoveAsync(
                () => _store.RemoveMeshNodeAsync(key, OwnerToken, cancellationToken),
                ZLinkLocationKind.MeshNode,
                ZLinkLocationKeyCodec.EncodeMeshNodeKey(key)
            )
            .ConfigureAwait(false);
    }

    internal async ValueTask RemoveDescriptorForShutdownAsync(
        ZLinkMeshNodeDescriptorKey key,
        CancellationToken cancellationToken = default
    )
    {
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            // Owner cleanup releases the lifecycle lease and preserves publisher
            // descriptors. Teardown after that release only closes local resources;
            // it cannot delete the descriptor using another lifecycle's lease.
            var token = await _lane
                .RunAsync(() =>
                {
                    if (_ownerCleanedForDrain)
                        return (ZLinkLocationOwnerToken?)null;
                    return RequireOwnerTokenOnLane();
                })
                .ConfigureAwait(false);
            if (token is null)
                return;

            _ = await ExecuteRemoveAsync(
                    () => _store.RemoveMeshNodeAsync(key, token.Value, cancellationToken),
                    ZLinkLocationKind.MeshNode,
                    ZLinkLocationKeyCodec.EncodeMeshNodeKey(key)
                )
                .ConfigureAwait(false);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask<bool> RenewOwnerLeaseOnceAsync(
        CancellationToken cancellationToken = default
    )
    {
        using var deadline = new CancellationTokenSource(_options.OwnerLeaseRenewTimeout, _time);
        return await RenewOwnerLeaseOnceAsync(
                _time.GetTimestamp(),
                cancellationToken,
                deadline.Token
            )
            .ConfigureAwait(false);
    }

    private async ValueTask<bool> RenewOwnerLeaseOnceAsync(
        long leaseOperationStartedAt,
        CancellationToken cancellationToken,
        CancellationToken deadlineToken,
        CancellationToken? cancellationCleanupToken = null
    )
    {
        _options.FreezeStoreOutageRetention();
        OwnerLeaseState owner = default;
        var readingOrRenewing = false;
        var claimStarted = false;
        Exception? claimFailure = null;
        ZLinkOwnerLeaseClaimResult? rejectedClaim = null;
        Task<ZLinkOwnerLeaseClaimResult>? pendingClaim = null;
        using var operation = CancellationTokenSource.CreateLinkedTokenSource(
            cancellationToken,
            deadlineToken
        );
        try
        {
            var renewed = await RenewOrClaimAsync().ConfigureAwait(false);
            // A claim this attempt started ends as cancelled once the caller
            // cancelled, whichever result the claim reached first.
            if (ClaimPending())
                cancellationToken.ThrowIfCancellationRequested();
            return renewed;
        }
        catch (OperationCanceledException cancellation)
            when (cancellationToken.IsCancellationRequested && ClaimPending())
        {
            // Spec 01-location-runtime §5: a claim already started is read
            // again and a confirmed lease is released. This is the one
            // cancellation cleanup of a claim, at startup and in the heartbeat.
            await RecordCancellationCleanupFailureAsync(
                    async () =>
                        await ReleaseClaimIfConfirmedAfterCancellationAsync(
                                owner.Id,
                                cancellationCleanupToken ?? deadlineToken,
                                cancellationCleanupToken.HasValue ? pendingClaim : null
                            )
                            .ConfigureAwait(false),
                    cancellation
                )
                .ConfigureAwait(false);
            throw;
        }

        bool ClaimPending() => claimStarted && owner.Token is null;

        async ValueTask<bool> RenewOrClaimAsync()
        {
            ZLinkOwnerLeaseRenewResult.Renewed result;
            while (true)
            {
                try
                {
                    owner = await _lane
                        .RunAsync(() => new OwnerLeaseState(_ownerId, _ownerToken))
                        .ConfigureAwait(false);
                    if (owner.Token is null)
                    {
                        if (claimFailure is not null || rejectedClaim is not null)
                        {
                            readingOrRenewing = true;
                            var confirmed = await ConfirmClaimAsync(owner.Id, deadlineToken)
                                .ConfigureAwait(false);
                            readingOrRenewing = false;
                            if (confirmed is not null)
                                return await CompleteOwnerLeaseRenewalAsync(
                                        confirmed,
                                        leaseOperationStartedAt
                                    )
                                    .ConfigureAwait(false);
                            if (rejectedClaim is not null)
                                throw new ZLinkOwnerLeaseClaimRejectedException(rejectedClaim);

                            RecordLeaseFailure(claimFailure!.Message);
                            OwnerLeaseRenewalFailed?.Invoke();
                            return false;
                        }

                        claimStarted = true;
                        pendingClaim = ObserveStoreAsync(
                                () =>
                                    _store.ClaimOwnerLeaseAsync(
                                        owner.Id,
                                        _options.OwnerLeaseTtl,
                                        operation.Token
                                    ),
                                "compare_exchange"
                            )
                            .AsTask();
                        var claim = await pendingClaim
                            .WaitAsync(operation.Token)
                            .ConfigureAwait(false);
                        if (claim is ZLinkOwnerLeaseClaimResult.Claimed claimed)
                        {
                            result = await InstallConfirmedClaimAsync(
                                    claimed.Token,
                                    claimed.LeaseExpiresAt,
                                    claimed.StoreNow
                                )
                                .ConfigureAwait(false);
                            return await CompleteOwnerLeaseRenewalAsync(
                                    result,
                                    leaseOperationStartedAt
                                )
                                .ConfigureAwait(false);
                        }
                        if (claim is ZLinkOwnerLeaseClaimResult.GenerationExhausted)
                            throw new ZLinkOwnerLeaseClaimRejectedException(claim);

                        rejectedClaim = claim;
                        continue;
                    }
                    else
                    {
                        readingOrRenewing = true;
                        var renewal = await ObserveStoreAsync(
                                () =>
                                    _store.RenewOwnerLeaseAsync(
                                        owner.Token.Value,
                                        _options.OwnerLeaseTtl,
                                        operation.Token
                                    ),
                                "lease_renew"
                            )
                            .AsTask()
                            .WaitAsync(deadlineToken)
                            .ConfigureAwait(false);
                        readingOrRenewing = false;
                        if (renewal is not ZLinkOwnerLeaseRenewResult.Renewed renewed)
                        {
                            await _lane
                                .RunAsync(() =>
                                {
                                    _ownerAdmissionDeadline = null;
                                    ExecutionsDiscarded?.Invoke(
                                        CaptureExecutions?.Invoke() ?? new HashSet<object>()
                                    );
                                })
                                .ConfigureAwait(false);
                            throw new InvalidOperationException(
                                "The owner lease token became stale."
                            );
                        }
                        result = renewed;
                        return await CompleteOwnerLeaseRenewalAsync(
                                result,
                                leaseOperationStartedAt,
                                owner.Token
                            )
                            .ConfigureAwait(false);
                    }
                }
                catch (ZLinkOwnerLeaseClaimRejectedException)
                {
                    throw;
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    // Caller cancellation is not a claim failure or a timeout.
                    throw;
                }
                catch (Exception exception)
                {
                    if (readingOrRenewing)
                        await _lane.RunAsync(ObserveStoreOutageOnLane).ConfigureAwait(false);
                    if (
                        (
                            exception is not OperationCanceledException
                            || !deadlineToken.IsCancellationRequested
                        )
                        && claimStarted
                        && owner.Token is null
                        && claimFailure is null
                        && rejectedClaim is null
                    )
                    {
                        claimFailure = exception;
                        continue;
                    }
                    if (rejectedClaim is not null)
                        throw new ZLinkOwnerLeaseClaimRejectedException(rejectedClaim);

                    // Fail-static: record the failure and retry on the next tick.
                    // Existing rows stay valid until the lease actually expires.
                    RecordLeaseFailure(
                        exception is OperationCanceledException
                            ? $"Owner lease renewal timeout after {_options.OwnerLeaseRenewTimeout}."
                            : exception.Message
                    );
                    OwnerLeaseRenewalFailed?.Invoke();
                    return false;
                }
            }
        }
    }

    private async ValueTask<bool> CompleteOwnerLeaseRenewalAsync(
        ZLinkOwnerLeaseRenewResult.Renewed result,
        long leaseOperationStartedAt,
        ZLinkLocationOwnerToken? renewingOwner = null
    )
    {
        var admissionLifetime =
            result.LeaseExpiresAt - result.StoreNow - _options.OwnerLeaseFencingMargin;
        if (admissionLifetime <= TimeSpan.Zero)
            throw new InvalidOperationException(
                "The owner lease does not leave a positive admission lifetime."
            );
        await _lane
            .RunAsync(() =>
            {
                _ownerAdmissionDeadline = new ZLinkOwnerAdmissionDeadline(
                    leaseOperationStartedAt,
                    admissionLifetime
                );
                var episode = GetHealthSnapshot().StoreOutage;
                if (episode is not null)
                {
                    LatchStoreOutageOnLane(episode);
                    if (
                        GetHealthSnapshot().StoreOutage!.Discarded
                        || renewingOwner != episode.Owner
                    )
                        return;
                    _storeOutageTimer?.Dispose();
                    _storeOutageTimer = null;
                    UpdateHealth(health => health with { StoreOutage = null });
                }
                if (
                    _lastSuccessfulLease is null
                    || leaseOperationStartedAt
                        >= _lastSuccessfulLease.Value.Deadline.LeaseOperationStartedAt
                )
                    _lastSuccessfulLease = (RequireOwnerTokenOnLane(), _ownerAdmissionDeadline);
            })
            .ConfigureAwait(false);
        UpdateHealth(health =>
            health with
            {
                Healthy = true,
                RenewedAt = result.StoreNow,
                LeaseError = null,
            }
        );

        OwnerLeaseRenewed?.Invoke(
            new ZLinkOwnerLeaseRenewal(result.LeaseExpiresAt, result.StoreNow)
        );

        return true;
    }

    private void ObserveStoreOutageOnLane()
    {
        if (
            _lastSuccessfulLease is not { } lease
            || GetHealthSnapshot().StoreOutage is not null
            || Volatile.Read(ref _disposeState) != 0
        )
            return;
        var lifetime = _options.StoreOutageRetention
            ? _options.OwnerLeaseTtl
                + _options.OwnerLeaseFencingMargin
                + _options.OwnerLeaseFencingMargin
            : lease.Deadline.Lifetime;
        var episode = new ZLinkStoreOutageEpisode(
            _time.GetTimestamp(),
            lease.Deadline.LeaseOperationStartedAt,
            lifetime,
            lease.Token
        )
        {
            Executions = CaptureSteadyExecutionsOnLane(),
        };
        UpdateHealth(health => health with { StoreOutage = episode });
        // Use the host clock's one-shot deadline; application turns and mesh peers do not participate.
        using (ExecutionContext.IsFlowSuppressed() ? default : ExecutionContext.SuppressFlow())
        {
            _storeOutageTimer = _time.CreateTimer(
                state =>
                    _lane
                        .RunAsync(() => LatchStoreOutageOnLane(episode))
                        .AsTask()
                        .ContinueWith(
                            static task => _ = task.Exception,
                            CancellationToken.None,
                            TaskContinuationOptions.OnlyOnFaulted
                                | TaskContinuationOptions.ExecuteSynchronously,
                            TaskScheduler.Default
                        ),
                null,
                Timeout.InfiniteTimeSpan,
                Timeout.InfiniteTimeSpan
            );
        }
        LatchStoreOutageOnLane(episode);
    }

    private void LatchStoreOutageOnLane(ZLinkStoreOutageEpisode episode)
    {
        if (!ReferenceEquals(GetHealthSnapshot().StoreOutage, episode) || episode.Discarded)
            return;
        var remaining = episode.Lifetime - _time.GetElapsedTime(episode.LeaseOperationStartedAt);
        if (remaining > TimeSpan.Zero)
        {
            _storeOutageTimer?.Change(remaining, Timeout.InfiniteTimeSpan);
            return;
        }
        UpdateHealth(health => health with { StoreOutage = episode with { Discarded = true } });
        _storeOutageTimer?.Dispose();
        _storeOutageTimer = null;
        ExecutionsDiscarded?.Invoke(episode.Executions);
        if (episode.Executions.Count != 0)
            UpdateHealth(health =>
                health with
                {
                    StoreOutage = health.StoreOutage! with { Executions = new HashSet<object>() },
                }
            );
    }

    private async ValueTask<ZLinkOwnerLeaseRenewResult.Renewed?> ConfirmClaimAsync(
        string ownerId,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var owner = await ObserveStoreAsync(
                () => _store.ReadOwnerLeaseAsync(ownerId, cancellationToken),
                "read"
            )
            .AsTask()
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        if (owner is ZLinkOwnerLeaseReadResult.Found found)
            return await InstallConfirmedClaimAsync(
                    found.Token,
                    found.LeaseExpiresAt,
                    found.StoreNow
                )
                .ConfigureAwait(false);

        return null;
    }

    private async ValueTask<ZLinkOwnerLeaseRenewResult.Renewed> InstallConfirmedClaimAsync(
        ZLinkLocationOwnerToken token,
        DateTimeOffset leaseExpiresAt,
        DateTimeOffset storeNow
    )
    {
        await _lane.RunAsync(() => _ownerToken = token).ConfigureAwait(false);
        return new ZLinkOwnerLeaseRenewResult.Renewed(leaseExpiresAt, storeNow);
    }

    private async ValueTask ReleaseClaimIfConfirmedAfterCancellationAsync(
        string ownerId,
        CancellationToken cancellationToken,
        Task<ZLinkOwnerLeaseClaimResult>? pendingClaim
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (pendingClaim is not null)
        {
            try
            {
                _ = await pendingClaim.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (Exception exception) when (!cancellationToken.IsCancellationRequested)
            {
                RecordLeaseFailure(exception.Message);
            }
        }
        var owner = await ObserveStoreAsync(
                () => _store.ReadOwnerLeaseAsync(ownerId, cancellationToken),
                "read"
            )
            .AsTask()
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);
        if (owner is ZLinkOwnerLeaseReadResult.Found found)
            _ = await ObserveStoreAsync(
                    () => _store.ReleaseOwnerLeaseAsync(found.Token, cancellationToken),
                    "release"
                )
                .AsTask()
                .WaitAsync(cancellationToken)
                .ConfigureAwait(false);
    }

    private async ValueTask RecordCancellationCleanupFailureAsync(
        Func<ValueTask> cleanup,
        OperationCanceledException cancellation
    )
    {
        try
        {
            await cleanup().ConfigureAwait(false);
        }
        catch (Exception cleanupException)
        {
            RecordLeaseFailure(
                $"Owner lease cancellation cleanup failed: {cleanupException.Message}"
            );
            throw new ZLinkOwnerLeaseCancellationCleanupException(cancellation, cleanupException);
        }
    }

    private async Task HeartbeatLoopAsync(
        CancellationToken cancellationToken,
        long leaseOperationStartedAt
    )
    {
        var intervalTicks = (long)(
            _options.OwnerLeaseRenewInterval.TotalSeconds * _time.TimestampFrequency
        );
        while (!cancellationToken.IsCancellationRequested)
        {
            var scheduledRenew = leaseOperationStartedAt + intervalTicks;
            var remainingTicks = scheduledRenew - _time.GetTimestamp();
            try
            {
                if (remainingTicks > 0)
                    await Task.Delay(
                            TimeSpan.FromSeconds(remainingTicks / (double)_time.TimestampFrequency),
                            _time,
                            cancellationToken
                        )
                        .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }

            foreach (var scope in _metricScopes)
                ZLinkRuntimeMetrics.RecordOwnerLeaseRenewAttempt(
                    _time,
                    scheduledRenew,
                    scope.Key,
                    scope.Value
                );
            leaseOperationStartedAt = _time.GetTimestamp();
            try
            {
                await RenewOwnerLeaseOnceAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (ZLinkOwnerLeaseClaimRejectedException exception)
            {
                RecordLeaseFailure(exception.Message);
                OwnerLeaseRenewalFailed?.Invoke();
            }
        }
    }

    private TaskCompletionSource StartHeartbeatOnLane(long leaseOperationStartedAt)
    {
        var heartbeat = new CancellationTokenSource();
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        _heartbeatCts = heartbeat;
        // Location section 5 anchors the next renewal to the preceding Store operation,
        // including the initial claim; starting this continuation must not extend it.
        _heartbeatLoop = StartHeartbeatLoop(heartbeat.Token, started.Task, leaseOperationStartedAt);
        return started;
    }

    private Task StartHeartbeatLoop(
        CancellationToken cancellationToken,
        Task started,
        long leaseOperationStartedAt
    ) =>
        ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(async () =>
        {
            await started.ConfigureAwait(false);
            await HeartbeatLoopAsync(cancellationToken, leaseOperationStartedAt)
                .ConfigureAwait(false);
        });

    private bool IsOwnerAdmissionOpenOnLane()
    {
        var deadline = Volatile.Read(ref _ownerAdmissionDeadline);
        return deadline is not null
            && _time.GetElapsedTime(deadline.LeaseOperationStartedAt) < deadline.Lifetime;
    }

    private void EnsureOwnerAdmissionOpenOnLane()
    {
        if (!IsOwnerAdmissionOpenOnLane())
            throw new InvalidOperationException("The owner lease admission deadline has expired.");
    }

    private readonly record struct StopState(
        CancellationTokenSource? Heartbeat,
        Task? HeartbeatLoop,
        ZLinkLocationOwnerToken? OwnerToken,
        bool OwnerCleanedForDrain
    );

    private readonly record struct OwnerLeaseState(string Id, ZLinkLocationOwnerToken? Token);

    private async ValueTask<ZLinkLocationWriteResult> GuardStoreWriteAsync(
        Func<ValueTask<ZLinkLocationWriteResult>> write
    )
    {
        try
        {
            var result = await ObserveStoreAsync(write, "compare_exchange").ConfigureAwait(false);
            UpdateHealth(static health => health with { StoreError = null });
            return result;
        }
        catch (Exception exception)
        {
            RecordStoreFailure(exception.Message);
            throw;
        }
    }

    private async ValueTask<ZLinkLocationWriteResult> ExecuteRemoveAsync(
        Func<ValueTask<ZLinkLocationWriteStatus>> remove,
        ZLinkLocationKind kind,
        string canonicalKey
    )
    {
        ZLinkLocationWriteStatus status;
        try
        {
            status = await ObserveStoreAsync(remove, "release").ConfigureAwait(false);
            UpdateHealth(static health => health with { StoreError = null });
        }
        catch (Exception exception)
        {
            RecordStoreFailure(exception.Message);
            throw;
        }

        var result = new ZLinkLocationWriteResult(status, 0, default);
        NotifyIfStale(result, kind, canonicalKey);
        return result;
    }

    private async ValueTask<ZLinkLocationWriteResult> ExecuteLocationWriteAsync(
        Func<ValueTask<ZLinkLocationWriteResult>> write,
        ZLinkLocationKind kind,
        string canonicalKey
    )
    {
        var result = await GuardStoreWriteAsync(write).ConfigureAwait(false);
        NotifyIfStale(result, kind, canonicalKey);
        return result;
    }

    private void NotifyIfStale(
        ZLinkLocationWriteResult result,
        ZLinkLocationKind kind,
        string canonicalKey
    )
    {
        if (result.Status == ZLinkLocationWriteStatus.IgnoredStale)
        {
            OwnershipLost?.Invoke(kind, canonicalKey);
        }
    }

    private static async ValueTask<T> ObserveStoreAsync<T>(
        Func<ValueTask<T>> request,
        string operation
    )
    {
        try
        {
            return await request().ConfigureAwait(false);
        }
        catch (ZLinkFrameworkException exception)
            when (exception.Kind == ZLinkFrameworkErrorKind.Unavailable)
        {
            ZLinkRuntimeMetrics.RecordLocationStoreError(operation);
            throw;
        }
    }

    private void RecordLeaseFailure(string message)
    {
        foreach (var scope in _metricScopes)
            ZLinkRuntimeMetrics.RecordOwnerLeaseRenewFailure(scope.Key, scope.Value);
        UpdateHealth(health => health with { Healthy = false, LeaseError = message });
    }

    private void RecordStoreFailure(string message)
    {
        UpdateHealth(health => health with { StoreError = message });
    }

    private void UpdateHealth(Func<ZLinkLocationRuntimeHealth, ZLinkLocationRuntimeHealth> update)
    {
        while (true)
        {
            var current = Volatile.Read(ref _health);
            var next = update(current);
            if (ReferenceEquals(Interlocked.CompareExchange(ref _health, next, current), current))
                return;
        }
    }
}

internal sealed record ZLinkLocationRuntimeHealth(
    bool Healthy,
    DateTimeOffset? RenewedAt,
    string? LeaseError,
    string? StoreError
)
{
    internal ZLinkStoreOutageEpisode? StoreOutage { get; init; }

    public string? LastError => StoreError ?? LeaseError;
}
