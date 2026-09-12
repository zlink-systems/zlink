namespace Zlink.Framework.Runtime.Backend.DotNet;

// Completion ownership moves here after a pending-operation table atomically
// takes its entry. One process-wide lane keeps callbacks off transport and
// cancellation stacks without creating a thread for every table or callback.
internal sealed class ZLinkCompletionDispatcher
{
    internal static ZLinkCompletionDispatcher Shared { get; } = new();

    [ThreadStatic]
    private static bool _isCurrentExecution;

    private readonly object _gate = new();
    private WorkItem? _head;
    private WorkItem? _tail;

    internal ZLinkCompletionDispatcher()
    {
        var worker = new Thread(Run)
        {
            IsBackground = true,
            Name = "zlink-framework-completion"
        };
        if (ExecutionContext.IsFlowSuppressed())
            worker.Start();
        else
            using (ExecutionContext.SuppressFlow())
                worker.Start();
    }

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
                _head = work;
            else
                _tail.Next = work;
            _tail = work;
            Monitor.Pulse(_gate);
        }
    }

    private void Run()
    {
        _isCurrentExecution = true;
        while (true)
        {
            WorkItem work;
            lock (_gate)
            {
                while (_head is null)
                    Monitor.Wait(_gate);
                work = _head;
                _head = work.Next;
                work.Next = null;
                if (_head is null)
                    _tail = null;
            }

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
