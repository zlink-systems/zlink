namespace RuntimeMonitoring.Server.Service.Support;

internal sealed class ApplicationDispatchGate
{
    private readonly object _gate = new();
    private TaskCompletionSource _release = NewSource();

    public void Reset()
    {
        lock (_gate)
            _release = NewSource();
    }

    public Task WaitAsync(CancellationToken cancellationToken)
    {
        Task release;
        lock (_gate)
            release = _release.Task;
        return release.WaitAsync(cancellationToken);
    }

    public void Release()
    {
        lock (_gate)
            _release.TrySetResult();
    }

    private static TaskCompletionSource NewSource() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);
}
