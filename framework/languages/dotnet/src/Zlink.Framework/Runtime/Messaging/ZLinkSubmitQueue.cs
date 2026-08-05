namespace Zlink.Framework.Runtime.Messaging;

internal sealed class ZLinkSubmitQueue
{
    private readonly int _capacity;
    private readonly object _gate = new();
    private readonly Queue<PendingSubmit> _pending = new();
    private bool _disposed;

    public ZLinkSubmitQueue(int capacity)
    {
        _capacity = capacity > 0
            ? capacity
            : throw new ArgumentOutOfRangeException(nameof(capacity));
    }

    public bool TryEnqueue(PendingSubmit pending)
    {
        lock (_gate)
        {
            ThrowIfDisposed();
            if (_pending.Count >= _capacity) return false;

            _pending.Enqueue(pending);
            return true;
        }
    }

    public bool TryPeek(out PendingSubmit? pending)
    {
        lock (_gate)
        {
            pending = _pending.Count > 0 ? _pending.Peek() : null;
            return pending is not null;
        }
    }

    public int Count
    {
        get
        {
            lock (_gate) return _pending.Count;
        }
    }

    public bool TryDequeue(
        PendingSubmit expected,
        out PendingSubmit? pending)
    {
        lock (_gate)
        {
            if (_pending.Count > 0 && ReferenceEquals(_pending.Peek(), expected))
            {
                pending = _pending.Dequeue();
                return true;
            }
        }

        pending = null;
        return false;
    }

    public bool TryRemove(PendingSubmit expected, out PendingSubmit? pending)
    {
        pending = null;
        lock (_gate)
        {
            if (_pending.Count == 0)
            {
                return false;
            }

            var removed = false;
            var count = _pending.Count;
            for (var index = 0; index < count; index++)
            {
                var candidate = _pending.Dequeue();
                if (!removed && ReferenceEquals(candidate, expected))
                {
                    pending = candidate;
                    removed = true;
                    continue;
                }

                _pending.Enqueue(candidate);
            }

            if (removed) return true;
        }

        return false;
    }

    public void Complete()
    {
        lock (_gate) _disposed = true;
    }

    public IReadOnlyList<PendingSubmit> DrainAll()
    {
        PendingSubmit[] remaining;
        lock (_gate)
        {
            _disposed = true;
            remaining = _pending.ToArray();
            _pending.Clear();
        }

        return remaining;
    }

    private void ThrowIfDisposed()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(ZLinkAsyncSubmitter));
    }
}
