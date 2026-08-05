namespace Zlink.Framework.AspNetCore;

/// <summary>
/// Owns auto-connect generation start and stop.
/// </summary>
internal sealed class ZLinkAutoConnectLifecycleCoordinator
{
    private readonly object _gate = new();
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
        lock (_gate)
        {
            _state = state;
            return TryStartUnderLock(cancellationToken) ?? Task.CompletedTask;
        }
    }

    internal ValueTask StopAsync(CancellationToken cancellationToken)
    {
        lock (_gate)
            return new ValueTask(
                _stopTask ??= StopCoreAsync(_startTask, cancellationToken));
    }

    private async Task StopCoreAsync(Task? startTask, CancellationToken cancellationToken)
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
        if (startTask is not null && _stop is { } stop)
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

    private Task? TryStartUnderLock(CancellationToken cancellationToken)
    {
        if (_startTask is not null
            || _stopTask is not null
            || _start is null
            || _state is null)
            return _startTask;

        _startTask = _start(_state, cancellationToken).AsTask();
        return _startTask;
    }
}
