namespace Zlink.Framework.Runtime.Backend.DotNet;

// The table reserves one WorkItem before submit and transfers terminal
// ownership to it after atomic take. Each completion gets a pool turn so an
// inline caller continuation cannot hold up unrelated operations.
internal static class ZLinkCompletionDispatcher
{
    [ThreadStatic]
    private static bool _isCurrentExecution;

    internal static bool IsCurrentExecution => _isCurrentExecution;

    internal static void Post(WorkItem work)
    {
        ArgumentNullException.ThrowIfNull(work);
        ThreadPool.UnsafeQueueUserWorkItem(work, preferLocal: false);
    }

    internal static ValueTask<T> DeliverLifecycle<T>(ValueTask<T> source) =>
        new LifecycleCompletion<T>(source).Dispatch();

    private sealed class LifecycleCompletion<T>(ValueTask<T> source) : WorkItem
    {
        private readonly TaskCompletionSource<T> _caller = new();

        internal ValueTask<T> Dispatch()
        {
            var awaiter = source.ConfigureAwait(false).GetAwaiter();
            if (awaiter.IsCompleted)
                Enqueue();
            else
                awaiter.UnsafeOnCompleted(Enqueue);
            return new ValueTask<T>(_caller.Task);
        }

        private void Enqueue() => Post(this);

        // Both awaiters reference the same source and token; at most one continuation is registered.
        // The single GetResult here completes the permitted IValueTaskSource consumption sequence.
        internal override void Execute() => _caller.SetResult(source.GetAwaiter().GetResult());

        internal override void Completed(Exception? failure)
        {
            if (failure is OperationCanceledException canceled)
                _caller.SetCanceled(canceled.CancellationToken);
            else if (failure is not null)
                _caller.SetException(failure);
        }
    }

    internal abstract class WorkItem : IThreadPoolWorkItem
    {
        internal abstract void Execute();

        internal abstract void Completed(Exception? failure);

        void IThreadPoolWorkItem.Execute()
        {
            _isCurrentExecution = true;
            try
            {
                Run();
            }
            finally
            {
                _isCurrentExecution = false;
            }
        }

        // Runs the terminal and its accounting in the pool turn above.
        private void Run()
        {
            Exception? failure = null;
            try
            {
                Execute();
            }
            catch (Exception exception)
            {
                failure = exception;
            }

            try
            {
                Completed(failure);
            }
            catch (Exception exception)
            {
                TryLogFailure("mesh-completion-accounting", exception);
            }
        }
    }

    private static void TryLogFailure(string taskName, Exception exception)
    {
        try
        {
            ZLinkFrameworkDebugLog.TaskFailure(taskName, exception);
        }
        catch
        {
            // A diagnostic failure cannot prevent the next operation's turn.
        }
    }
}
