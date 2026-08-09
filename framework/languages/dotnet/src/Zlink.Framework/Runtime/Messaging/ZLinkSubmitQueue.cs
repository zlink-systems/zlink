namespace Zlink.Framework.Runtime.Messaging;

internal sealed class ZLinkSubmitQueue
{
    private readonly int _capacity;
    private readonly object _gate = new();
    // Linked list + node map keep TryRemove O(1). The previous Queue<T>
    // implementation rebuilt the entire queue per removal, which made a
    // timeout storm under backpressure O(n^2) while holding _gate.
    private readonly LinkedList<PendingSubmit> _pending = new();
    private readonly Dictionary<PendingSubmit, LinkedListNode<PendingSubmit>> _nodes =
        new(ReferenceEqualityComparer.Instance);
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

            _nodes.Add(pending, _pending.AddLast(pending));
            return true;
        }
    }

    public bool TryPeek(out PendingSubmit? pending)
    {
        lock (_gate)
        {
            pending = _pending.First?.Value;
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
            if (_pending.First is { } head
                && ReferenceEquals(head.Value, expected))
            {
                _pending.RemoveFirst();
                _nodes.Remove(head.Value);
                pending = head.Value;
                return true;
            }
        }

        pending = null;
        return false;
    }

    public bool TryRemove(PendingSubmit expected, out PendingSubmit? pending)
    {
        lock (_gate)
        {
            if (_nodes.Remove(expected, out var node))
            {
                _pending.Remove(node);
                pending = node.Value;
                return true;
            }
        }

        pending = null;
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
            remaining = new PendingSubmit[_pending.Count];
            _pending.CopyTo(remaining, 0);
            _pending.Clear();
            _nodes.Clear();
        }

        return remaining;
    }

    private void ThrowIfDisposed()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(ZLinkAsyncSubmitter));
    }
}
