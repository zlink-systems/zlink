namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamTaskRunner(CancellationToken shutdownToken)
{
    private readonly object _gate = new();
    private readonly HashSet<Task> _tasks = [];
    private bool _accepting = true;

    public Task Run(Func<CancellationToken, ValueTask> callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        return Start(callback);
    }

    public void RunDetached(Func<CancellationToken, ValueTask> callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        Task task;
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(!_accepting, this);
            task = Start(callback);
            _tasks.Add(task);
        }

        _ = task.ContinueWith(
            static (completed, state) => ((ZlinkStreamTaskRunner)state!).Remove(completed),
            this,
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    public async ValueTask StopAndDrainAsync()
    {
        Task[] tasks;
        lock (_gate)
        {
            _accepting = false;
            tasks = _tasks.ToArray();
        }

        await Task.WhenAll(tasks).ConfigureAwait(false);
    }

    private void Remove(Task task)
    {
        lock (_gate)
        {
            _tasks.Remove(task);
        }
    }

    private Task Start(Func<CancellationToken, ValueTask> callback) =>
        Task.Factory.StartNew(
            static state => RunCoreAsync((TaskState)state!),
            new TaskState(callback, shutdownToken),
            CancellationToken.None,
            TaskCreationOptions.DenyChildAttach,
            TaskScheduler.Default).Unwrap();

    private static async Task RunCoreAsync(TaskState state)
    {
        try
        {
            await state.Callback(state.ShutdownToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (state.ShutdownToken.IsCancellationRequested)
        {
        }
        catch
        {
        }
    }

    private sealed record TaskState(
        Func<CancellationToken, ValueTask> Callback,
        CancellationToken ShutdownToken);
}
