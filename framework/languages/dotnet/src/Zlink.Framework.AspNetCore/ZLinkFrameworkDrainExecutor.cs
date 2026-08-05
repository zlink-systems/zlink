using Microsoft.Extensions.Logging;

namespace Zlink.Framework.AspNetCore;

internal sealed class ZLinkFrameworkDrainExecutor : IZLinkDrainExecutor
{
    private static readonly TimeSpan SchedulerJitterBudget = TimeSpan.FromMilliseconds(100);
    private readonly ZLinkDrainExecutionOperations _operations;
    private readonly ZLinkLocationOptions _locationOptions;
    private readonly ILogger<ZLinkFrameworkDrainExecutor>? _logger;
    private readonly Action _stopMeshMonitoring;
    private readonly CancellationTokenSource _shutdownDeadline = new();
    private int _shutdownRequested;

    public ZLinkFrameworkDrainExecutor(
        ZLinkFrameworkRuntime runtime,
        ZLinkRouteMeshRuntimeService routeMeshRuntime,
        ZLinkLocationOptions locationOptions,
        ZLinkLocationAutoConnectHost? autoConnect,
        ZLinkLocationRuntime? locationRuntime,
        ILogger<ZLinkFrameworkDrainExecutor>? logger = null)
        : this(
            ZLinkDrainExecutionOperations.Create(
                runtime,
                autoConnect,
                locationRuntime),
            locationOptions,
            logger,
            routeMeshRuntime.Stop)
    {
    }

    internal ZLinkFrameworkDrainExecutor(
        ZLinkDrainExecutionOperations operations,
        ZLinkLocationOptions locationOptions,
        ILogger<ZLinkFrameworkDrainExecutor>? logger = null,
        Action? stopMeshMonitoring = null)
    {
        _operations = operations;
        _locationOptions = locationOptions;
        _logger = logger;
        _stopMeshMonitoring = stopMeshMonitoring ?? Noop;
    }

    private static void Noop()
    {
    }

    public void RequestShutdown(TimeSpan deadline)
    {
        if (deadline <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(deadline));
        if (Interlocked.Exchange(ref _shutdownRequested, 1) != 0)
            return;
        Zlink.Framework.Runtime.Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            "admission_sealed site=request_shutdown");
        _operations.SealApplicationAdmissions();
        _shutdownDeadline.CancelAfter(deadline);
    }

    public ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        TimeSpan deadline,
        CancellationToken deadlineToken) =>
        ExecuteAsync(
            ZLinkFrameworkLifecycleIntent.Shutdown,
            deadline,
            deadlineToken);

    public async ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        CancellationToken deadlineToken)
        => await ExecuteAsync(intent, deadline, null, deadlineToken)
            .ConfigureAwait(false);

    public async ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached,
        CancellationToken deadlineToken)
    {
        var result = await ExecuteWithProgressAsync(
                intent,
                deadline,
                relocationDetached,
                deadlineToken)
            .ConfigureAwait(false);
        if (result.Disposition == ZLinkDrainExecutionDisposition.Blocked)
            throw new ZLinkDrainBlockedException(
                result.BlockedReason
                    ?? ZLinkFrameworkRelocationReason.RelocationFailed);
        return result.ForceReason;
    }

    public async ValueTask<ZLinkDrainExecutionResult> ExecuteWithProgressAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached,
        CancellationToken deadlineToken)
    {
        var absoluteDeadline = DateTimeOffset.UtcNow + deadline;
        ulong committedUnitCount = 0;
        var relocationFence = intent == ZLinkFrameworkLifecycleIntent.Relocate
            ? _operations.CaptureRelocationFence?.Invoke()
            : null;
        try
        {
            //  Shutdown 경로는 단계별 흔적이 없어 deadline이 어디서 소진되는지
            //  알 수 없었다. 각 단계의 시작을 남긴다.
            void ShutdownStep(string step) =>
                Zlink.Framework.Runtime.Diagnostics.ZLinkFrameworkDebugLog
                    .SpotDiscovery($"shutdown_step step={step} intent={intent}");
            if (intent == ZLinkFrameworkLifecycleIntent.Shutdown)
            {
                ShutdownStep("seal_admissions");
                Zlink.Framework.Runtime.Diagnostics.ZLinkFrameworkDebugLog
                    .SpotDiscovery("admission_sealed site=shutdown_intent");
                _operations.SealApplicationAdmissions();
            }

            if (intent == ZLinkFrameworkLifecycleIntent.Shutdown)
            {
                ShutdownStep("publish_draining_marker");
                if (!await PublishDrainingMarkerAsync(deadlineToken).ConfigureAwait(false))
                    return Result(ZLinkDrainForceReason.DrainingStatePublishFailed);
                ShutdownStep("publish_serving_weight");
                if (!await PublishServingWeightAsync(deadlineToken).ConfigureAwait(false))
                    return Result(ZLinkDrainForceReason.DrainingStatePublishFailed);
                ShutdownStep("wait_descriptor_propagation");
                await WaitForDescriptorPropagationAsync(deadlineToken).ConfigureAwait(false);
                ShutdownStep("descriptor_propagated");
            }

            if (intent == ZLinkFrameworkLifecycleIntent.Shutdown)
            {
                ShutdownStep("wait_accepted_operations");
                await _operations.WaitForAcceptedOperations().WaitAsync(deadlineToken)
                    .ConfigureAwait(false);
                ShutdownStep("wait_accepted_actor_handoffs");
                await _operations.WaitForAcceptedActorHandoffs(deadlineToken).ConfigureAwait(false);
                ShutdownStep("accepted_drained");
            }

            if (intent == ZLinkFrameworkLifecycleIntent.Relocate)
            {
                //  Spec 28 §567은 이동을 시작한 node를 새 selection과 placement에서
                //  빼도록 요구한다. Shutdown 경로는 serving weight를 내리며 함께
                //  게시하지만 Relocate는 그 경로를 타지 않는다.
                _operations.PublishDrainingToPeers();
                while (true)
                {
                    ThrowIfShutdownRequested();
                    ZLinkRelocationWorkloadDrainResult workloads;
                    try
                    {
                        using var unitCancellation =
                            CancellationTokenSource.CreateLinkedTokenSource(
                                deadlineToken,
                                _shutdownDeadline.Token);
                        workloads = await _operations
                            .DrainRelocationWorkloads(
                                new ZLinkRelocationWorkloadDrainControl(
                                    ShutdownRequested,
                                    unitCancellation.Token,
                                    absoluteDeadline))
                            .ConfigureAwait(false);
                    }
                    catch (OperationCanceledException)
                        when (Volatile.Read(ref _shutdownRequested) != 0)
                    {
                        throw new ZLinkDrainBlockedException(
                            ZLinkFrameworkRelocationReason.ShutdownRequested);
                    }
                    catch (ZLinkAuthorityGenerationExhaustedException)
                    {
                        return committedUnitCount == 0
                            ? await RollBackBlockedRetireAsync(
                                    ZLinkFrameworkRelocationReason
                                        .RelocationFailed,
                                    relocationFence)
                                .ConfigureAwait(false)
                            : Result(ZLinkDrainForceReason.RelocationFailed);
                    }
                    committedUnitCount = checked(
                        committedUnitCount + workloads.CommittedUnitCount);
                    ThrowIfShutdownRequested();
                    if (workloads.TerminalReason is not null)
                        return workloads.CommitKnowledge
                                   == ZLinkRelocationCommitKnowledge.NotCommitted
                               && committedUnitCount == 0
                            ? await RollBackBlockedRetireAsync(
                                    workloads.TerminalReason.Value,
                                    relocationFence)
                                .ConfigureAwait(false)
                            : workloads.CommitKnowledge
                                  != ZLinkRelocationCommitKnowledge.Unknown
                              && workloads.SourceTerminalized
                                ? await RestorePartialRelocationAsync(
                                        relocationFence,
                                        committedUnitCount)
                                    .ConfigureAwait(false)
                                : Result(ZLinkDrainForceReason.TeardownFailed);
                    if (workloads.Completed)
                        break;
                    await Task.Delay(
                            _locationOptions.PollingInterval,
                            deadlineToken)
                        .ConfigureAwait(false);
                }

                Zlink.Framework.Runtime.Diagnostics.ZLinkFrameworkDebugLog
                    .SpotDiscovery("admission_sealed site=relocate_completed");
                _operations.SealApplicationAdmissions();
                relocationDetached?.Invoke();
                return Result(null);
            }

            var actorsDrained = false;
            while (!actorsDrained)
            {
                ShutdownStep("drain_actors");
                var actorDrain = await _operations.DrainActors(
                        absoluteDeadline,
                        deadlineToken)
                    .ConfigureAwait(false);
                committedUnitCount = checked(
                    committedUnitCount + actorDrain.CommittedUnitCount);
                if (actorDrain.TerminalReason is not null)
                    return intent == ZLinkFrameworkLifecycleIntent.Relocate
                           && committedUnitCount == 0
                        ? await RollBackBlockedRetireAsync(
                                actorDrain.TerminalReason.Value,
                                relocationFence)
                            .ConfigureAwait(false)
                        : Result(ZLinkDrainForceReason.RelocationFailed);
                actorsDrained = actorDrain.Completed;
                if (!actorsDrained)
                    await Task.Delay(_locationOptions.PollingInterval, deadlineToken).ConfigureAwait(false);
            }

            var spotsDrained = false;
            while (!spotsDrained)
            {
                try
                {
                    ShutdownStep("drain_spots");
                    var spotDrain = await _operations.DrainSpots(
                            false,
                            intent == ZLinkFrameworkLifecycleIntent.Shutdown,
                            absoluteDeadline,
                            deadlineToken)
                        .ConfigureAwait(false);
                    committedUnitCount = checked(
                        committedUnitCount + spotDrain.CommittedUnitCount);
                    //  Actor 루프와 달리 여기에는 terminal reason 탈출구가 없었다.
                    //  Spot drain이 더 진행할 수 없다고 알려도 루프가 그대로 돌아
                    //  deadline을 소진했고, 호출자는 이유 없이 ForceStopped만 받았다.
                    if (spotDrain.TerminalReason is not null)
                        return intent == ZLinkFrameworkLifecycleIntent.Relocate
                               && committedUnitCount == 0
                            ? await RollBackBlockedRetireAsync(
                                    spotDrain.TerminalReason.Value,
                                    relocationFence)
                                .ConfigureAwait(false)
                            : Result(ZLinkDrainForceReason.RelocationFailed);
                    spotsDrained = spotDrain.Completed;
                }
                catch (ZLinkAuthorityGenerationExhaustedException)
                {
                    return intent == ZLinkFrameworkLifecycleIntent.Relocate
                           && committedUnitCount == 0
                        ? await RollBackBlockedRetireAsync(
                                ZLinkFrameworkRelocationReason.RelocationFailed,
                                relocationFence)
                            .ConfigureAwait(false)
                        : Result(ZLinkDrainForceReason.RelocationFailed);
                }
                if (!spotsDrained)
                    await Task.Delay(_locationOptions.PollingInterval, deadlineToken).ConfigureAwait(false);
            }

            ShutdownStep("drain_stream_sessions");
            var streamSessionsDrained = await _operations
                .DrainStreamSessions(deadlineToken)
                .ConfigureAwait(false);
            if (!streamSessionsDrained)
            {
                // A session reports false both when its terminal close failed
                // and when the drain token expired while waiting for that
                // close. Preserve the cancellation cause at this boundary so
                // the coordinator reports the original deadline instead of a
                // misleading teardown failure.
                var forceReason = deadlineToken.IsCancellationRequested
                    ? ZLinkDrainForceReason.DeadlineExceeded
                    : ZLinkDrainForceReason.TeardownFailed;
                Zlink.Framework.Runtime.Diagnostics.ZLinkFrameworkDebugLog
                    .SpotDiscovery(
                        $"stream_session_drain_failed cancellation_requested="
                        + $"{deadlineToken.IsCancellationRequested} "
                        + $"reason={forceReason}");
                return Result(forceReason);
            }

            ShutdownStep("freeze_owner_writes");
            if (_operations.HasAutoConnect)
                await _operations.FreezeOwnerWrites(deadlineToken).ConfigureAwait(false);
            if (_operations.HasAutoConnect)
                await _operations.StopAutoConnect(deadlineToken).ConfigureAwait(false);
            if (!await CleanupOwnerAsync(deadlineToken).ConfigureAwait(false))
                return Result(ZLinkDrainForceReason.OwnerCleanupFailed);
            _stopMeshMonitoring();
            await _operations.StopRuntime(deadlineToken).ConfigureAwait(false);
            if (_operations.HasLocationRuntime)
                await _operations.StopLocation(deadlineToken).ConfigureAwait(false);
            return Result(null);
        }
        catch (OperationCanceledException) when (
            deadlineToken.IsCancellationRequested
            && intent == ZLinkFrameworkLifecycleIntent.Relocate)
        {
            return committedUnitCount == 0
                ? await RollBackBlockedRetireAsync(
                        ZLinkFrameworkRelocationReason.DeadlineExceeded,
                        relocationFence)
                    .ConfigureAwait(false)
                : Result(ZLinkDrainForceReason.DeadlineExceeded);
        }

        ZLinkDrainExecutionResult Result(ZLinkDrainForceReason? reason) =>
            reason is { } forceReason
                ? ZLinkDrainExecutionResult.ForceStop(
                    forceReason,
                    committedUnitCount)
                : ZLinkDrainExecutionResult.Completed(committedUnitCount);

        void ThrowIfShutdownRequested()
        {
            if (ShutdownRequested())
                throw new ZLinkDrainBlockedException(
                    ZLinkFrameworkRelocationReason.ShutdownRequested);
        }

        bool ShutdownRequested() =>
            Volatile.Read(ref _shutdownRequested) != 0;
    }

    private async ValueTask<ZLinkDrainExecutionResult> RollBackBlockedRetireAsync(
        ZLinkFrameworkRelocationReason reason,
        ZLinkRelocationAdmissionFence? relocationFence)
    {
        using var rollbackBound = new CancellationTokenSource(TimeSpan.FromSeconds(2));
        try
        {
            if (relocationFence is not { } expectedFence
                || _operations.AcquireRelocationRollback is null
                || _operations.ReopenRelocationAdmissions is null)
                return OwnershipLostResult();
            var rollbackLease = _operations.AcquireRelocationRollback(expectedFence);
            if (rollbackLease is null || !rollbackLease.IsCurrent)
                return OwnershipLostResult();
            using (rollbackLease)
            using (var restoreBound = CancellationTokenSource.CreateLinkedTokenSource(
                       rollbackBound.Token,
                       rollbackLease.CancellationToken))
            {
                if (!await _operations.RestoreServing(restoreBound.Token)
                        .ConfigureAwait(false)
                    || !rollbackLease.IsCurrent
                    || !_operations.ReopenRelocationAdmissions(
                        expectedFence,
                        rollbackLease))
                    return OwnershipLostResult();
            }
            throw new ZLinkDrainBlockedException(reason);
        }
        catch (ZLinkDrainBlockedException)
        {
            throw;
        }
        catch (OperationCanceledException) when (ShutdownRequested())
        {
            return ZLinkDrainExecutionResult.Blocked(
                ZLinkFrameworkRelocationReason.ShutdownRequested);
        }
        catch
        {
            return OwnershipLostResult();
        }

        ZLinkDrainExecutionResult OwnershipLostResult() =>
            ShutdownRequested()
                ? ZLinkDrainExecutionResult.Blocked(
                    ZLinkFrameworkRelocationReason.ShutdownRequested)
                : ZLinkDrainExecutionResult.ForceStop(
                    ZLinkDrainForceReason.TeardownFailed);

        bool ShutdownRequested() =>
            Volatile.Read(ref _shutdownRequested) != 0;
    }

    private async ValueTask<ZLinkDrainExecutionResult>
        RestorePartialRelocationAsync(
            ZLinkRelocationAdmissionFence? relocationFence,
            ulong committedUnitCount)
    {
        if (ShutdownRequested())
            return ZLinkDrainExecutionResult.Blocked(
                ZLinkFrameworkRelocationReason.ShutdownRequested,
                committedUnitCount);
        if (relocationFence is not { } expectedFence
            || _operations.AcquireRelocationRollback is null
            || _operations.ReopenRelocationAdmissions is null)
            return ZLinkDrainExecutionResult.ForceStop(
                ZLinkDrainForceReason.TeardownFailed,
                committedUnitCount);

        var lease = _operations.AcquireRelocationRollback(expectedFence);
        if (lease is null || !lease.IsCurrent)
            return ShutdownRequested()
                ? ZLinkDrainExecutionResult.Blocked(
                    ZLinkFrameworkRelocationReason.ShutdownRequested,
                    committedUnitCount)
                : ZLinkDrainExecutionResult.ForceStop(
                    ZLinkDrainForceReason.TeardownFailed,
                    committedUnitCount);

        using (lease)
        using (var terminalBound = new CancellationTokenSource(
                   TimeSpan.FromSeconds(2)))
        using (var restoreBound = CancellationTokenSource.CreateLinkedTokenSource(
                   terminalBound.Token,
                   lease.CancellationToken))
        {
            try
            {
                if (!await _operations.RestoreServing(restoreBound.Token)
                        .ConfigureAwait(false)
                    || !lease.IsCurrent
                    || !_operations.ReopenRelocationAdmissions(
                        expectedFence,
                        lease))
                {
                    return ShutdownRequested()
                        ? ZLinkDrainExecutionResult.Blocked(
                            ZLinkFrameworkRelocationReason.ShutdownRequested,
                            committedUnitCount)
                        : ZLinkDrainExecutionResult.ForceStop(
                            ZLinkDrainForceReason.TeardownFailed,
                            committedUnitCount);
                }
            }
            catch (OperationCanceledException) when (ShutdownRequested())
            {
                return ZLinkDrainExecutionResult.Blocked(
                    ZLinkFrameworkRelocationReason.ShutdownRequested,
                    committedUnitCount);
            }
            catch
            {
                return ZLinkDrainExecutionResult.ForceStop(
                    ZLinkDrainForceReason.TeardownFailed,
                    committedUnitCount);
            }
        }

        return ZLinkDrainExecutionResult.Blocked(
            ZLinkFrameworkRelocationReason.RelocationFailed,
            committedUnitCount);

        bool ShutdownRequested() =>
            Volatile.Read(ref _shutdownRequested) != 0;
    }

    private async ValueTask WaitForDescriptorPropagationAsync(
        CancellationToken cancellationToken)
    {
        var propagationDelay = CalculatePropagationDelay(_locationOptions);
        if (!_operations.HasAutoConnect) return;
        _logger?.LogInformation(
            "ZLink drain propagation bound polling={PollingInterval} storeReadTimeout={StoreReadTimeout} schedulerJitterBudget={SchedulerJitterBudget} total={PropagationBound}",
            _locationOptions.PollingInterval,
            ZLinkLocationStoreRead.Timeout,
            SchedulerJitterBudget,
            propagationDelay);
        await Task.Delay(propagationDelay, cancellationToken).ConfigureAwait(false);
    }

    public async ValueTask ForceStopAsync(
        ZLinkDrainForceReason reason,
        CancellationToken cancellationToken)
    {
        _ = reason;
        // The runtime force-stop owner sends the ServerDrain notification and
        // cancels session work before disposing the component state. Waiting
        // for the same sessions here would consume the entire force budget
        // before that owner can perform the actual bounded teardown.
        var failures = new List<Exception>();
        Capture(_stopMeshMonitoring, failures);
        await CaptureAsync(
                "force_runtime",
                () => _operations.ForceStopRuntime(cancellationToken),
                failures)
            .ConfigureAwait(false);
        if (_operations.HasAutoConnect)
            await CaptureAsync(
                    "stop_auto_connect",
                    () => _operations.StopAutoConnect(cancellationToken),
                    failures)
                .ConfigureAwait(false);
        var ownerCleanupFailed = false;
        if (_operations.HasLocationRuntime)
        {
            using var cleanupBound = CancellationTokenSource
                .CreateLinkedTokenSource(cancellationToken);
            cleanupBound.CancelAfter(TimeSpan.FromSeconds(2));
            try
            {
                await _operations.CleanupOwner(cleanupBound.Token).ConfigureAwait(false);
            }
            catch (Exception error)
            {
                ownerCleanupFailed = true;
                failures.Add(error);
                LogCleanupFailure("cleanup_owner", error);
            }
            await CaptureAsync(
                    "stop_location",
                    () => _operations.StopLocation(cancellationToken),
                    failures)
                .ConfigureAwait(false);
        }
        if (ownerCleanupFailed)
            throw new ZLinkDrainForceException(ZLinkDrainForceReason.OwnerCleanupFailed, failures);
        if (failures.Count == 1) throw failures[0];
        if (failures.Count > 1) throw new AggregateException(failures);

        static void Capture(Action operation, ICollection<Exception> failures)
        {
            try
            {
                operation();
            }
            catch (Exception error)
            {
                failures.Add(error);
            }
        }
    }

    private async ValueTask<bool> PublishDrainingMarkerAsync(CancellationToken cancellationToken)
    {
        if (!_operations.HasAutoConnect) return true;
        while (true)
        {
            try
            {
                if (await _operations.MarkDraining(cancellationToken).ConfigureAwait(false))
                    return true;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return false;
            }
            catch
            {
            }

            try
            {
                await Task.Delay(_locationOptions.PollingInterval, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return false;
            }
        }
    }

    private async ValueTask<bool> CleanupOwnerAsync(CancellationToken cancellationToken)
    {
        if (!_operations.HasLocationRuntime) return true;
        while (true)
        {
            try
            {
                await _operations.CleanupOwner(cancellationToken).ConfigureAwait(false);
                return true;
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return false;
            }
            catch
            {
                try
                {
                    await Task.Delay(_locationOptions.PollingInterval, cancellationToken).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
                {
                    return false;
                }
            }
        }
    }

    internal static TimeSpan CalculatePropagationDelay(ZLinkLocationOptions options) =>
        options.PollingInterval
        + ZLinkLocationStoreRead.Timeout
        + SchedulerJitterBudget;

    private async ValueTask<bool> PublishServingWeightAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            if (await _operations.QuiesceServingChannels(cancellationToken).ConfigureAwait(false))
                return true;
            try
            {
                await Task.Delay(_locationOptions.PollingInterval, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return false;
            }
        }

        return false;
    }

    private static async ValueTask CaptureAsync(
        string stage,
        Func<ValueTask> operation,
        List<Exception> failures)
    {
        try
        {
            await operation().ConfigureAwait(false);
        }
        catch (Exception error)
        {
            failures.Add(error);
            LogCleanupFailure(stage, error);
        }
    }

    private static void LogCleanupFailure(string stage, Exception error)
    {
        Zlink.Framework.Runtime.Diagnostics.ZLinkFrameworkDebugLog
            .SpotDiscovery(
                $"force_stop_cleanup_failed stage={stage} "
                + $"type={error.GetType().Name} "
                + $"message={error.Message.Replace('\r', ' ').Replace('\n', ' ')} "
                + $"detail={error.ToString().Replace('\r', ' ').Replace("\n", " | ")}");
    }
}

internal sealed record ZLinkDrainExecutionOperations(
    bool HasAutoConnect,
    bool HasLocationRuntime,
    Func<CancellationToken, ValueTask<bool>> QuiesceServingChannels,
    Func<CancellationToken, ValueTask<bool>> MarkDraining,
    Func<CancellationToken, ValueTask<bool>> RestoreServing,
    Action SealApplicationAdmissions,
    Action PublishDrainingToPeers,
    Func<Task> WaitForAcceptedOperations,
    Func<CancellationToken, Task> WaitForAcceptedActorHandoffs,
    Func<DateTimeOffset, CancellationToken, ValueTask<ZLinkActorDrainResult>> DrainActors,
    Func<bool, bool, DateTimeOffset, CancellationToken,
        ValueTask<ZLinkSpotDrainResult>> DrainSpots,
    Func<ZLinkRelocationWorkloadDrainControl,
        ValueTask<ZLinkRelocationWorkloadDrainResult>>
        DrainRelocationWorkloads,
    Func<CancellationToken, ValueTask<bool>> DrainStreamSessions,
    Func<CancellationToken, ValueTask> FreezeOwnerWrites,
    Func<CancellationToken, ValueTask> CleanupOwner,
    Func<ZLinkDrainRemainderCounts> GetRemainderCounts,
    Func<CancellationToken, ValueTask> StopRuntime,
    Func<CancellationToken, ValueTask> ForceStopRuntime,
    Func<CancellationToken, ValueTask> StopAutoConnect,
    Func<CancellationToken, ValueTask> StopLocation,
    Func<ZLinkRelocationAdmissionFence?>? CaptureRelocationFence = null,
    Func<ZLinkRelocationAdmissionFence, ZLinkRelocationRollbackLease?>?
        AcquireRelocationRollback = null,
    Func<ZLinkRelocationAdmissionFence, ZLinkRelocationRollbackLease, bool>?
        ReopenRelocationAdmissions = null)
{
    internal static ZLinkDrainExecutionOperations Create(
        ZLinkFrameworkRuntime runtime,
        ZLinkLocationAutoConnectHost? autoConnect,
        ZLinkLocationRuntime? locationRuntime) => new(
        autoConnect is not null,
        locationRuntime is not null,
        cancellationToken => runtime.QuiesceServingChannelsForDrainAsync(autoConnect, cancellationToken),
        autoConnect is null
            ? static _ => ValueTask.FromResult(true)
            : autoConnect.MarkDrainingAsync,
        autoConnect is null
            ? static _ => ValueTask.FromResult(true)
            : autoConnect.MarkServingAsync,
        runtime.SealApplicationAdmissionsForDrain,
        runtime.PublishDrainingToPeers,
        runtime.WaitForAcceptedOperationsForDrainAsync,
        runtime.WaitForAcceptedActorHandoffsAsync,
        runtime.DrainActorsAsync,
        runtime.TryDrainSpotsAsync,
        runtime.DrainRelocationWorkloadsAsync,
        runtime.DrainStreamSessionsAsync,
        autoConnect is null
            ? static _ => ValueTask.CompletedTask
            : autoConnect.FreezeOwnerWritesAsync,
        locationRuntime is null
            ? static _ => ValueTask.CompletedTask
            : locationRuntime.CleanupOwnerForDrainAsync,
        runtime.GetDrainRemainderCounts,
        runtime.StopAsync,
        runtime.ForceStopAsync,
        autoConnect is null
            ? static _ => ValueTask.CompletedTask
            : autoConnect.StopAsync,
        locationRuntime is null
            ? static _ => ValueTask.CompletedTask
            : locationRuntime.StopAsync,
        runtime.CaptureRelocationAdmissionFence,
        runtime.TryAcquireRelocationRollbackLease,
        runtime.TryReopenRetireAdmissionsAfterRollback);
}
