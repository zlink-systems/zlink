using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Configuration;

namespace ObservabilityOps.Server.Support;

public sealed class RelocationOperation
{
    private readonly object _gate = new();
    private Task<ZLinkFrameworkRelocationResult>? _task;
    private Exception? _error;
    private int _terminalCount;

    public MaintenanceStatus Start(
        IZLinkFrameworkRuntime runtime,
        ZLinkFrameworkRelocationMode mode,
        long? targetApplicationVersion,
        TimeSpan deadline)
    {
        lock (_gate)
            _task ??= RunAsync(
                runtime, mode, targetApplicationVersion, deadline);
        return Snapshot();
    }

    public MaintenanceStatus Snapshot()
    {
        lock (_gate)
        {
            ZLinkFrameworkRelocationResult? result =
                _task is { Status: TaskStatus.RanToCompletion } ? _task.Result : null;
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
        Task<ZLinkFrameworkRelocationResult> task;
        lock (_gate)
            task = _task
                   ?? throw new InvalidOperationException(
                       "Relocation has not started.");
        await task.WaitAsync(timeout, cancellationToken);
        return Snapshot();
    }

    private async Task<ZLinkFrameworkRelocationResult> RunAsync(
        IZLinkFrameworkRuntime runtime,
        ZLinkFrameworkRelocationMode mode,
        long? targetApplicationVersion,
        TimeSpan deadline)
    {
        try
        {
            return await runtime.RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = mode,
                    TargetApplicationVersion = targetApplicationVersion,
                    Deadline = deadline
                },
                CancellationToken.None);
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
