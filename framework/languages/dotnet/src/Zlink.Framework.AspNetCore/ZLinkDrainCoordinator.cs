using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.AspNetCore;

internal enum ZLinkFrameworkLifecycleIntent
{
    Relocate = 0,
    Shutdown = 1
}

internal enum ZLinkDrainState
{
    Serving = 0,
    Draining = 1,
    Drained = 2,
    ForceStopping = 3
}

internal enum ZLinkDrainForceReason
{
    DeadlineExceeded = 0,
    DrainingStatePublishFailed = 1,
    OwnerCleanupFailed = 2,
    TeardownFailed = 3,
    RelocationFailed = 4
}

internal abstract record ZLinkDrainResult
{
    private protected ZLinkDrainResult()
    {
    }
}

internal sealed record Drained : ZLinkDrainResult;

internal sealed record ForceStopped(
    ZLinkDrainForceReason Reason,
    bool HasCommitted = false,
    ulong CommittedUnitCount = 0) : ZLinkDrainResult;

internal sealed record DrainBlocked(
    ZLinkFrameworkRelocationReason Reason) : ZLinkDrainResult;

internal enum ZLinkDrainExecutionDisposition
{
    Completed = 0,
    Blocked = 1,
    ForceStop = 2
}

internal readonly record struct ZLinkDrainExecutionResult
{
    private ZLinkDrainExecutionResult(
        ZLinkDrainExecutionDisposition disposition,
        ZLinkFrameworkRelocationReason? blockedReason,
        ZLinkDrainForceReason? forceReason,
        ulong committedUnitCount)
    {
        Disposition = disposition;
        BlockedReason = blockedReason;
        ForceReason = forceReason;
        CommittedUnitCount = committedUnitCount;
    }

    internal ZLinkDrainExecutionDisposition Disposition { get; }

    internal ZLinkFrameworkRelocationReason? BlockedReason { get; }

    internal ZLinkDrainForceReason? ForceReason { get; }

    internal ulong CommittedUnitCount { get; }

    internal bool HasCommitted => CommittedUnitCount != 0;

    internal static ZLinkDrainExecutionResult Completed(
        ulong committedUnitCount = 0) =>
        new(
            ZLinkDrainExecutionDisposition.Completed,
            null,
            null,
            committedUnitCount);

    internal static ZLinkDrainExecutionResult Blocked(
        ZLinkFrameworkRelocationReason reason,
        ulong committedUnitCount = 0) =>
        new(
            ZLinkDrainExecutionDisposition.Blocked,
            reason,
            null,
            committedUnitCount);

    internal static ZLinkDrainExecutionResult ForceStop(
        ZLinkDrainForceReason reason,
        ulong committedUnitCount = 0) =>
        new(
            ZLinkDrainExecutionDisposition.ForceStop,
            null,
            reason,
            committedUnitCount);
}

internal sealed class ZLinkDrainBlockedException(
    ZLinkFrameworkRelocationReason reason) : Exception(
    $"The Relocate operation was blocked before its first relocation commit: {reason}.")
{
    internal ZLinkFrameworkRelocationReason Reason { get; } = reason;
}

internal interface IZLinkDrainExecutor
{
    void RequestShutdown(TimeSpan deadline)
    {
    }

    ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        TimeSpan deadline,
        CancellationToken deadlineToken);

    ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        CancellationToken deadlineToken) =>
        ExecuteAsync(deadline, deadlineToken);

    async ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached,
        CancellationToken deadlineToken)
    {
        var result = await ExecuteAsync(intent, deadline, deadlineToken)
            .ConfigureAwait(false);
        if (result is null) relocationDetached?.Invoke();
        return result;
    }

    async ValueTask<ZLinkDrainExecutionResult> ExecuteWithProgressAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached,
        CancellationToken deadlineToken)
    {
        var forceReason = await ExecuteAsync(
                intent,
                deadline,
                relocationDetached,
                deadlineToken)
            .ConfigureAwait(false);
        return forceReason is { } reason
            ? ZLinkDrainExecutionResult.ForceStop(reason)
            : ZLinkDrainExecutionResult.Completed();
    }

    ValueTask ForceStopAsync(
        ZLinkDrainForceReason reason,
        CancellationToken cancellationToken);
}

internal sealed class ZLinkDrainForceException(
    ZLinkDrainForceReason reason,
    IReadOnlyList<Exception> failures) : Exception(
    $"Forced drain teardown failed with '{reason}'.",
    failures.Count == 1 ? failures[0] : new AggregateException(failures))
{
    public ZLinkDrainForceReason Reason { get; } = reason;
}

internal sealed class ZLinkDrainCoordinator : IDisposable
{
    // The caller's deadline bounds orderly drain. Once it expires, force
    // teardown still needs a small independent budget to release runtime
    // resources and classify the original timeout as DeadlineExceeded.
    private static readonly TimeSpan MinimumForceStopTeardown =
        TimeSpan.FromSeconds(2);

    internal static readonly TimeSpan DefaultDeadline = TimeSpan.FromSeconds(30);

    private readonly ZLinkDrainAdmissionGate _admission;
    private readonly IZLinkDrainExecutor _executor;
    private readonly Func<bool> _flowCaptureEnabled;
    private readonly ILogger<ZLinkDrainCoordinator>? _logger;
    private readonly object _gate = new();
    private readonly TaskCompletionSource<ZLinkDrainResult> _terminal =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private Task<ZLinkDrainResult>? _operation;
    private Task<ZLinkDrainResult>? _forceStopOperation;

    public ZLinkDrainCoordinator(
        ZLinkDrainAdmissionGate admission,
        IZLinkDrainExecutor executor,
        Func<bool>? flowCaptureEnabled = null,
        ILogger<ZLinkDrainCoordinator>? logger = null)
    {
        _admission = admission;
        _executor = executor;
        _flowCaptureEnabled = flowCaptureEnabled ?? AlwaysDisabled;
        _logger = logger;
    }

    public bool IsReady => !_admission.IsDraining;

    internal void RequestShutdown(TimeSpan deadline)
    {
        if (deadline <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(deadline));
        _admission.BeginDrain(ZLinkDrainOwner.Shutdown);
        _executor.RequestShutdown(deadline);
    }

    public ValueTask<ZLinkDrainResult> DrainAsync(
        CancellationToken cancellationToken = default) =>
        DrainAsync(
            ZLinkFrameworkLifecycleIntent.Shutdown,
            DefaultDeadline,
            cancellationToken: cancellationToken);

    public async ValueTask<ZLinkDrainResult> DrainAsync(
        TimeSpan deadline,
        CancellationToken cancellationToken = default) =>
        await DrainAsync(
                ZLinkFrameworkLifecycleIntent.Shutdown,
                deadline,
                cancellationToken: cancellationToken)
            .ConfigureAwait(false);

    internal async ValueTask<ZLinkDrainResult> DrainAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached = null,
        CancellationToken cancellationToken = default)
    {
        if (deadline <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(
                nameof(deadline),
                deadline,
                "Drain deadline must be greater than zero.");

        Task<ZLinkDrainResult> operation;
        lock (_gate)
        {
            if (_operation is null)
            {
                Action? effectiveRelocationDetached = relocationDetached;
                if (intent == ZLinkFrameworkLifecycleIntent.Shutdown)
                {
                    _admission.BeginDrain(ZLinkDrainOwner.Shutdown);
                }
                else
                {
                    effectiveRelocationDetached = () =>
                    {
                        _admission.BeginDrain(ZLinkDrainOwner.Relocation);
                        relocationDetached?.Invoke();
                    };
                }
                _operation = ExecuteSharedAsync(
                    intent,
                    deadline,
                    effectiveRelocationDetached);
            }
            operation = _operation;
        }

        var result = await operation.WaitAsync(cancellationToken).ConfigureAwait(false);
        if (intent == ZLinkFrameworkLifecycleIntent.Relocate
            && result is Drained)
        {
            lock (_gate)
            {
                if (ReferenceEquals(_operation, operation))
                    _operation = null;
            }
        }
        return result;
    }

    public async ValueTask<ZLinkDrainResult> AwaitDrainedAsync(
        CancellationToken cancellationToken = default)
    {
        return await _terminal.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkDrainResult> ForceStopAsync(
        ZLinkDrainForceReason reason,
        TimeSpan deadline)
    {
        if (deadline <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(deadline));
        _admission.BeginDrain(ZLinkDrainOwner.Shutdown);
        var forced = await ForceStopAsync(
                reason,
                deadline,
                hasCommitted: false,
                committedUnitCount: 0)
            .ConfigureAwait(false);
        _terminal.TrySetResult(forced);
        return forced;
    }

    private async Task<ZLinkDrainResult> ExecuteSharedAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached)
    {
        using var flow = ZLinkFlowContext.Enter(
            null,
            null,
            _flowCaptureEnabled(),
            ZLinkFlowOrigin.Lifecycle);
        ZLinkDrainResult result;
        try
        {
            using var deadlineSource = new CancellationTokenSource(deadline);
            TaskCompletionSource? detached = null;
            Action? observedRelocationDetached = relocationDetached;
            if (intent == ZLinkFrameworkLifecycleIntent.Relocate)
            {
                detached = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                observedRelocationDetached = () =>
                {
                    relocationDetached?.Invoke();
                    detached.TrySetResult();
                };
            }
            else
            {
                await PublishStateAsync(ZLinkDrainState.Draining).ConfigureAwait(false);
            }

            var execution = _executor.ExecuteWithProgressAsync(
                    intent,
                    deadline,
                    observedRelocationDetached,
                    deadlineSource.Token)
                .AsTask();
            if (detached is not null)
            {
                var first = await Task.WhenAny(detached.Task, execution)
                    .ConfigureAwait(false);
                if (ReferenceEquals(first, detached.Task))
                    await PublishStateAsync(ZLinkDrainState.Draining)
                        .ConfigureAwait(false);
            }
            var executionResult = await execution.ConfigureAwait(false);
            result = executionResult.Disposition switch
            {
                ZLinkDrainExecutionDisposition.Completed => new Drained(),
                ZLinkDrainExecutionDisposition.Blocked => new DrainBlocked(
                    executionResult.BlockedReason
                        ?? ZLinkFrameworkRelocationReason.RelocationFailed),
                ZLinkDrainExecutionDisposition.ForceStop =>
                    await ForceStopAsync(
                            executionResult.ForceReason
                                ?? ZLinkDrainForceReason.TeardownFailed,
                            deadline,
                            executionResult.HasCommitted,
                            executionResult.CommittedUnitCount)
                        .ConfigureAwait(false),
                _ => throw new InvalidOperationException(
                    "The drain execution returned an unknown disposition.")
            };
        }
        catch (ZLinkDrainBlockedException blocked)
        {
            result = new DrainBlocked(blocked.Reason);
        }
        catch (OperationCanceledException)
        {
            result = await ForceStopAsync(
                    ZLinkDrainForceReason.DeadlineExceeded,
                    deadline)
                .ConfigureAwait(false);
        }
        catch (Exception error)
        {
            _logger?.LogError(error, "ZLink drain execution failed before terminal teardown.");
            result = await ForceStopAsync(
                    ZLinkDrainForceReason.TeardownFailed,
                    deadline)
                .ConfigureAwait(false);
        }

        if (result is DrainBlocked)
        {
            lock (_gate)
                _operation = null;
            return result;
        }

        if (result is Drained)
        {
            try
            {
                if (intent == ZLinkFrameworkLifecycleIntent.Shutdown)
                    await PublishStateAsync(ZLinkDrainState.Drained).ConfigureAwait(false);
            }
            catch (Exception error)
            {
                _logger?.LogError(error, "ZLink drained terminal event publication failed.");
            }
        }

        if (intent != ZLinkFrameworkLifecycleIntent.Relocate
            || result is not Drained)
        {
            _terminal.TrySetResult(result);
        }
        return result;
    }

    private ValueTask<ZLinkDrainResult> ForceStopAsync(
        ZLinkDrainForceReason reason,
        TimeSpan deadline,
        bool hasCommitted = false,
        ulong committedUnitCount = 0)
    {
        lock (_gate)
            _forceStopOperation ??= ExecuteForceStopAsync(
                reason,
                deadline,
                hasCommitted,
                committedUnitCount);
        return new ValueTask<ZLinkDrainResult>(_forceStopOperation);
    }

    private async Task<ZLinkDrainResult> ExecuteForceStopAsync(
        ZLinkDrainForceReason reason,
        TimeSpan deadline,
        bool hasCommitted,
        ulong committedUnitCount)
    {
        try
        {
            await PublishStateAsync(ZLinkDrainState.ForceStopping).ConfigureAwait(false);
        }
        catch (Exception error)
        {
            _logger?.LogError(error, "ZLink force-stopping terminal event publication failed.");
        }
        // Do not reuse a very short orderly-drain deadline as the complete
        // force-teardown budget. A 100 ms shutdown can already be expired
        // when this path starts; passing that same budget to owner cleanup
        // turns the original deadline into OwnerCleanupFailed/TeardownFailed.
        // The original reason remains authoritative while teardown receives
        // the minimum bounded window required to release its resources.
        var teardownBudget = deadline < MinimumForceStopTeardown
            ? MinimumForceStopTeardown
            : deadline;
        Zlink.Framework.Runtime.Diagnostics.ZLinkFrameworkDebugLog.SpotDiscovery(
            $"force_stop_begin reason={reason} deadline={deadline} "
            + $"budget={teardownBudget}");
        using var teardownBound = new CancellationTokenSource(teardownBudget);
        try
        {
            await _executor.ForceStopAsync(reason, teardownBound.Token)
                .ConfigureAwait(false);
        }
        catch (ZLinkDrainForceException failure)
        {
            reason = failure.Reason;
        }
        catch (OperationCanceledException)
            when (teardownBound.IsCancellationRequested)
        {
            reason = ZLinkDrainForceReason.DeadlineExceeded;
        }
        catch (Exception error)
        {
            _logger?.LogError(error, "ZLink forced drain teardown failed.");
            reason = ZLinkDrainForceReason.TeardownFailed;
        }
        return new ForceStopped(reason, hasCommitted, committedUnitCount);
    }

    private ValueTask PublishStateAsync(ZLinkDrainState state)
    {
        _logger?.LogInformation(
            "ZLink host lifecycle changed. state={State}",
            state);
        return ValueTask.CompletedTask;
    }

    private static bool AlwaysDisabled() => false;

    public void Dispose()
    {
    }
}
