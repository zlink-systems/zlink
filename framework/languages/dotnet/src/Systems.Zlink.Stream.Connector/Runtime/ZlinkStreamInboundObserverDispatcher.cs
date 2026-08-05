using System.Collections.Concurrent;

namespace Systems.Zlink.Stream.Connector.Runtime;

internal sealed class ZlinkStreamInboundObserverDispatcher : IDisposable
{
    private readonly ZlinkStreamConnectorCallbacks _callbacks;
    private readonly object _gate = new();
    private readonly int _maxNotifications;
    private readonly int _maxPayloadPreviewBytes;
    private readonly ConcurrentQueue<ZlinkStreamInboundObservation> _queue = new();
    private readonly SemaphoreSlim _signal = new(0);
    private readonly ZlinkStreamTaskRunner _taskRunner;
    private int _drainStarted;
    private int _dropReportPending;
    private ObserverRegistration? _observers;
    private int _queuedCount;
    private int _disposed;

    public ZlinkStreamInboundObserverDispatcher(
        ZlinkStreamTaskRunner taskRunner,
        ZlinkStreamConnectorCallbacks callbacks,
        int maxNotifications,
        int maxPayloadPreviewBytes)
    {
        _taskRunner = taskRunner;
        _callbacks = callbacks;
        _maxNotifications = maxNotifications;
        _maxPayloadPreviewBytes = maxPayloadPreviewBytes;
    }

    public IDisposable Add(Func<ZlinkStreamInboundObservation, CancellationToken, ValueTask> observer)
    {
        ArgumentNullException.ThrowIfNull(observer);
        ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
        StartDrain();

        var registration = new ObserverRegistration(this, observer);
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(Volatile.Read(ref _disposed) != 0, this);
            registration.Next = _observers;
            _observers = registration;
        }

        return registration;
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0) return;

        lock (_gate)
        {
            _observers = null;
        }

        while (_queue.TryDequeue(out _))
        {
        }

        Volatile.Write(ref _queuedCount, 0);
        _signal.Dispose();
    }

    public void Enqueue(ZlinkStreamHeader header, ReadOnlyMemory<byte> wirePayload)
    {
        if (!HasObservers()) return;

        var count = Interlocked.Increment(ref _queuedCount);
        if (count > _maxNotifications)
        {
            Interlocked.Decrement(ref _queuedCount);
            ReportDropped();
            return;
        }

        var observation = CreateObservation(header, wirePayload);
        _queue.Enqueue(observation);
        _signal.Release();
    }

    private ZlinkStreamInboundObservation CreateObservation(
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> wirePayload)
    {
        var previewLength = Math.Min(_maxPayloadPreviewBytes, wirePayload.Length);
        var preview = previewLength == 0
            ? ReadOnlyMemory<byte>.Empty
            : wirePayload[..previewLength].ToArray();

        return new ZlinkStreamInboundObservation(
            header.Kind,
            header.Name,
            header.Codec,
            header.RequestSeq?.Value,
            header.Metadata,
            wirePayload.Length,
            header.Flags.HasFlag(ZlinkStreamHeaderFlags.PayloadCompressed),
            DateTimeOffset.UtcNow,
            preview);
    }

    private bool HasObservers()
    {
        lock (_gate)
        {
            return _observers is not null;
        }
    }

    private ObserverRegistration[] SnapshotObservers()
    {
        lock (_gate)
        {
            var count = 0;
            for (var current = _observers; current is not null; current = current.Next) count++;

            var snapshot = new ObserverRegistration[count];
            var index = 0;
            for (var current = _observers; current is not null; current = current.Next) snapshot[index++] = current;

            return snapshot;
        }
    }

    private async ValueTask DrainAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            await _signal.WaitAsync(cancellationToken).ConfigureAwait(false);
            while (_queue.TryDequeue(out var observation))
            {
                Interlocked.Decrement(ref _queuedCount);
                foreach (var observer in SnapshotObservers())
                {
                    if (observer.IsDisposed) continue;

                    try
                    {
                        using var callback = _callbacks.EnterCallback();
                        await observer.InvokeAsync(observation, cancellationToken).ConfigureAwait(false);
                    }
                    catch (Exception ex)
                    {
                        await ReportObserverFailedAsync(ex, cancellationToken).ConfigureAwait(false);
                    }
                }
            }
        }
    }

    private void StartDrain()
    {
        if (Interlocked.Exchange(ref _drainStarted, 1) != 0) return;

        try
        {
            _taskRunner.RunDetached(DrainAsync);
        }
        catch
        {
            Volatile.Write(ref _drainStarted, 0);
            throw;
        }
    }

    private void ReportDropped()
    {
        if (Interlocked.Exchange(ref _dropReportPending, 1) != 0) return;

        _taskRunner.RunDetached(
            async token =>
            {
                try
                {
                    await _callbacks.PublishErrorAsync(new ZlinkStreamError(
                            ZlinkStreamErrorCode.ObserverDropped,
                            "Inbound observer notification was dropped because the observer queue is full."),
                        token).ConfigureAwait(false);
                }
                finally
                {
                    Volatile.Write(ref _dropReportPending, 0);
                }
            });
    }

    private async ValueTask ReportObserverFailedAsync(Exception exception, CancellationToken cancellationToken)
    {
        try
        {
            await _callbacks.PublishErrorAsync(new ZlinkStreamError(
                    ZlinkStreamErrorCode.ObserverFailed,
                    "Inbound observer callback failed.",
                    exception),
                cancellationToken).ConfigureAwait(false);
        }
        catch
        {
        }
    }

    private void Remove(ObserverRegistration registration)
    {
        lock (_gate)
        {
            ObserverRegistration? previous = null;
            var current = _observers;
            while (current is not null)
            {
                if (ReferenceEquals(current, registration))
                {
                    if (previous is null)
                        _observers = current.Next;
                    else
                        previous.Next = current.Next;

                    break;
                }

                previous = current;
                current = current.Next;
            }
        }
    }

    private sealed class ObserverRegistration(
        ZlinkStreamInboundObserverDispatcher owner,
        Func<ZlinkStreamInboundObservation, CancellationToken, ValueTask> observer) : IDisposable
    {
        private int _disposed;

        public ObserverRegistration? Next { get; set; }

        public bool IsDisposed => Volatile.Read(ref _disposed) != 0;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0) owner.Remove(this);
        }

        public ValueTask InvokeAsync(
            ZlinkStreamInboundObservation observation,
            CancellationToken cancellationToken)
        {
            return observer(observation, cancellationToken);
        }
    }
}
