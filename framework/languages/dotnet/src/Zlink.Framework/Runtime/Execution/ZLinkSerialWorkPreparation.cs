namespace Zlink.Framework.Runtime.Execution;

// Readiness belongs to an unstarted FIFO record, never to an execution turn.
internal sealed class ZLinkSerialWorkPreparation
{
    private readonly Func<CancellationToken, ValueTask> _prepare;
    private readonly Action _cancel;
    private readonly Action _notify;
    private readonly Action<Exception>? _reject;
    private CancellationTokenSource? _cancellation = new();
    private readonly CancellationToken _token;
    private Task? _ready;

    internal ZLinkSerialWorkPreparation(
        Func<CancellationToken, ValueTask> prepare,
        Action cancel,
        Action notify,
        Action<Exception>? reject = null
    )
    {
        _prepare = prepare;
        _cancel = cancel;
        _notify = notify;
        _reject = reject;
        _token = _cancellation.Token;
    }

    internal bool IsReady
    {
        get
        {
            var task = Volatile.Read(ref _ready);
            if (task is null)
            {
                var completion = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                task = Interlocked.CompareExchange(ref _ready, completion.Task, null);
                if (task is null)
                {
                    task = completion.Task;
                    _ = PrepareAsync(completion);
                }
            }
            return task.IsCompleted;
        }
    }

    private async Task PrepareAsync(TaskCompletionSource completion)
    {
        try
        {
            await _prepare(_token).ConfigureAwait(false);
            _token.ThrowIfCancellationRequested();
            completion.TrySetResult();
        }
        catch (Exception error)
        {
            completion.TrySetException(error);
            _ = completion.Task.Exception;
            _reject?.Invoke(error);
        }
        finally
        {
            if (_token.IsCancellationRequested)
                _cancel();
            _notify();
        }
    }

    internal void Complete() => Volatile.Read(ref _ready)!.GetAwaiter().GetResult();

    internal void Cancel()
    {
        var cancellation = Interlocked.Exchange(ref _cancellation, null);
        if (cancellation is null)
            return;
        try
        {
            cancellation.Cancel();
        }
        finally
        {
            cancellation.Dispose();
            _cancel();
        }
    }
}
