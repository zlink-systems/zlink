namespace LocationMessaging.Server.Provider.Infrastructure;

internal sealed class BackpressureGate
{
    private readonly object _sync = new();
    private TaskCompletionSource _release = CreateRelease();

    public void Reset()
    {
        lock (_sync)
            _release = CreateRelease();
    }

    public void Release()
    {
        lock (_sync)
            _release.TrySetResult();
    }

    public Task WaitAsync(CancellationToken cancellationToken)
    {
        lock (_sync)
            return _release.Task.WaitAsync(cancellationToken);
    }

    private static TaskCompletionSource CreateRelease()
        => new(TaskCreationOptions.RunContinuationsAsynchronously);
}
