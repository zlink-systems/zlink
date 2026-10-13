namespace Zlink.Framework.Runtime.Backend.DotNet;

// Completion ownership moves here after a pending-operation table atomically
// takes its entry. One process-wide lane keeps callbacks off transport and
// cancellation stacks without creating a thread for every table or callback.
internal sealed class ZLinkCompletionDispatcher : IThreadPoolWorkItem
{
    internal static ZLinkCompletionDispatcher Shared { get; } = new();

    [ThreadStatic]
    private static bool _isCurrentExecution;

    private readonly object _gate = new();
    private WorkItem? _head;
    private WorkItem? _tail;

    internal static bool IsCurrentExecution => _isCurrentExecution;

    // WorkItem is allocated with the pending operation. Posting a terminal
    // result only links that existing node, so a reply, cancellation, or close
    // cannot arrive before its callback has a dispatcher node.
    internal void Post(WorkItem work)
    {
        ArgumentNullException.ThrowIfNull(work);
        lock (_gate)
        {
            if (_tail is null)
            {
                _head = work;
                ThreadPool.UnsafeQueueUserWorkItem(this, preferLocal: false);
            }
            else if (_head is null)
                _head = work;
            else
                _tail.Next = work;
            _tail = work;
        }
    }

    void IThreadPoolWorkItem.Execute()
    {
        _isCurrentExecution = true;
        try
        {
            while (true)
            {
                WorkItem work;
                lock (_gate)
                {
                    if (_head is null)
                    {
                        _tail = null;
                        return;
                    }
                    work = _head;
                    _head = null;
                    // Retain the tail as this drain's reservation. A post
                    // during callback execution starts the next accepted batch
                    // without scheduling a second consumer for the same lane.
                }

                DrainBatch(work);
            }
        }
        finally
        {
            _isCurrentExecution = false;
        }
    }

    private static void DrainBatch(WorkItem? work)
    {
        while (work is not null)
        {
            // Detach only the already accepted chain. Work posted by a callback
            // belongs to the next turn, after all predecessors in this batch.
            var next = work.Next;
            work.Next = null;
            Exception? failure = null;
            try
            {
                work.Execute();
            }
            catch (Exception exception)
            {
                failure = exception;
            }

            try
            {
                work.Completed(failure);
            }
            catch (Exception exception)
            {
                // Completion-accounting failures must not stop the only shared
                // lane and strand every later terminal callback.
                TryLogFailure("mesh-completion-accounting", exception);
            }
            work = next;
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
            // Diagnostics must never terminate the only process-wide lane.
        }
    }

    internal abstract class WorkItem
    {
        internal WorkItem? Next { get; set; }

        internal abstract void Execute();

        internal abstract void Completed(Exception? failure);
    }
}
