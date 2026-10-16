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

        // Runs the terminal and its accounting. The pool turn above calls it; a
        // terminal that is known before its caller holds the completion calls it
        // in that call, because no continuation can be waiting yet.
        internal void Run()
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
