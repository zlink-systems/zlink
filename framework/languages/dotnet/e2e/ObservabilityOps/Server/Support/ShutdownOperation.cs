using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Configuration;

namespace ObservabilityOps.Server.Support;

public sealed class ShutdownOperation
{
    private readonly object _gate = new();
    private Task<ZLinkFrameworkTerminationResult>? _task;
    private Exception? _error;
    private int _terminalCount;

    public MaintenanceStatus Start(
        IZLinkFrameworkRuntime runtime,
        TimeSpan deadline)
    {
        lock (_gate)
            _task ??= RunAsync(runtime, deadline);
        return Snapshot();
    }

    public MaintenanceStatus Snapshot()
    {
        lock (_gate)
        {
            ZLinkFrameworkTerminationResult? result =
                _task is { Status: TaskStatus.RanToCompletion }
                    ? _task.Result
                    : null;
            return new MaintenanceStatus(
                _task is not null,
                _task?.IsCompleted ?? false,
                result?.Outcome.ToString(),
                result?.Reason.ToString(),
                _error?.Message,
                Volatile.Read(ref _terminalCount));
        }
    }

    public async Task<MaintenanceStatus> WaitAsync(
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        Task<ZLinkFrameworkTerminationResult> task;
        lock (_gate)
            task = _task
                   ?? throw new InvalidOperationException(
                       "Shutdown has not started.");
        await task.WaitAsync(timeout, cancellationToken);
        return Snapshot();
    }

    private async Task<ZLinkFrameworkTerminationResult> RunAsync(
        IZLinkFrameworkRuntime runtime,
        TimeSpan deadline)
    {
        try
        {
            return await runtime.ShutdownAsync(deadline, CancellationToken.None);
        }
        catch (Exception exception)
        {
            lock (_gate) _error = exception;
            throw;
        }
        finally
        {
            Interlocked.Increment(ref _terminalCount);
        }
    }
}
