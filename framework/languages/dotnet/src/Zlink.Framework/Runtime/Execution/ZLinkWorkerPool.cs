namespace Zlink.Framework.Runtime.Execution;

/// <summary>
///     Single elastic bounded worker pool. Threads are spawned on demand up to
///     <see cref="MaxThreads" />, exit after <see cref="_idleTimeout" /> of
///     inactivity, and queued work is bounded by <see cref="_maxQueueLength" />.
///     A full queue fails the submit immediately; the pool never blocks the
///     submitting dispatcher and never runs work on the caller thread.
/// </summary>
internal sealed class ZLinkWorkerPool : IDisposable, IAsyncDisposable
{
    private readonly TimeSpan _idleTimeout;
    private readonly int _maxQueueLength;
    private readonly int _minThreads;
    private readonly Queue<WorkerItem> _directQueue = new();
    private readonly SemaphoreSlim _directWaiterSlots;
    private readonly Queue<WorkerItem> _queue = new();
    private readonly CancellationTokenSource _shutdownSource = new();
    private readonly object _sync = new();
    private readonly HashSet<Thread> _threads = [];
    private TaskCompletionSource _directCapacityChanged = NewCapacitySignal();
    private bool _disposed;
    private Task? _disposeTask;
    private int _idleThreads;
    private int _threadCount;

    public ZLinkWorkerPool(
        int minThreads,
        int maxThreads,
        TimeSpan idleTimeout,
        int maxQueueLength)
    {
        if (minThreads < 0) throw new ArgumentOutOfRangeException(nameof(minThreads));

        if (maxThreads < Math.Max(1, minThreads)) throw new ArgumentOutOfRangeException(nameof(maxThreads));

        if (idleTimeout <= TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(idleTimeout));

        if (maxQueueLength < 1) throw new ArgumentOutOfRangeException(nameof(maxQueueLength));

        _minThreads = minThreads;
        MaxThreads = maxThreads;
        _idleTimeout = idleTimeout;
        _maxQueueLength = maxQueueLength;
        _directWaiterSlots = new SemaphoreSlim(maxQueueLength, maxQueueLength);
    }

    public CancellationToken ShutdownToken => _shutdownSource.Token;

    public int MaxThreads { get; }

    internal int DirectAdmissionWaiterCount =>
        _maxQueueLength - _directWaiterSlots.CurrentCount;

    public int ThreadCount
    {
        get
        {
            lock (_sync)
            {
                return _threadCount;
            }
        }
    }

    public int QueueLength
    {
        get
        {
            lock (_sync)
            {
                return _directQueue.Count + _queue.Count;
            }
        }
    }

    public void Dispose()
    {
        DisposeAsync().AsTask().GetAwaiter().GetResult();
    }

    public ValueTask DisposeAsync()
    {
        TaskCompletionSource completion;
        Thread[] threads;
        WorkerItem[] abandoned = [];
        var cancel = false;
        lock (_sync)
        {
            if (_threads.Contains(Thread.CurrentThread))
                throw new InvalidOperationException("A worker cannot dispose its own pool.");
            if (_disposeTask is not null) return new ValueTask(_disposeTask);
            if (!_disposed)
            {
                _disposed = true;
                abandoned = _directQueue.Concat(_queue).ToArray();
                _directQueue.Clear();
                _queue.Clear();
                Monitor.PulseAll(_sync);
                SignalDirectCapacityChanged();
                cancel = true;
            }
            completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            _disposeTask = completion.Task;
            threads = _threads.ToArray();
        }

        var stopFailures = new ZLinkFailureCollector();
        foreach (var item in abandoned)
            if (item.CancelBeforeStart is { } cancelBeforeStart)
                stopFailures.Capture(cancelBeforeStart);
        if (cancel)
            stopFailures.Capture(_shutdownSource.Cancel);

        _ = CompleteStopAsync(threads, completion, stopFailures.BuildException());
        return new ValueTask(completion.Task);
    }

    public void RequestStop()
    {
        var cancel = false;
        WorkerItem[] abandoned = [];
        lock (_sync)
        {
            if (!_disposed)
            {
                _disposed = true;
                abandoned = _directQueue.Concat(_queue).ToArray();
                _directQueue.Clear();
                _queue.Clear();
                Monitor.PulseAll(_sync);
                SignalDirectCapacityChanged();
                cancel = true;
            }
        }

        var failures = new ZLinkFailureCollector();
        foreach (var item in abandoned)
            if (item.CancelBeforeStart is { } cancelBeforeStart)
                failures.Capture(cancelBeforeStart);
        if (cancel) failures.Capture(_shutdownSource.Cancel);
        failures.ThrowIfAny();
    }

    public ZLinkWorkerSubmitResult TrySubmit(
        Action<CancellationToken> work,
        Action? cancelBeforeStart = null)
    {
        lock (_sync)
        {
            if (_disposed) return ZLinkWorkerSubmitResult.Stopped;
            if (_queue.Count >= _maxQueueLength) return ZLinkWorkerSubmitResult.Full;

            _queue.Enqueue(new WorkerItem(work, cancelBeforeStart));
            if (_idleThreads > 0)
            {
                Monitor.Pulse(_sync);
            }
            else if (_threadCount < MaxThreads)
            {
                _threadCount++;
                StartWorkerThread();
            }

            return ZLinkWorkerSubmitResult.Accepted;
        }
    }

    internal ZLinkWorkerSubmitResult TrySubmitDirect(
        Action<CancellationToken> work,
        Action? cancelBeforeStart = null)
    {
        lock (_sync)
        {
            if (_disposed) return ZLinkWorkerSubmitResult.Stopped;

            var availableReservations = _idleThreads
                                        + (MaxThreads - _threadCount)
                                        - _directQueue.Count;
            if (availableReservations <= 0) return ZLinkWorkerSubmitResult.Full;

            var reservedIdleThread = _idleThreads > _directQueue.Count;
            _directQueue.Enqueue(new WorkerItem(work, cancelBeforeStart));
            if (reservedIdleThread)
            {
                Monitor.Pulse(_sync);
            }
            else
            {
                _threadCount++;
                StartWorkerThread();
            }

            return ZLinkWorkerSubmitResult.Accepted;
        }
    }

    internal async ValueTask<ZLinkWorkerSubmitResult> SubmitDirectAsync(
        Action<CancellationToken> work,
        Action? cancelBeforeStart,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        if (timeout <= TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(timeout));
        var deadline = DateTimeOffset.UtcNow.Add(timeout);

        while (true)
        {
            Task capacityChanged;
            lock (_sync)
            {
                if (_disposed) return ZLinkWorkerSubmitResult.Stopped;
                if (deadline <= DateTimeOffset.UtcNow) return ZLinkWorkerSubmitResult.Full;

                var availableReservations = _idleThreads
                                            + (MaxThreads - _threadCount)
                                            - _directQueue.Count;
                if (availableReservations > 0)
                {
                    var reservedIdleThread = _idleThreads > _directQueue.Count;
                    _directQueue.Enqueue(new WorkerItem(work, cancelBeforeStart));
                    if (reservedIdleThread)
                    {
                        Monitor.Pulse(_sync);
                    }
                    else
                    {
                        _threadCount++;
                        StartWorkerThread();
                    }

                    return ZLinkWorkerSubmitResult.Accepted;
                }

                capacityChanged = _directCapacityChanged.Task;
            }

            var remaining = deadline - DateTimeOffset.UtcNow;
            if (remaining <= TimeSpan.Zero) return ZLinkWorkerSubmitResult.Full;
            if (!_directWaiterSlots.Wait(0)) return ZLinkWorkerSubmitResult.Full;
            try
            {
                await capacityChanged.WaitAsync(remaining, cancellationToken).ConfigureAwait(false);
            }
            catch (TimeoutException)
            {
                return ZLinkWorkerSubmitResult.Full;
            }
            finally
            {
                _directWaiterSlots.Release();
            }
        }
    }

    private void StartWorkerThread()
    {
        var thread = new Thread(WorkerLoop)
        {
            IsBackground = true,
            Name = "zlink-worker"
        };
        _threads.Add(thread);
        thread.Start();
    }

    private void WorkerLoop()
    {
        try
        {
            while (true)
            {
                WorkerItem item;
                lock (_sync)
                {
                    while (_directQueue.Count == 0 && _queue.Count == 0)
                    {
                        if (_disposed) return;

                        _idleThreads++;
                        var signaled = Monitor.Wait(_sync, _idleTimeout);
                        _idleThreads--;
                        if (!signaled
                            && _directQueue.Count == 0
                            && _queue.Count == 0
                            && _threadCount > _minThreads)
                            return;
                    }

                    item = _directQueue.Count > 0
                        ? _directQueue.Dequeue()
                        : _queue.Dequeue();
                }

                try
                {
                    item.Run(_shutdownSource.Token);
                }
                catch
                {
                    // Worker call wrappers convert their own failures; a throwing
                    // wrapper must never take the pool thread down.
                }
                finally
                {
                    lock (_sync) SignalDirectCapacityChanged();
                }
            }
        }
        finally
        {
            lock (_sync)
            {
                _threads.Remove(Thread.CurrentThread);
                _threadCount--;
                Monitor.PulseAll(_sync);
                SignalDirectCapacityChanged();
            }
        }
    }

    private sealed record WorkerItem(
        Action<CancellationToken> Run,
        Action? CancelBeforeStart);

    private void SignalDirectCapacityChanged()
    {
        var signal = _directCapacityChanged;
        _directCapacityChanged = NewCapacitySignal();
        signal.TrySetResult();
    }

    private static TaskCompletionSource NewCapacitySignal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private async Task CompleteStopAsync(
        IReadOnlyList<Thread> threads,
        TaskCompletionSource completion,
        Exception? cancellationFailure)
    {
        var failures = new ZLinkFailureCollector(cancellationFailure);
        await failures.CaptureAsync(
                () => new ValueTask(Task.WhenAll(
                    threads.Select(static thread => Task.Run(thread.Join)))))
            .ConfigureAwait(false);
        failures.Capture(_shutdownSource.Dispose);
        var failure = failures.BuildException();
        if (failure is null) completion.TrySetResult();
        else completion.TrySetException(failure);
    }
}

internal enum ZLinkWorkerSubmitResult
{
    Accepted,
    Full,
    Stopped
}
