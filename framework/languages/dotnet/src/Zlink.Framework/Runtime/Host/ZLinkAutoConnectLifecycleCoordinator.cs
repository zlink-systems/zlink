using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Host;

/// <summary>
/// Owns auto-connect generation start and stop.
/// </summary>
internal sealed class ZLinkAutoConnectLifecycleCoordinator
{
    private readonly ZLinkStateLane _lane = new();
    private readonly Func<ZLinkFrameworkComponentState, CancellationToken, ValueTask>? _start;
    private readonly Func<CancellationToken, ValueTask>? _stop;
    private ZLinkFrameworkComponentState? _state;
    private Task? _startTask;
    private Task? _stopTask;

    internal ZLinkAutoConnectLifecycleCoordinator(
        ZLinkLocationAutoConnectHost? autoConnect)
        : this(
            autoConnect is null ? null : autoConnect.StartAsync,
            autoConnect is null ? null : autoConnect.StopAsync)
    {
    }

    internal ZLinkAutoConnectLifecycleCoordinator(
        Func<ZLinkFrameworkComponentState, CancellationToken, ValueTask>? start,
        Func<CancellationToken, ValueTask>? stop)
    {
        _start = start;
        _stop = stop;
    }

    internal Task FrameworkReadyAsync(
        ZLinkFrameworkComponentState state,
        CancellationToken cancellationToken)
    {
        var start = AwaitStateLane(_lane.RunAsync(() =>
        {
            _state = state;
            return PrepareStart(cancellationToken);
        }));
        if (start is null)
            return AwaitStateLane(_lane.RunAsync(
                () => _startTask ?? Task.CompletedTask));

        try
        {
            CompleteTask(start.Completion, _start!(start.State, cancellationToken).AsTask());
        }
        catch (Exception exception)
        {
            AwaitStateLane(_lane.RunAsync(() => _startTask = null));
            start.Completion.TrySetException(exception);
            throw;
        }
        return start.Completion.Task;
    }

    internal ValueTask StopAsync(CancellationToken cancellationToken)
    {
        var stop = AwaitStateLane(_lane.RunAsync(() => PrepareStop()));
        if (stop.Existing is not null)
            return new ValueTask(stop.Existing);

        CompleteTask(
            stop.Completion!,
            StopCoreAsync(stop.StartTask, _stop, cancellationToken));
        return new ValueTask(stop.Completion!.Task);
    }

    private static async Task StopCoreAsync(
        Task? startTask,
        Func<CancellationToken, ValueTask>? stop,
        CancellationToken cancellationToken)
    {
        Exception? startFailure = null;
        if (startTask is not null)
            try
            {
                await startTask.ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                startFailure = exception;
            }

        Exception? stopFailure = null;
        if (startTask is not null && stop is not null)
            try
            {
                await stop(cancellationToken).ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                stopFailure = exception;
            }

        if (startFailure is not null && stopFailure is not null)
            throw new AggregateException(startFailure, stopFailure);
        if (startFailure is not null)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(startFailure).Throw();
        if (stopFailure is not null)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(stopFailure).Throw();
    }

    private StartPreparation? PrepareStart(CancellationToken cancellationToken)
    {
        if (_startTask is not null
            || _stopTask is not null
            || _start is null
            || _state is null)
            return null;

        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _startTask = completion.Task;
        return new StartPreparation(_state, completion);
    }

    private StopPreparation PrepareStop()
    {
        if (_stopTask is not null)
            return new StopPreparation(_stopTask, null, null);

        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _stopTask = completion.Task;
        return new StopPreparation(null, _startTask, completion);
    }

    private static async void CompleteTask(TaskCompletionSource completion, Task task)
    {
        try
        {
            await task.ConfigureAwait(false);
            completion.TrySetResult();
        }
        catch (OperationCanceledException) when (task.IsCanceled)
        {
            completion.TrySetCanceled();
        }
        catch (Exception exception)
        {
            completion.TrySetException(exception);
        }
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private sealed record StartPreparation(
        ZLinkFrameworkComponentState State,
        TaskCompletionSource Completion);

    private sealed record StopPreparation(
        Task? Existing,
        Task? StartTask,
        TaskCompletionSource? Completion);
}
