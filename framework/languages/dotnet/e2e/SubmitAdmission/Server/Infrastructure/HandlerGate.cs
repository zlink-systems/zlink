namespace SubmitAdmission.Server.Infrastructure;

internal sealed class HandlerGate
{
    private readonly object _gate = new();
    private TaskCompletionSource<bool> _release = Completed();

    public void Close()
    {
        lock (_gate)
        {
            if (!_release.Task.IsCompleted) return;
            _release = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        }
    }

    public void Open()
    {
        TaskCompletionSource<bool> release;
        lock (_gate) release = _release;
        release.TrySetResult(true);
    }

    public Task WaitAsync(CancellationToken cancellationToken)
    {
        lock (_gate) return _release.Task.WaitAsync(cancellationToken);
    }

    private static TaskCompletionSource<bool> Completed()
    {
        var value = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        value.SetResult(true);
        return value;
    }
}
