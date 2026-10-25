using System.Diagnostics;
using System.Threading.Tasks.Sources;

namespace Zlink.Framework.Runtime.Execution;

// The process owns physical execution resources. Sources retain socket, permit and
// coroutine lifetimes; application entries only reserve a turn of their existing queue.
internal sealed class ZLinkProcessExecutionPool
{
    private static readonly object ProcessGate = new();
    private static ZLinkProcessExecutionPool? _current;

    // Call context for a finite receive continuation. Worker.Receiving alone
    // owns role assignment; this context only permits inline poller mutations.
    [ThreadStatic]
    private static ZLinkProcessExecutionPool? _receiving;
    private readonly object _gate = new();
    private IPoller _poller = null!;
    private IZlinkTimer? _wake;
    private readonly List<ReceiveSource> _sources = [];
    private readonly HashSet<ApplicationSource> _applications = [];
    private readonly Dictionary<nuint, Registration> _registrations = [];
    private readonly HashSet<ReceiveSource> _dirty = [];
    private readonly HashSet<ReceiveSource> _resuming = [];
    private readonly List<ReceiveSource> _deadlines = [];

    // Ready-owner reservations (spec 04 §3); records stay in their owner queues.
    private readonly LinkedList<ApplicationSource> _ready = [];
    private readonly HashSet<ReceiveSource> _receiveReady = [];
    private readonly Worker[] _workers;
    private readonly TaskCompletionSource _joined = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );
    private PollEvent[] _events = new PollEvent[1];
    private nuint _nextSlot = 1;
    private int _references;
    private int _remainingWorkers;
    private bool _stopping;
    private Exception? _failure;

    private static readonly TimeSpan ReceiveWakeInterval = TimeSpan.FromTicks(1);
    private const int WakeFireCount = 1;
    private const int MinimumApplicationWorkers = 2;
    internal static int ApplicationWorkerCount =>
        Math.Max(MinimumApplicationWorkers, Environment.ProcessorCount);
    internal static int WorkerCount => 1 + ApplicationWorkerCount;

    private ZLinkProcessExecutionPool(Func<IPoller>? factory, Task previousGeneration)
    {
        _workers = new Worker[WorkerCount];
        _remainingWorkers = 1;
        for (var index = 0; index < _workers.Length; index++)
            _workers[index] = new Worker { Receiving = index == 0, Idle = index != 0 };
        _ = ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(() =>
            StartAsync(factory, previousGeneration)
        );
    }

    private async Task StartAsync(Func<IPoller>? factory, Task previousGeneration)
    {
        // Only ordering is inherited from the old generation; its own callers
        // observe its failure. A new generation still starts after cleanup.
        await Task.WhenAny(previousGeneration).ConfigureAwait(false);
        List<(Action<object?> Callback, object? State)>? failedContinuations = null;
        try
        {
            lock (_gate)
            {
                try
                {
                    _poller = (factory ?? Systems.Zlink.Zlink.CreatePoller)();
                    _wake = Systems.Zlink.Zlink.CreateTimer();
                    _poller.Add(_wake, 0);
                    for (var index = 0; index < _workers.Length; index++)
                    {
                        var worker = _workers[index];
                        Interlocked.Increment(ref _remainingWorkers);
                        try
                        {
                            _ = ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(() =>
                                ZLinkRuntimeTaskRunner.StartDedicatedThread(
                                    _ => RunWorker(worker),
                                    null
                                )
                            );
                        }
                        catch
                        {
                            Interlocked.Decrement(ref _remainingWorkers);
                            throw;
                        }
                    }
                }
                catch (Exception failure)
                {
                    failedContinuations = FailReceiveUnderLock(failure);
                }
            }
            NotifyFailedSources(failedContinuations);
        }
        finally
        {
            WorkerExited();
        }
    }

    private void WorkerExited()
    {
        if (Interlocked.Decrement(ref _remainingWorkers) != 0)
            return;
        ThreadPool.UnsafeQueueUserWorkItem(static pool => pool.JoinExitedWorkers(), this, false);
    }

    private void JoinExitedWorkers()
    {
        foreach (var worker in _workers)
            if (worker.Thread is { } thread)
                thread.Join();
            else
                worker.Turn.Dispose();
        if (_failure is { } error)
            _joined.TrySetException(error);
        else
            _joined.TrySetResult();
    }

    private void FailReceive(Exception failure)
    {
        List<(Action<object?> Callback, object? State)>? continuations;
        lock (_gate)
            continuations = FailReceiveUnderLock(failure);
        NotifyFailedSources(continuations);
    }

    private static void NotifyFailedSources(
        List<(Action<object?> Callback, object? State)>? continuations
    )
    {
        if (continuations is null)
            return;
        foreach (var (callback, state) in continuations)
            callback(state);
    }

    private List<(Action<object?> Callback, object? State)>? FailReceiveUnderLock(Exception failure)
    {
        List<(Action<object?> Callback, object? State)> continuations = [];
        if (_stopping && _failure is not null && _poller is null)
            return null;
        _failure ??= failure;
        _stopping = true;
        var failures = new ZLinkFailureCollector(_failure);
        if (_poller is not null)
            failures.Capture(_poller.Clear);
        if (_wake is not null)
            failures.Capture(_wake.Dispose);
        _wake = null;
        if (_poller is not null)
            failures.Capture(_poller.Dispose);
        _poller = null!;
        _failure = failures.BuildException()!;
        foreach (var source in _sources)
        {
            source.Closing = true;
            source.Failure = _failure;
            source.Removed.TrySetException(_failure);
            if (source.Continuation is { } callback)
                continuations.Add((callback, source.ContinuationState));
            source.Continuation = null;
            source.Waiting = false;
        }
        _sources.Clear();
        foreach (var application in _applications)
            application.FailUnderLock(_failure);
        _registrations.Clear();
        _dirty.Clear();
        _resuming.Clear();
        _receiveReady.Clear();
        foreach (var source in _deadlines)
            source.DeadlineIndex = -1;
        _deadlines.Clear();
        _ready.Clear();
        Monitor.PulseAll(_gate);
        return continuations;
    }

    private static ZLinkProcessExecutionPool Acquire(Func<IPoller>? factory = null)
    {
        lock (ProcessGate)
        {
            if (_current is { } current)
                lock (current._gate)
                    if (!current._stopping)
                    {
                        current._references++;
                        return current;
                    }
            var pool = new ZLinkProcessExecutionPool(
                factory,
                _current?._joined.Task ?? Task.CompletedTask
            );
            lock (pool._gate)
                pool._references++;
            _current = pool;
            return pool;
        }
    }

    internal static ReceiveSource CreateReceiveSource(Func<IPoller>? factory = null)
    {
        var pool = Acquire(factory);
        lock (pool._gate)
        {
            var source = new ReceiveSource(pool, pool._nextSlot++);
            if (pool._failure is { } failure)
            {
                source.Closing = true;
                source.Failure = failure;
                source.Removed.TrySetException(failure);
            }
            else
            {
                pool._sources.Add(source);
                pool._deadlines.EnsureCapacity(pool._sources.Count);
                pool._dirty.EnsureCapacity(pool._sources.Count);
                pool._resuming.EnsureCapacity(pool._sources.Count);
                pool._receiveReady.EnsureCapacity(pool._sources.Count);
                pool._dirty.Add(source);
            }
            pool.WakeUnderLock();
            return source;
        }
    }

    internal static ApplicationSource CreateApplicationSource(
        Func<ApplicationTurn, ValueTask> turn,
        Action<Exception> report,
        Func<IPoller>? factory = null,
        int maximumConcurrentTurns = 1
    )
    {
        var pool = Acquire(factory);
        lock (pool._gate)
        {
            var source = new ApplicationSource(pool, turn, report, maximumConcurrentTurns);
            pool._applications.Add(source);
            if (pool._failure is { } failure)
                source.FailUnderLock(failure);
            return source;
        }
    }

    private Task ReleaseUnderLock()
    {
        if (--_references != 0)
            return Task.CompletedTask;
        _stopping = true;
        WakeUnderLock();
        Monitor.PulseAll(_gate);
        return _joined.Task;
    }

    private void WakeUnderLock() => _wake?.Start(ReceiveWakeInterval, WakeFireCount);

    private void RunWorker(Worker worker)
    {
        worker.Thread = Thread.CurrentThread;
        try
        {
            while (true)
            {
                ApplicationSource? application = null;
                lock (_gate)
                {
                    while (
                        !worker.Receiving
                        && (application = TakeApplicationUnderLock()) is null
                        && !_stopping
                    )
                    {
                        worker.Idle = true;
                        Monitor.Wait(_gate);
                    }
                    worker.Idle = false;
                    if (_stopping && (!worker.Receiving || _poller is null))
                        return;
                }
                if (worker.Receiving)
                {
                    _receiving = this;
                    try
                    {
                        application = ReceiveUntilHandoff(worker);
                    }
                    finally
                    {
                        _receiving = null;
                    }
                    if (_stopping)
                        return;
                }
                application?.Run(worker.Turn);
            }
        }
        catch (Exception failure)
        {
            if (worker.Receiving)
                FailReceive(failure);
            else
                lock (_gate)
                {
                    _failure ??= failure;
                    _stopping = true;
                    WakeUnderLock();
                    Monitor.PulseAll(_gate);
                }
        }
        finally
        {
            try
            {
                worker.Turn.Dispose();
            }
            catch (Exception failure)
            {
                lock (_gate)
                {
                    _failure ??= failure;
                    _stopping = true;
                    WakeUnderLock();
                    Monitor.PulseAll(_gate);
                }
            }
            finally
            {
                WorkerExited();
            }
        }
    }

    private ApplicationSource? TakeApplicationUnderLock()
    {
        for (var entry = _ready.First; entry is not null; entry = entry.Next)
        {
            var application = entry.Value;
            if (application.Running >= application.MaximumConcurrentTurns)
                continue;
            _ready.Remove(entry);
            application.Running++;
            return application;
        }
        return null;
    }

    private ApplicationSource? ReceiveUntilHandoff(Worker worker)
    {
        var readySources = _receiveReady;
        while (true)
        {
            TimeSpan timeout = Timeout.InfiniteTimeSpan;
            List<(Action<object?> Callback, object? State)>? failedContinuations = null;
            var terminating = false;
            lock (_gate)
            {
                while (_dirty.Count != 0)
                {
                    var enumerator = _dirty.GetEnumerator();
                    enumerator.MoveNext();
                    var source = enumerator.Current;
                    _dirty.Remove(source);
                    ApplySourceUnderLock(source);
                }
                if (_stopping)
                {
                    if (_failure is { } failure)
                        failedContinuations = FailReceiveUnderLock(failure);
                    else
                    {
                        _poller.Remove(_wake!);
                        _wake!.Dispose();
                        _wake = null;
                        _poller.Dispose();
                        _poller = null!;
                    }
                    worker.Receiving = false;
                    terminating = true;
                    Monitor.PulseAll(_gate);
                }
                if (!terminating)
                {
                    CollectReadySourcesUnderLock(readySources);
                    if (readySources.Count != 0)
                        timeout = TimeSpan.Zero;
                    else if (_deadlines.Count != 0)
                    {
                        var remaining = Stopwatch.GetElapsedTime(
                            Stopwatch.GetTimestamp(),
                            _deadlines[0].Deadline
                        );
                        timeout = remaining > TimeSpan.Zero ? remaining : TimeSpan.Zero;
                    }
                    if (_events.Length != _registrations.Count + 1)
                        _events = new PollEvent[_registrations.Count + 1];
                }
            }
            if (terminating)
            {
                NotifyFailedSources(failedContinuations);
                return null;
            }
            var count = _poller.Wait(_events, timeout);
            lock (_gate)
            {
                for (var index = 0; index < count; index++)
                {
                    var item = _events[index];
                    if (item.SourceKind == PollSourceKind.Timer && item.Slot == 0)
                    {
                        _wake!.Recv();
                        continue;
                    }
                    if (!_registrations.TryGetValue(item.Slot, out var registration))
                        continue;
                    var source = registration.Source;
                    if (!source.Waiting || source.Continuation is null)
                        continue;
                    source.Events.Add(
                        new ReceiveEvent(item.SourceKind, registration.LocalSlot, item.Revents)
                    );
                    readySources.Add(source);
                }
                CollectReadySourcesUnderLock(readySources);
            }
            // Completing these waits resumes only the finite source turn. Every source
            // reaches its next readiness/permit/management await before the role moves.
            foreach (var source in readySources)
            {
                Action<object?>? continuation;
                object? continuationState;
                int received;
                lock (_gate)
                {
                    continuation = source.Continuation;
                    continuationState = source.ContinuationState;
                    source.Continuation = null;
                    source.Waiting = false;
                    RemoveDeadlineUnderLock(source);
                    _resuming.Remove(source);
                    source.Woken = false;
                    received = source.Events.Count;
                    for (
                        var index = 0;
                        index < Math.Min(received, source.Destination!.Length);
                        index++
                    )
                        source.Destination![index] = source.Events[index];
                    source.Events.Clear();
                    source.Received = Math.Min(received, source.Destination!.Length);
                }
                try
                {
                    continuation?.Invoke(continuationState);
                }
                finally
                {
                    lock (_gate)
                    {
                        if (source.Closing)
                            source.Removed.TrySetResult();
                        else if (!source.Waiting || source.Continuation is null)
                            _dirty.Add(source);
                    }
                }
            }
            readySources.Clear();
            lock (_gate)
            {
                if (_ready.Count == 0)
                    continue;
                var application = TakeApplicationUnderLock();
                if (application is null)
                    continue;
                foreach (var replacement in _workers)
                {
                    if (!replacement.Idle)
                        continue;
                    // Reservation is exclusive before the old receiver runs any application.
                    replacement.Idle = false;
                    replacement.Receiving = true;
                    worker.Receiving = false;
                    Monitor.PulseAll(_gate);
                    return application;
                }
                // No idle resource: the current worker keeps the receive role.
                application.Running--;
                _ready.AddLast(application.ReadyEntry);
                Monitor.PulseAll(_gate);
            }
        }
    }

    private void CollectReadySourcesUnderLock(HashSet<ReceiveSource> ready)
    {
        foreach (var source in _resuming)
            ready.Add(source);
        _resuming.Clear();
        var now = Stopwatch.GetTimestamp();
        while (_deadlines.Count != 0 && _deadlines[0].Deadline <= now)
        {
            var due = _deadlines[0];
            RemoveDeadlineUnderLock(due);
            ready.Add(due);
        }
    }

    private static bool Earlier(ReceiveSource left, ReceiveSource right) =>
        left.Deadline < right.Deadline || left.Deadline == right.Deadline && left.Id < right.Id;

    private void AddDeadlineUnderLock(ReceiveSource source)
    {
        source.DeadlineIndex = _deadlines.Count;
        _deadlines.Add(source);
        SiftDeadlineUp(source.DeadlineIndex);
    }

    private void RemoveDeadlineUnderLock(ReceiveSource source)
    {
        var index = source.DeadlineIndex;
        if (index < 0)
            return;
        source.DeadlineIndex = -1;
        var last = _deadlines[^1];
        _deadlines.RemoveAt(_deadlines.Count - 1);
        if (index == _deadlines.Count)
            return;
        _deadlines[index] = last;
        last.DeadlineIndex = index;
        if (index > 0 && Earlier(last, _deadlines[(index - 1) / 2]))
            SiftDeadlineUp(index);
        else
            SiftDeadlineDown(index);
    }

    private void SiftDeadlineUp(int index)
    {
        while (index > 0)
        {
            var parent = (index - 1) / 2;
            if (!Earlier(_deadlines[index], _deadlines[parent]))
                return;
            SwapDeadline(index, parent);
            index = parent;
        }
    }

    private void SiftDeadlineDown(int index)
    {
        while (index * 2 + 1 < _deadlines.Count)
        {
            var child = index * 2 + 1;
            if (child + 1 < _deadlines.Count && Earlier(_deadlines[child + 1], _deadlines[child]))
                child++;
            if (!Earlier(_deadlines[child], _deadlines[index]))
                return;
            SwapDeadline(index, child);
            index = child;
        }
    }

    private void SwapDeadline(int left, int right)
    {
        (_deadlines[left], _deadlines[right]) = (_deadlines[right], _deadlines[left]);
        _deadlines[left].DeadlineIndex = left;
        _deadlines[right].DeadlineIndex = right;
    }

    private void ApplySourceUnderLock(ReceiveSource source)
    {
        foreach (var registration in source.Registrations)
        {
            if (source.Closing)
            {
                if (registration.Registered)
                {
                    if (registration.Target is IZlinkSocket socket)
                        _poller.Remove(socket);
                    else
                        _poller.Remove((ISocketMonitor)registration.Target);
                    _registrations.Remove(registration.Slot);
                    registration.Registered = false;
                }
                continue;
            }
            var mask =
                source.Waiting && source.Continuation is not null
                    ? registration.Events
                    : registration.Events & PollEventFlags.PollCompletion;
            if (!registration.Registered)
            {
                if (registration.Target is IZlinkSocket socket)
                    _poller.Add(socket, mask, registration.Slot);
                else
                    _poller.Add((ISocketMonitor)registration.Target, mask, registration.Slot);
                _registrations.Add(registration.Slot, registration);
                registration.Registered = true;
                registration.Applied = mask;
            }
            else if (registration.Applied != mask)
            {
                if (registration.Target is IZlinkSocket socket)
                    _poller.Modify(socket, mask);
                else
                    _poller.Modify((ISocketMonitor)registration.Target, mask);
                registration.Applied = mask;
            }
        }
        if (source.Closing)
        {
            _sources.Remove(source);
            RemoveDeadlineUnderLock(source);
            _resuming.Remove(source);
            source.Failure ??= new ObjectDisposedException(nameof(ReceiveSource));
            if (source.Waiting && source.Continuation is not null)
                _receiveReady.Add(source);
            else
                source.Removed.TrySetResult();
        }
    }

    private sealed class Worker
    {
        internal Thread? Thread;
        internal readonly ApplicationTurn Turn = new();
        internal bool Receiving;
        internal bool Idle;
    }

    internal sealed class Registration(
        ReceiveSource source,
        object target,
        PollEventFlags events,
        nuint localSlot,
        nuint slot
    )
    {
        internal readonly ReceiveSource Source = source;
        internal readonly object Target = target;
        internal PollEventFlags Events = events;
        internal readonly nuint LocalSlot = localSlot;
        internal readonly nuint Slot = slot;
        internal PollEventFlags Applied;
        internal bool Registered;
    }

    internal readonly record struct ReceiveEvent(
        PollSourceKind SourceKind,
        nuint Slot,
        PollEventFlags Revents
    );

    internal sealed class ReceiveSource(ZLinkProcessExecutionPool pool, nuint id)
        : IAsyncDisposable,
            IValueTaskSource<int>,
            IValueTaskSource
    {
        internal readonly nuint Id = id;
        internal readonly List<Registration> Registrations = [];
        internal readonly List<ReceiveEvent> Events = [];
        internal readonly TaskCompletionSource Removed = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal bool Waiting;
        internal Action<object?>? Continuation;
        internal object? ContinuationState;
        internal int Received;
        internal ReceiveEvent[]? Destination;
        internal long Deadline;
        internal int DeadlineIndex = -1;
        internal bool Woken;
        internal bool Closing;
        internal Exception? Failure;
        internal bool IsTerminated => Closing;
        private Task? _dispose;
        private CancellationToken _cancellationToken;
        private CancellationTokenRegistration _wakeOnStop;

        internal void SetCancellationToken(CancellationToken token)
        {
            if (_cancellationToken == token)
                return;
            _wakeOnStop.Dispose();
            _cancellationToken = token;
            _wakeOnStop = token.UnsafeRegister(
                static state => ((ReceiveSource)state!).Wake(),
                this
            );
        }

        internal void Add(IZlinkSocket socket, PollEventFlags events, nuint slot) =>
            AddCore(socket, events, slot);

        internal void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot) =>
            AddCore(monitor, events, slot);

        private void AddCore(object target, PollEventFlags events, nuint slot)
        {
            lock (pool._gate)
            {
                if (Failure is { } failure)
                    System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
                ObjectDisposedException.ThrowIf(Closing, this);
                Registrations.Add(new Registration(this, target, events, slot, pool._nextSlot++));
                Events.EnsureCapacity(Registrations.Count);
                pool._dirty.Add(this);
                pool.WakeUnderLock();
            }
        }

        internal void Modify(IZlinkSocket socket, PollEventFlags events)
        {
            lock (pool._gate)
            {
                Registration? registration = null;
                foreach (var item in Registrations)
                    if (ReferenceEquals(item.Target, socket))
                    {
                        registration = item;
                        break;
                    }
                if (registration is null)
                    throw new InvalidOperationException("The receive socket is not registered.");
                if (ReferenceEquals(_receiving, pool) && registration.Registered)
                {
                    pool._poller.Modify(socket, events);
                    registration.Applied = events;
                }
                registration.Events = events;
                pool._dirty.Add(this);
                if (!ReferenceEquals(_receiving, pool))
                    pool.WakeUnderLock();
            }
        }

        internal ValueTask<int> WaitAsync(ReceiveEvent[] events, TimeSpan timeout)
        {
            lock (pool._gate)
            {
                if (Failure is { } failure)
                    System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failure).Throw();
                ObjectDisposedException.ThrowIf(Closing, this);
                if (Waiting)
                    throw new InvalidOperationException("A receive source already has a waiter.");
                Destination = events;
                Deadline =
                    timeout == Timeout.InfiniteTimeSpan
                        ? long.MaxValue
                        : Stopwatch.GetTimestamp()
                            + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
                // Inline continuations belong to the receive role, never the CLR pool.
                Waiting = true;
                return new ValueTask<int>(this, 0);
            }
        }

        public ValueTaskSourceStatus GetStatus(short token) => ValueTaskSourceStatus.Pending;

        public int GetResult(short token)
        {
            if (Failure is { } error)
                System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(error).Throw();
            return Received;
        }

        public void OnCompleted(
            Action<object?> continuation,
            object? state,
            short token,
            ValueTaskSourceOnCompletedFlags flags
        )
        {
            lock (pool._gate)
            {
                if (Failure is not null)
                {
                    ThreadPool.QueueUserWorkItem(
                        static value => value.Callback(value.State),
                        (Callback: continuation, State: state),
                        false
                    );
                    return;
                }
                Continuation = continuation;
                ContinuationState = state;
                pool._dirty.Add(this);
                if (Woken || Deadline <= Stopwatch.GetTimestamp())
                    pool._resuming.Add(this);
                else if (Deadline != long.MaxValue)
                    pool.AddDeadlineUnderLock(this);
                if (!ReferenceEquals(_receiving, pool))
                    pool.WakeUnderLock();
            }
        }

        internal ValueTask ResumeAsync()
        {
            if (ReferenceEquals(_receiving, pool))
                return ValueTask.CompletedTask;
            _ = WaitAsync(Array.Empty<ReceiveEvent>(), TimeSpan.Zero);
            return new ValueTask(this, 0);
        }

        void IValueTaskSource.GetResult(short token) => _ = GetResult(token);

        internal void Wake()
        {
            lock (pool._gate)
            {
                if (Closing)
                    return;
                Woken = true;
                if (Waiting && Continuation is not null)
                    pool._resuming.Add(this);
                pool.WakeUnderLock();
            }
        }

        public ValueTask DisposeAsync()
        {
            lock (pool._gate)
                return new ValueTask(
                    _dispose ??= ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(
                        DisposeLaterAsync
                    )
                );
        }

        private async Task DisposeLaterAsync()
        {
            await Task.Yield();
            await DisposeCoreAsync().ConfigureAwait(false);
        }

        private async Task DisposeCoreAsync()
        {
            lock (pool._gate)
            {
                Closing = true;
                pool._dirty.Add(this);
                pool.WakeUnderLock();
            }
            try
            {
                await Removed.Task.ConfigureAwait(false);
            }
            finally
            {
                _wakeOnStop.Dispose();
                Task joined;
                lock (pool._gate)
                    joined = pool.ReleaseUnderLock();
                await joined.ConfigureAwait(false);
            }
        }
    }

    // These are the old persistent pump worker's reusable batches/results, now
    // attached to process resources. Async suspension transfers their ownership.
    internal sealed class ApplicationTurn : IDisposable
    {
        internal MeshReadyBatch Ready = new();
        internal MeshReceiveBatch Received = new();
        internal List<Task> Pending = [];

        internal void Transfer()
        {
            Ready = new MeshReadyBatch();
            Received = new MeshReceiveBatch();
            Pending = [];
        }

        public void Dispose()
        {
            Ready.Dispose();
            Received.Dispose();
        }
    }

    internal sealed class ApplicationSource(
        ZLinkProcessExecutionPool pool,
        Func<ApplicationTurn, ValueTask> turn,
        Action<Exception> report,
        int maximumConcurrentTurns
    ) : IAsyncDisposable
    {
        private readonly TaskCompletionSource _drained = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal readonly int MaximumConcurrentTurns = maximumConcurrentTurns;
        internal readonly LinkedListNode<ApplicationSource> ReadyEntry = new(null!);
        internal Task Completion => _drained.Task;
        private bool _closing;
        internal int Running;
        private int _retained;
        private Exception? _failure;
        private Task? _dispose;
        private CancellationTokenRegistration _stopRegistration;

        internal void StopOnCancellation(CancellationToken cancellationToken) =>
            _stopRegistration = cancellationToken.UnsafeRegister(
                static state => _ = ((ApplicationSource)state!).ObserveStopAsync(),
                this
            );

        private async Task ObserveStopAsync()
        {
            try
            {
                await DisposeAsync().ConfigureAwait(false);
            }
            catch (Exception error)
            {
                Report(error);
            }
        }

        internal void FailUnderLock(Exception failure)
        {
            _failure ??= failure;
            _closing = true;
            if (ReadyEntry.List is not null)
                pool._ready.Remove(ReadyEntry);
            CompleteDrainedUnderLock();
        }

        internal void Signal()
        {
            lock (pool._gate)
            {
                if (_closing)
                    return;
                if (ReadyEntry.List is null)
                {
                    ReadyEntry.Value = this;
                    pool._ready.AddLast(ReadyEntry);
                }
                if (!ReferenceEquals(_receiving, pool))
                {
                    Monitor.PulseAll(pool._gate);
                    pool.WakeUnderLock();
                }
            }
        }

        internal void Run(ApplicationTurn worker)
        {
            try
            {
                var result = turn(worker);
                if (result.IsCompleted)
                {
                    result.GetAwaiter().GetResult();
                    Complete();
                }
                else
                    _ = ObserveAsync(result);
            }
            catch (Exception error)
            {
                Report(error);
                Complete();
            }
        }

        internal void Retain(ValueTask result)
        {
            lock (pool._gate)
                _retained++;
            _ = ObserveRetainedAsync(result);
        }

        private async Task ObserveRetainedAsync(ValueTask result)
        {
            try
            {
                await result.ConfigureAwait(false);
            }
            catch (Exception error)
            {
                Report(error);
            }
            finally
            {
                lock (pool._gate)
                {
                    _retained--;
                    CompleteDrainedUnderLock();
                }
            }
        }

        private void Report(Exception error)
        {
            try
            {
                report(error);
            }
            catch (Exception failure)
            {
                lock (pool._gate)
                    _failure ??= ReferenceEquals(error, failure)
                        ? error
                        : new AggregateException(error, failure);
                ZLinkFrameworkDebugLog.TaskFailure("process-application-report", failure);
            }
        }

        private async Task ObserveAsync(ValueTask result)
        {
            try
            {
                await result.ConfigureAwait(false);
            }
            catch (Exception error)
            {
                Report(error);
            }
            finally
            {
                Complete();
            }
        }

        private void CompleteDrainedUnderLock()
        {
            if (!_closing || ReadyEntry.List is not null || Running != 0 || _retained != 0)
                return;
            if (_failure is { } error)
                _drained.TrySetException(error);
            else
                _drained.TrySetResult();
        }

        private void Complete()
        {
            lock (pool._gate)
            {
                Running--;
                CompleteDrainedUnderLock();
                Monitor.PulseAll(pool._gate);
                if (ReadyEntry.List is not null)
                    pool.WakeUnderLock();
            }
        }

        public ValueTask DisposeAsync()
        {
            lock (pool._gate)
            {
                if (_dispose is null)
                {
                    _closing = true;
                    CompleteDrainedUnderLock();
                    _dispose = ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(
                        DisposeLaterAsync
                    );
                }
                return new ValueTask(_dispose);
            }
        }

        private async Task DisposeLaterAsync()
        {
            await Task.Yield();
            await DisposeCoreAsync().ConfigureAwait(false);
        }

        private async Task DisposeCoreAsync()
        {
            try
            {
                await _drained.Task.ConfigureAwait(false);
            }
            finally
            {
                _stopRegistration.Dispose();
                Task joined;
                lock (pool._gate)
                {
                    pool._applications.Remove(this);
                    joined = pool.ReleaseUnderLock();
                }
                await joined.ConfigureAwait(false);
            }
        }
    }
}
