namespace Zlink.Framework.Runtime.Backend.DotNet;

// Completion ownership moves here after a pending-operation table atomically
// takes its entry. One process-wide lane keeps callbacks off transport and
// cancellation stacks without creating a thread for every table or callback.
internal sealed class ZLinkCompletionDispatcher
{
    private const int DefaultCapacity = 4_096;

    internal static ZLinkCompletionDispatcher Shared { get; } = new();

    [ThreadStatic]
    private static bool _isCurrentExecution;

    private readonly object _gate = new();
    private readonly int _capacity;
    private WorkItem? _head;
    private WorkItem? _tail;
    private int _reservations;

    private ZLinkCompletionDispatcher()
        : this(DefaultCapacity)
    {
    }

    // Isolated instances let unit tests prove aggregate admission at a small
    // capacity. Production has exactly one call site: Shared above.
    internal ZLinkCompletionDispatcher(int capacity)
    {
        if (capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        _capacity = capacity;
        var worker = new Thread(Run)
        {
            IsBackground = true,
            Name = "zlink-framework-completion"
        };
        worker.Start();
    }

    internal static bool IsCurrentExecution => _isCurrentExecution;

    // The process-wide admission slot is acquired while the owning table lock
    // is held. The same preallocated node retains it until synchronous
    // unregister or callback completion.
    internal bool TryReserve(WorkItem work)
    {
        ArgumentNullException.ThrowIfNull(work);
        lock (_gate)
        {
            if (_reservations >= _capacity)
                return false;
            work.HasReservation = true;
            _reservations++;
            return true;
        }
    }

    internal void ReleaseReservation(WorkItem work)
    {
        ArgumentNullException.ThrowIfNull(work);
        lock (_gate)
        {
            if (!work.HasReservation)
                return;
            work.HasReservation = false;
            _reservations--;
        }
    }

    // WorkItem is allocated while its pending-operation reservation is being
    // admitted. Posting a terminal result only links that existing node, so a
    // reply, cancellation, or close cannot be lost to queue growth allocation.
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
        internal bool HasReservation { get; set; }

        internal WorkItem? Next { get; set; }

        internal abstract void Execute();

        internal abstract void Completed(Exception? failure);
    }
}
