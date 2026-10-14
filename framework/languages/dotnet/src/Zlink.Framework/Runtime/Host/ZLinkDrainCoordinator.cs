using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.Runtime.Host;

internal enum ZLinkFrameworkLifecycleIntent
{
    Relocate = 0,
    Shutdown = 1,
}

internal enum ZLinkDrainState
{
    Serving = 0,
    Draining = 1,
    Drained = 2,
    ForceStopping = 3,
}

internal enum ZLinkDrainForceReason
{
    DeadlineExceeded = 0,
    DrainingStatePublishFailed = 1,
    OwnerCleanupFailed = 2,
    TeardownFailed = 3,
    RelocationFailed = 4,
}

internal abstract record ZLinkDrainResult
{
    private protected ZLinkDrainResult() { }
}

internal sealed record Drained : ZLinkDrainResult;

internal sealed record ForceStopped(
    ZLinkDrainForceReason Reason,
    bool HasCommitted = false,
    ulong CommittedUnitCount = 0
) : ZLinkDrainResult;

internal sealed record DrainBlocked(ZLinkFrameworkRelocationReason Reason) : ZLinkDrainResult;

internal readonly record struct ZLinkUnfinishedOperation(string Name, string Owner);

internal enum ZLinkDrainExecutionDisposition
{
    Completed = 0,
    Blocked = 1,
    ForceStop = 2,
}

internal readonly record struct ZLinkDrainExecutionResult
{
    private ZLinkDrainExecutionResult(
        ZLinkDrainExecutionDisposition disposition,
        ZLinkFrameworkRelocationReason? blockedReason,
        ZLinkDrainForceReason? forceReason,
        ulong committedUnitCount
    )
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

    internal static ZLinkDrainExecutionResult Completed(ulong committedUnitCount = 0) =>
        new(ZLinkDrainExecutionDisposition.Completed, null, null, committedUnitCount);

    internal static ZLinkDrainExecutionResult Blocked(
        ZLinkFrameworkRelocationReason reason,
        ulong committedUnitCount = 0
    ) => new(ZLinkDrainExecutionDisposition.Blocked, reason, null, committedUnitCount);

    internal static ZLinkDrainExecutionResult ForceStop(
        ZLinkDrainForceReason reason,
        ulong committedUnitCount = 0
    ) => new(ZLinkDrainExecutionDisposition.ForceStop, null, reason, committedUnitCount);
}

internal sealed class ZLinkDrainBlockedException(ZLinkFrameworkRelocationReason reason)
    : Exception($"The Relocate operation was blocked before its first relocation commit: {reason}.")
{
    internal ZLinkFrameworkRelocationReason Reason { get; } = reason;
}

internal interface IZLinkDrainExecutor
{
    TimeSpan OwnerLeaseRenewTimeout => new ZLinkLocationOptions().OwnerLeaseRenewTimeout;
    CancellationToken ShutdownCancellationToken => CancellationToken.None;

    void RequestShutdown(TimeSpan deadline) { }

    ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        TimeSpan deadline,
        CancellationToken deadlineToken
    );

    ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        CancellationToken deadlineToken
    ) => ExecuteAsync(deadline, deadlineToken);

    async ValueTask<ZLinkDrainForceReason?> ExecuteAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached,
        CancellationToken deadlineToken
    )
    {
        var result = await ExecuteAsync(intent, deadline, deadlineToken).ConfigureAwait(false);
        if (result is null)
            relocationDetached?.Invoke();
        return result;
    }

    async ValueTask<ZLinkDrainExecutionResult> ExecuteWithProgressAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached,
        CancellationToken deadlineToken
    )
    {
        var forceReason = await ExecuteAsync(intent, deadline, relocationDetached, deadlineToken)
            .ConfigureAwait(false);
        return forceReason is { } reason
            ? ZLinkDrainExecutionResult.ForceStop(reason)
            : ZLinkDrainExecutionResult.Completed();
    }

    ValueTask ForceStopAsync(ZLinkDrainForceReason reason, CancellationToken cancellationToken);
}

internal sealed class ZLinkDrainForceException(
    ZLinkDrainForceReason reason,
    IReadOnlyList<Exception> failures
)
    : Exception(
        $"Forced drain teardown failed with '{reason}'.",
        failures.Count == 1 ? failures[0] : new AggregateException(failures)
    )
{
    public ZLinkDrainForceReason Reason { get; } = reason;
}

internal sealed class ZLinkDrainCoordinator : IDisposable
{
    internal static readonly TimeSpan DefaultDeadline = TimeSpan.FromSeconds(30);

    private readonly ZLinkDrainAdmissionGate _admission;
    private readonly IZLinkDrainExecutor _executor;
    private readonly Func<bool> _flowCaptureEnabled;
    private readonly Func<ValueTask<IReadOnlyList<ZLinkUnfinishedOperation>>> _snapshotUnfinished;
    private readonly Action<string>? _traceUnfinished;
    private readonly ILogger<ZLinkDrainCoordinator>? _logger;
    private readonly ZLinkStateLane _lane = new();
    private readonly TaskCompletionSource<ZLinkDrainResult> _terminal = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );
    private Task<ZLinkDrainResult>? _operation;
    private Task<ZLinkDrainResult>? _forceStopOperation;
    private DateTimeOffset? _shutdownEnd;
    internal DateTimeOffset? TeardownEnd { get; private set; }

    public ZLinkDrainCoordinator(
        ZLinkDrainAdmissionGate admission,
        IZLinkDrainExecutor executor,
        Func<bool>? flowCaptureEnabled = null,
        ILogger<ZLinkDrainCoordinator>? logger = null,
        Func<ValueTask<IReadOnlyList<ZLinkUnfinishedOperation>>>? snapshotUnfinished = null,
        Action<string>? traceUnfinished = null
    )
    {
        _admission = admission;
        _executor = executor;
        _flowCaptureEnabled = flowCaptureEnabled ?? AlwaysDisabled;
        _logger = logger;
        _snapshotUnfinished =
            snapshotUnfinished
            ?? (static () => ValueTask.FromResult<IReadOnlyList<ZLinkUnfinishedOperation>>([]));
        _traceUnfinished = traceUnfinished;
    }

    internal void RequestShutdown(TimeSpan deadline)
    {
        if (deadline <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(deadline));
        _admission.BeginDrain(ZLinkDrainOwner.Shutdown);
        _shutdownEnd ??= DateTimeOffset.UtcNow + deadline;
        EstablishTeardownEnd(_shutdownEnd.Value);
        _executor.RequestShutdown(deadline);
    }

    internal CancellationToken ShutdownCancellationToken => _executor.ShutdownCancellationToken;

    public ValueTask<ZLinkDrainResult> DrainAsync(CancellationToken cancellationToken = default) =>
        DrainAsync(
            ZLinkFrameworkLifecycleIntent.Shutdown,
            DefaultDeadline,
            cancellationToken: cancellationToken
        );

    public async ValueTask<ZLinkDrainResult> DrainAsync(
        TimeSpan deadline,
        CancellationToken cancellationToken = default
    ) =>
        await DrainAsync(
                ZLinkFrameworkLifecycleIntent.Shutdown,
                deadline,
                cancellationToken: cancellationToken
            )
            .ConfigureAwait(false);

    internal async ValueTask<ZLinkDrainResult> DrainAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached = null,
        CancellationToken cancellationToken = default
    )
    {
        if (deadline <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(
                nameof(deadline),
                deadline,
                "Drain deadline must be greater than zero."
            );

        var preparation = await _lane
            .RunAsync(() => PrepareOperation(intent, deadline, relocationDetached))
            .ConfigureAwait(false);
        preparation.Start?.Invoke();
        var operation = preparation.Operation;

        var result = await operation.WaitAsync(cancellationToken).ConfigureAwait(false);
        if (intent == ZLinkFrameworkLifecycleIntent.Relocate && result is Drained)
        {
            await _lane
                .RunAsync(() =>
                {
                    if (ReferenceEquals(_operation, operation))
                        _operation = null;
                })
                .ConfigureAwait(false);
        }
        return result;
    }

    private DrainOperationPreparation PrepareOperation(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached
    )
    {
        if (_operation is not null)
            return new DrainOperationPreparation(_operation, null);

        var completion = new TaskCompletionSource<ZLinkDrainResult>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        _operation = completion.Task;
        return new DrainOperationPreparation(
            _operation,
            () => StartSharedOperation(intent, deadline, relocationDetached, completion)
        );
    }

    private void StartSharedOperation(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached,
        TaskCompletionSource<ZLinkDrainResult> completion
    )
    {
        Action? effectiveRelocationDetached = relocationDetached;
        if (intent == ZLinkFrameworkLifecycleIntent.Shutdown)
        {
            _shutdownEnd ??= DateTimeOffset.UtcNow + deadline;
            EstablishTeardownEnd(_shutdownEnd.Value);
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

        using (ExecutionContext.SuppressFlow())
            _ = Task.Run(async () =>
            {
                try
                {
                    completion.TrySetResult(
                        await ExecuteSharedAsync(intent, deadline, effectiveRelocationDetached)
                            .ConfigureAwait(false)
                    );
                }
                catch (Exception exception)
                {
                    completion.TrySetException(exception);
                }
            });
    }

    public async ValueTask<ZLinkDrainResult> AwaitDrainedAsync(
        CancellationToken cancellationToken = default
    )
    {
        return await _terminal.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    internal async ValueTask<ZLinkDrainResult> ForceStopAsync(ZLinkDrainForceReason reason)
    {
        _admission.BeginDrain(ZLinkDrainOwner.Shutdown);
        var forced = await ForceStopAsync(reason, hasCommitted: false, committedUnitCount: 0)
            .ConfigureAwait(false);
        _terminal.TrySetResult(forced);
        return forced;
    }

    private async Task<ZLinkDrainResult> ExecuteSharedAsync(
        ZLinkFrameworkLifecycleIntent intent,
        TimeSpan deadline,
        Action? relocationDetached
    )
    {
        using var flow = ZLinkFlowContext.Enter(
            null,
            null,
            _flowCaptureEnabled(),
            ZLinkFlowOrigin.Lifecycle
        );
        ZLinkDrainResult result;
        try
        {
            var shutdownToken =
                intent == ZLinkFrameworkLifecycleIntent.Shutdown
                    ? ShutdownCancellationToken
                    : CancellationToken.None;
            using var deadlineSource = shutdownToken.CanBeCanceled
                ? null
                : new CancellationTokenSource(deadline);
            var deadlineToken = shutdownToken.CanBeCanceled ? shutdownToken : deadlineSource!.Token;
            TaskCompletionSource? detached = null;
            Action? observedRelocationDetached = relocationDetached;
            if (intent == ZLinkFrameworkLifecycleIntent.Relocate)
            {
                detached = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
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

            var execution = _executor
                .ExecuteWithProgressAsync(
                    intent,
                    deadline,
                    observedRelocationDetached,
                    deadlineToken
                )
                .AsTask();
            if (detached is not null)
            {
                var first = await Task.WhenAny(detached.Task, execution).ConfigureAwait(false);
                if (ReferenceEquals(first, detached.Task))
                    await PublishStateAsync(ZLinkDrainState.Draining).ConfigureAwait(false);
            }
            var executionResult = await execution.ConfigureAwait(false);
            result = executionResult.Disposition switch
            {
                ZLinkDrainExecutionDisposition.Completed => intent
                    == ZLinkFrameworkLifecycleIntent.Shutdown
                && _shutdownEnd is { } end
                && DateTimeOffset.UtcNow >= end
                    ? new ForceStopped(ZLinkDrainForceReason.DeadlineExceeded)
                    : new Drained(),
                ZLinkDrainExecutionDisposition.Blocked => new DrainBlocked(
                    executionResult.BlockedReason ?? ZLinkFrameworkRelocationReason.RelocationFailed
                ),
                ZLinkDrainExecutionDisposition.ForceStop => await ForceStopAsync(
                        executionResult.ForceReason ?? ZLinkDrainForceReason.TeardownFailed,
                        executionResult.HasCommitted,
                        executionResult.CommittedUnitCount
                    )
                    .ConfigureAwait(false),
                _ => throw new InvalidOperationException(
                    "The drain execution returned an unknown disposition."
                ),
            };
        }
        catch (ZLinkDrainBlockedException blocked)
        {
            result = new DrainBlocked(blocked.Reason);
        }
        catch (OperationCanceledException)
        {
            result = await ForceStopAsync(ZLinkDrainForceReason.DeadlineExceeded)
                .ConfigureAwait(false);
        }
        catch (Exception error)
        {
            _logger?.LogError(error, "ZLink drain execution failed before terminal teardown.");
            result = await ForceStopAsync(ZLinkDrainForceReason.TeardownFailed)
                .ConfigureAwait(false);
        }

        if (result is DrainBlocked)
        {
            await _lane.RunAsync(() => _operation = null).ConfigureAwait(false);
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

        if (intent != ZLinkFrameworkLifecycleIntent.Relocate || result is not Drained)
        {
            _terminal.TrySetResult(result);
        }
        return result;
    }

    private async ValueTask<ZLinkDrainResult> ForceStopAsync(
        ZLinkDrainForceReason reason,
        bool hasCommitted = false,
        ulong committedUnitCount = 0
    )
    {
        var operation = await _lane
            .RunAsync(() =>
            {
                if (_forceStopOperation is not null)
                    return _forceStopOperation;

                EstablishTeardownEnd(DateTimeOffset.UtcNow);
                using (ExecutionContext.SuppressFlow())
                    _forceStopOperation = Task.Run(() =>
                        ExecuteForceStopAsync(reason, hasCommitted, committedUnitCount)
                    );
                return _forceStopOperation;
            })
            .ConfigureAwait(false);
        return await operation.ConfigureAwait(false);
    }

    private async Task<ZLinkDrainResult> ExecuteForceStopAsync(
        ZLinkDrainForceReason reason,
        bool hasCommitted,
        ulong committedUnitCount
    )
    {
        if (reason == ZLinkDrainForceReason.DeadlineExceeded)
        {
            IReadOnlyList<ZLinkUnfinishedOperation> unfinished;
            try
            {
                var snapshot = _snapshotUnfinished();
                if (snapshot.IsCompletedSuccessfully)
                    unfinished = snapshot.Result;
                else
                {
                    _ = snapshot
                        .AsTask()
                        .ContinueWith(
                            static task => _ = task.Exception,
                            TaskContinuationOptions.OnlyOnFaulted
                        );
                    unfinished = [new("owner snapshot pending", "framework-runtime")];
                }
            }
            catch (Exception error)
            {
                _logger?.LogError(error, "ZLink drain owner snapshot failed.");
                unfinished = [new("owner snapshot unavailable", "framework-runtime")];
            }
            try
            {
                RecordUnfinished(unfinished);
            }
            catch (Exception error)
            {
                _logger?.LogError(error, "ZLink drain unfinished operation record failed.");
            }
        }
        try
        {
            await PublishStateAsync(ZLinkDrainState.ForceStopping).ConfigureAwait(false);
        }
        catch (Exception error)
        {
            _logger?.LogError(error, "ZLink force-stopping terminal event publication failed.");
        }
        var now = DateTimeOffset.UtcNow;
        var remaining = TeardownEnd!.Value - now;
        using var teardownBound = new CancellationTokenSource();
        if (remaining <= TimeSpan.Zero)
            teardownBound.Cancel();
        else
            teardownBound.CancelAfter(remaining);
        try
        {
            await _executor.ForceStopAsync(reason, teardownBound.Token).ConfigureAwait(false);
        }
        catch (ZLinkDrainForceException failure)
        {
            reason = HasTeardownFailure(failure.InnerException!, teardownBound.Token)
                ? failure.Reason
                : ZLinkDrainForceReason.DeadlineExceeded;
        }
        catch (OperationCanceledException error)
            when (teardownBound.IsCancellationRequested
                && error.CancellationToken == teardownBound.Token
            )
        {
            reason = ZLinkDrainForceReason.DeadlineExceeded;
        }
        catch (Exception error)
        {
            _logger?.LogError(error, "ZLink forced drain teardown failed.");
            reason = HasTeardownFailure(error, teardownBound.Token)
                ? ZLinkDrainForceReason.TeardownFailed
                : ZLinkDrainForceReason.DeadlineExceeded;
        }
        return new ForceStopped(reason, hasCommitted, committedUnitCount);
    }

    private DateTimeOffset EstablishTeardownEnd(DateTimeOffset confirmedAt) =>
        TeardownEnd ??= (_shutdownEnd ?? confirmedAt) + _executor.OwnerLeaseRenewTimeout;

    private static bool HasTeardownFailure(Exception error, CancellationToken teardownToken)
    {
        if (error is AggregateException aggregate)
            return aggregate.InnerExceptions.Any(inner => HasTeardownFailure(inner, teardownToken));
        if (
            error is OperationCanceledException canceled
            && teardownToken.IsCancellationRequested
            && canceled.CancellationToken == teardownToken
        )
            return false;
        return error
            is not ZLinkFrameworkException
            {
                Origin: ZLinkErrorOrigin.Framework,
                Kind: ZLinkFrameworkErrorKind.DeadlineExceeded
                    or ZLinkFrameworkErrorKind.ShuttingDown
            };
    }

    private void RecordUnfinished(IReadOnlyList<ZLinkUnfinishedOperation> unfinished)
    {
        var names =
            unfinished.Count == 0
                ? "none"
                : string.Join(
                    ", ",
                    unfinished.Select(static operation =>
                        $"{operation.Name} owner={operation.Owner}"
                    )
                );
        var message = $"runtime drain deadline exceeded; unfinished: {names}";
        _logger?.LogError("{DrainUnfinished}", message);
        if (_flowCaptureEnabled())
            _traceUnfinished?.Invoke(message);
    }

    private ValueTask PublishStateAsync(ZLinkDrainState state)
    {
        _logger?.LogInformation("ZLink host lifecycle changed. state={State}", state);
        return ValueTask.CompletedTask;
    }

    private static bool AlwaysDisabled() => false;

    private readonly record struct DrainOperationPreparation(
        Task<ZLinkDrainResult> Operation,
        Action? Start
    );

    public void Dispose() { }
}
