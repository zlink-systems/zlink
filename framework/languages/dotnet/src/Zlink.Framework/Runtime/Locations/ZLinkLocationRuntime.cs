using Zlink.Framework.Internal.Locations;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Locations;

internal readonly record struct ZLinkOwnerLeaseRenewal(
    DateTimeOffset LeaseExpiresAt,
    DateTimeOffset StoreNow);

internal sealed record ZLinkOwnerAdmissionDeadline(
    long LeaseOperationStartedAt,
    TimeSpan Lifetime);

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
    private readonly ZLinkObservedLocationGenerations? _observed;
    private readonly TimeProvider _time;
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
    private bool _started;
    private bool _ownerCleanedForDrain;
    private int _disposeState;
    private Task? _disposeTask;

    internal ZLinkLocationRuntime(
        ZLinkLocationOptions options,
        IZLinkLocationRepository store,
        TimeProvider? timeProvider = null,
        ZLinkObservedLocationGenerations? observed = null,
        IReadOnlyList<KeyValuePair<string, string>>? metricScopes = null)
    {
        _options = options;
        _store = store;
        _observed = observed;
        _time = timeProvider ?? TimeProvider.System;
        _metricScopes = metricScopes ?? [];
    }

    /// <summary>Stable owner id for the current runtime generation. Each
    /// successful restart attempt uses a fresh id so rows left by an older
    /// generation cannot become live again.</summary>
    internal string OwnerId => AwaitStateLane(_lane.RunAsync(() => _ownerId));

    internal ZLinkLocationOwnerToken OwnerToken
        => AwaitStateLane(_lane.RunAsync(RequireOwnerTokenOnLane));

    internal ZLinkLocationOwnerToken AdmissionOwnerToken
    {
        get
        {
            return AwaitStateLane(_lane.RunAsync(() =>
            {
                EnsureOwnerAdmissionOpenOnLane();
                return RequireOwnerTokenOnLane();
            }));
        }
    }

    internal bool IsOwnerAdmissionOpen
    {
        get
        {
            return AwaitStateLane(_lane.RunAsync(IsOwnerAdmissionOpenOnLane));
        }
    }

    internal void EnsureOwnerAdmissionOpen()
    {
        AwaitStateLane(_lane.RunAsync(EnsureOwnerAdmissionOpenOnLane));
    }

    internal IZLinkLocationRepository Store => _store;

    internal string? LastError => Volatile.Read(ref _health).LastError;

    internal ZLinkLocationRuntimeHealth GetHealthSnapshot() => Volatile.Read(ref _health);

    /// <summary>Raised when a write for a row this owner believed it owned
    /// came back IgnoredStale. The argument is the canonical location key.</summary>
    internal event Action<ZLinkLocationKind, string>? OwnershipLost;

    internal void NotifyAuthorityOwnershipLost(
        ZLinkLocationKind kind,
        ZLinkAuthorityKey key)
    {
        OwnershipLost?.Invoke(kind, key.Value);
    }

    internal event Action<ZLinkOwnerLeaseRenewal>? OwnerLeaseRenewed;

    internal event Action? OwnerLeaseRenewalFailed;

    internal async ValueTask StartAsync(
        RoutingId nodeRid,
        CancellationToken cancellationToken = default)
    {
        ThrowIfDisposingOrDisposed();
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var start = await _lane.RunAsync(() =>
            {
                ThrowIfDisposingOrDisposed();
                if (_started) return false;

                _nodeRid = nodeRid;
                _ownerId = Guid.NewGuid().ToString("n");
                _ownerToken = null;
                _ownerAdmissionDeadline = null;
                _ownerCleanedForDrain = false;
                return true;
            }).ConfigureAwait(false);
            if (!start) return;

            // Register liveness before any row write so readers joining rows
            // against the lease never see this owner's rows as stale on start.
            if (!await RenewOwnerLeaseOnceAsync(cancellationToken).ConfigureAwait(false))
                throw new InvalidOperationException(
                    $"The location runtime could not establish its owner lease: {LastError ?? "unknown store failure"}");

            var heartbeatStarted = await _lane.RunAsync(() =>
            {
                _started = true;
                return StartHeartbeatOnLane();
            }).ConfigureAwait(false);
            heartbeatStarted.TrySetResult();
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask StopAsync(CancellationToken cancellationToken = default)
    {
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var stop = await _lane.RunAsync(() =>
            {
                if (!_started) return (StopState?)null;

                _started = false;
                var heartbeat = _heartbeatCts;
                var heartbeatLoop = _heartbeatLoop;
                _heartbeatCts = null;
                _heartbeatLoop = null;
                return new StopState(
                    heartbeat,
                    heartbeatLoop,
                    _ownerToken,
                    _ownerCleanedForDrain);
            }).ConfigureAwait(false);
            if (stop is null) return;
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
                catch (OperationCanceledException)
                {
                }
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
                        await _store.RemoveAllByOwnerAsync(
                                token,
                                cancellationToken)
                            .ConfigureAwait(false);
                        _ = await _store.ReleaseOwnerLeaseAsync(
                                token,
                                cancellationToken)
                            .ConfigureAwait(false);
                    }
                    await _lane.RunAsync(() =>
                    {
                        _ownerToken = null;
                        _ownerAdmissionDeadline = null;
                    }).ConfigureAwait(false);
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
                RecordStoreFailure(exception.Message, "release");
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
            var cleanup = await _lane.RunAsync(() =>
            {
                if (!_started || _ownerCleanedForDrain)
                    return (ZLinkLocationOwnerToken?)null;
                return RequireOwnerTokenOnLane();
            }).ConfigureAwait(false);
            if (cleanup is null) return;

            // Keep the owner lease valid while rows are removed. If row
            // cleanup fails, the heartbeat continues and the next retry sees
            // the same live owner rather than stale state.
            await _store.RemoveAllByOwnerAsync(
                    cleanup.Value,
                    cancellationToken)
                .ConfigureAwait(false);

            var heartbeatState = await _lane.RunAsync(() =>
            {
                var heartbeat = _heartbeatCts;
                var heartbeatLoop = _heartbeatLoop;
                _heartbeatCts = null;
                _heartbeatLoop = null;
                return (heartbeat, heartbeatLoop);
            }).ConfigureAwait(false);
            var heartbeat = heartbeatState.heartbeat;
            var heartbeatLoop = heartbeatState.heartbeatLoop;
            if (heartbeat is not null)
                await heartbeat.CancelAsync().ConfigureAwait(false);
            if (heartbeatLoop is not null)
                try
                {
                    await heartbeatLoop.ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                }
            heartbeat?.Dispose();

            try
            {
                if (cleanup.Value is { } token)
                    _ = await _store.ReleaseOwnerLeaseAsync(
                            token,
                            cancellationToken)
                        .ConfigureAwait(false);
                await _lane.RunAsync(() =>
                {
                    _ownerToken = null;
                    _ownerAdmissionDeadline = null;
                }).ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                RecordStoreFailure(exception.Message, "release");
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
        CancellationToken cancellationToken)
    {
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var token = await _lane.RunAsync(() =>
            {
                if (!_started || _ownerCleanedForDrain) return null;
                return _ownerToken;
            }).ConfigureAwait(false);
            if (token is null)
                return;
            _ = await _store.RemoveAllByOwnerAsync(
                    token.Value,
                    cancellationToken)
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
            "The location runtime has not claimed its owner lease.");

    private void StartHeartbeatAfterCleanupFailure()
    {
        var heartbeatStarted = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_started || _heartbeatCts is not null)
                return (TaskCompletionSource?)null;
            return StartHeartbeatOnLane();
        }));
        heartbeatStarted?.TrySetResult();
    }

    public ValueTask DisposeAsync()
    {
        return new ValueTask(AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposeTask is not null) return _disposeTask;
            Volatile.Write(ref _disposeState, 1);
            _disposeTask = StartDisposeCore();
            return _disposeTask;
        })));
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
        CancellationToken cancellationToken = default)
    {
        var ownerToken = AdmissionOwnerToken;
        var stamped = descriptor with
        {
            OwnerId = ownerToken.OwnerId,
            LeaseGeneration = ownerToken.LeaseGeneration,
            // The canonical opaque descriptor stores the timestamp of this
            // write. Stamp the payload before the provider persists it;
            // updating only the local observation from result.UpdatedAt
            // leaves DateTimeOffset's default value in Redis.
            UpdatedAt = _time.GetUtcNow()
        };
        var result = await ExecuteLocationWriteAsync(
                () => _store.UpdateMeshNodeAsync(stamped, intent, cancellationToken),
                ZLinkLocationKind.MeshNode,
                ZLinkLocationKeyCodec.EncodeMeshNodeKey(
                    new ZLinkMeshNodeDescriptorKey(descriptor.MeshName, descriptor.Rid)))
            .ConfigureAwait(false);
        if (result.Status == ZLinkLocationWriteStatus.Stored)
            _observed?.ObserveDescriptor(stamped with { UpdatedAt = result.UpdatedAt });
        return result;
    }

    internal async ValueTask<ZLinkLocationWriteResult> RemoveDescriptorAsync(
        ZLinkMeshNodeDescriptorKey key,
        CancellationToken cancellationToken = default)
    {
        return await ExecuteRemoveAsync(
                () => _store.RemoveMeshNodeAsync(
                    key,
                    OwnerToken,
                    cancellationToken),
                ZLinkLocationKind.MeshNode,
                ZLinkLocationKeyCodec.EncodeMeshNodeKey(key))
            .ConfigureAwait(false);
    }

    internal async ValueTask RemoveDescriptorForShutdownAsync(
        ZLinkMeshNodeDescriptorKey key,
        CancellationToken cancellationToken = default)
    {
        await _lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            // Drain cleanup removes every row owned by this runtime before it
            // releases the owner lease. Auto-connect teardown still needs to
            // disconnect sockets, but its per-descriptor delete is then
            // already complete and must not require the released lease again.
            var token = await _lane.RunAsync(() =>
            {
                if (_ownerCleanedForDrain)
                    return (ZLinkLocationOwnerToken?)null;
                return RequireOwnerTokenOnLane();
            }).ConfigureAwait(false);
            if (token is null)
                return;

            _ = await ExecuteRemoveAsync(
                    () => _store.RemoveMeshNodeAsync(
                        key,
                        token.Value,
                        cancellationToken),
                    ZLinkLocationKind.MeshNode,
                    ZLinkLocationKeyCodec.EncodeMeshNodeKey(key))
                .ConfigureAwait(false);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    internal async ValueTask<bool> RenewOwnerLeaseOnceAsync(
        CancellationToken cancellationToken = default)
    {
        var leaseOperationStartedAt = _time.GetTimestamp();
        try
        {
            using var deadline = new CancellationTokenSource(_options.OwnerLeaseRenewTimeout);
            using var operation = CancellationTokenSource.CreateLinkedTokenSource(
                cancellationToken,
                deadline.Token);
            ZLinkOwnerLeaseRenewResult.Renewed result;
            var owner = await _lane.RunAsync(
                () => new OwnerLeaseState(_ownerId, _ownerToken)).ConfigureAwait(false);
            if (owner.Token is null)
            {
                var claim = await _store.ClaimOwnerLeaseAsync(
                        owner.Id,
                        _options.OwnerLeaseTtl,
                        operation.Token)
                    .AsTask()
                    .WaitAsync(deadline.Token)
                    .ConfigureAwait(false);
                if (claim is not ZLinkOwnerLeaseClaimResult.Claimed claimed)
                    throw new InvalidOperationException(
                        $"Owner lease claim failed with '{claim.GetType().Name}'.");
                await _lane.RunAsync(() => _ownerToken = claimed.Token).ConfigureAwait(false);
                result = new ZLinkOwnerLeaseRenewResult.Renewed(
                    claimed.LeaseExpiresAt,
                    claimed.StoreNow);
            }
            else
            {
                var renewal = await _store.RenewOwnerLeaseAsync(
                        owner.Token.Value,
                        _options.OwnerLeaseTtl,
                        operation.Token)
                    .AsTask()
                    .WaitAsync(deadline.Token)
                    .ConfigureAwait(false);
                if (renewal is not ZLinkOwnerLeaseRenewResult.Renewed renewed)
                    throw new InvalidOperationException(
                        "The owner lease token became stale.");
                result = renewed;
            }
            var admissionLifetime = result.LeaseExpiresAt
                                    - result.StoreNow
                                    - _options.OwnerLeaseFencingMargin;
            if (admissionLifetime <= TimeSpan.Zero)
                throw new InvalidOperationException(
                    "The owner lease does not leave a positive admission lifetime.");
            await _lane.RunAsync(() =>
                _ownerAdmissionDeadline = new ZLinkOwnerAdmissionDeadline(
                    leaseOperationStartedAt,
                    admissionLifetime)).ConfigureAwait(false);
            UpdateHealth(
                health => health with
                {
                    Healthy = true,
                    RenewedAt = result.StoreNow,
                    LeaseError = null
                });

            OwnerLeaseRenewed?.Invoke(
                new ZLinkOwnerLeaseRenewal(
                    result.LeaseExpiresAt,
                    result.StoreNow));

            return true;
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            RecordLeaseFailure(
                $"Owner lease renewal timeout after {_options.OwnerLeaseRenewTimeout}.");
            OwnerLeaseRenewalFailed?.Invoke();
            return false;
        }
        catch (Exception exception)
        {
            // Fail-static: record the failure and retry on the next tick.
            // Existing rows stay valid until the lease actually expires.
            RecordLeaseFailure(exception.Message);
            OwnerLeaseRenewalFailed?.Invoke();
            return false;
        }
    }

    private async Task HeartbeatLoopAsync(CancellationToken cancellationToken)
    {
        var intervalTicks = (long)(
            _options.OwnerLeaseRenewInterval.TotalSeconds
            * _time.TimestampFrequency);
        var scheduledRenew = _time.GetTimestamp() + intervalTicks;
        while (!cancellationToken.IsCancellationRequested)
        {
            var remainingTicks = scheduledRenew - _time.GetTimestamp();
            try
            {
                if (remainingTicks > 0)
                    await Task.Delay(
                            TimeSpan.FromSeconds(remainingTicks / (double)_time.TimestampFrequency),
                            _time,
                            cancellationToken)
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
                    scope.Value);
            await RenewOwnerLeaseOnceAsync(cancellationToken).ConfigureAwait(false);
            scheduledRenew += intervalTicks;
        }
    }

    private TaskCompletionSource StartHeartbeatOnLane()
    {
        var heartbeat = new CancellationTokenSource();
        var started = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _heartbeatCts = heartbeat;
        _heartbeatLoop = StartHeartbeatLoop(heartbeat.Token, started.Task);
        return started;
    }

    private Task StartHeartbeatLoop(CancellationToken cancellationToken, Task started)
    {
        if (ExecutionContext.IsFlowSuppressed())
            return Task.Run(
                async () =>
                {
                    await started.ConfigureAwait(false);
                    await HeartbeatLoopAsync(cancellationToken).ConfigureAwait(false);
                },
                CancellationToken.None);

        using (ExecutionContext.SuppressFlow())
            return Task.Run(
                async () =>
                {
                    await started.ConfigureAwait(false);
                    await HeartbeatLoopAsync(cancellationToken).ConfigureAwait(false);
                },
                CancellationToken.None);
    }

    private bool IsOwnerAdmissionOpenOnLane()
    {
        var deadline = _ownerAdmissionDeadline;
        return deadline is not null
               && _time.GetElapsedTime(deadline.LeaseOperationStartedAt)
               < deadline.Lifetime;
    }

    private void EnsureOwnerAdmissionOpenOnLane()
    {
        if (!IsOwnerAdmissionOpenOnLane())
            throw new InvalidOperationException(
                "The owner lease admission deadline has expired.");
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private readonly record struct StopState(
        CancellationTokenSource? Heartbeat,
        Task? HeartbeatLoop,
        ZLinkLocationOwnerToken? OwnerToken,
        bool OwnerCleanedForDrain);

    private readonly record struct OwnerLeaseState(
        string Id,
        ZLinkLocationOwnerToken? Token);

    private async ValueTask<ZLinkLocationWriteResult> GuardStoreWriteAsync(
        Func<ValueTask<ZLinkLocationWriteResult>> write)
    {
        try
        {
            var result = await write().ConfigureAwait(false);
            UpdateHealth(static health => health with { StoreError = null });
            return result;
        }
        catch (Exception exception)
        {
            RecordStoreFailure(exception.Message, "compare_exchange");
            throw;
        }
    }

    private async ValueTask<ZLinkLocationWriteResult> ExecuteRemoveAsync(
        Func<ValueTask<ZLinkLocationWriteStatus>> remove,
        ZLinkLocationKind kind,
        string canonicalKey)
    {
        ZLinkLocationWriteStatus status;
        try
        {
            status = await remove().ConfigureAwait(false);
            UpdateHealth(static health => health with { StoreError = null });
        }
        catch (Exception exception)
        {
            RecordStoreFailure(exception.Message, "compare_exchange");
            throw;
        }

        var result = new ZLinkLocationWriteResult(status, 0, default);
        NotifyIfStale(result, kind, canonicalKey);
        return result;
    }

    private async ValueTask<ZLinkLocationWriteResult> ExecuteLocationWriteAsync(
        Func<ValueTask<ZLinkLocationWriteResult>> write,
        ZLinkLocationKind kind,
        string canonicalKey)
    {
        var result = await GuardStoreWriteAsync(write).ConfigureAwait(false);
        NotifyIfStale(result, kind, canonicalKey);
        return result;
    }

    private void NotifyIfStale(
        ZLinkLocationWriteResult result,
        ZLinkLocationKind kind,
        string canonicalKey)
    {
        if (result.Status == ZLinkLocationWriteStatus.IgnoredStale)
        {
            OwnershipLost?.Invoke(kind, canonicalKey);
        }
    }

    private void RecordLeaseFailure(string message)
    {
        foreach (var scope in _metricScopes)
            ZLinkRuntimeMetrics.RecordOwnerLeaseRenewFailure(scope.Key, scope.Value);
        ZLinkRuntimeMetrics.RecordLocationStoreError("lease_renew");
        UpdateHealth(health => health with { Healthy = false, LeaseError = message });
    }

    private void RecordStoreFailure(string message, string operation)
    {
        ZLinkRuntimeMetrics.RecordLocationStoreError(operation);
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
    string? StoreError)
{
    public string? LastError => StoreError ?? LeaseError;
}
