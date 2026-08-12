using System.Collections;

namespace Zlink.Framework.Runtime.Execution;

/// <summary>
/// Stores serial work without allocating while an item is appended.
/// </summary>
internal sealed class ZLinkSerialWorkQueue
    : IEnumerable<ZLinkSerialWorkItem>
{
    private ZLinkSerialWorkItem? _head;
    private ZLinkSerialWorkItem? _tail;
    private int _version;

    public int Count { get; private set; }

    public void Enqueue(ZLinkSerialWorkItem item)
    {
        ArgumentNullException.ThrowIfNull(item);
        EnsureConsistentState();
        if (item.Next is not null)
            throw new InvalidOperationException(
                "ZLink serial work is already attached to a queue.");

        var nextCount = checked(Count + 1);
        if (_tail is not null
            && !ReferenceEquals(_tail.Next, _tail))
            throw new InvalidOperationException(
                "ZLink serial work queue tail is corrupt.");

        if (_tail is null)
            _head = item;
        else
            _tail.Next = item;

        // A self-link marks the attached tail. Every attached item therefore
        // has a non-null link, including a one-item queue, so another queue
        // cannot attach the same item at the same time.
        item.Next = item;
        _tail = item;
        Count = nextCount;
        unchecked
        {
            _version++;
        }
    }

    public bool TryDequeue(out ZLinkSerialWorkItem item)
    {
        EnsureConsistentState();
        if (_head is null)
        {
            item = null!;
            return false;
        }

        item = _head;
        var link = item.Next
                   ?? throw new InvalidOperationException(
                       "ZLink serial work queue link is corrupt.");
        var next = ReferenceEquals(link, item) ? null : link;
        item.Next = null;
        _head = next;
        Count--;
        if (next is null)
            _tail = null;
        unchecked
        {
            _version++;
        }
        return true;
    }

    public void Clear()
    {
        EnsureConsistentState();
        if (_head is null) return;

        var current = _head;
        while (current is not null)
        {
            var link = current.Next
                       ?? throw new InvalidOperationException(
                           "ZLink serial work queue link is corrupt.");
            var next = ReferenceEquals(link, current) ? null : link;
            current.Next = null;
            current = next;
        }

        _head = null;
        _tail = null;
        Count = 0;
        unchecked
        {
            _version++;
        }
    }

    public Enumerator GetEnumerator() => new(this);

    IEnumerator<ZLinkSerialWorkItem>
        IEnumerable<ZLinkSerialWorkItem>.GetEnumerator() => GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    private void EnsureConsistentState()
    {
        if ((_head is null) != (_tail is null)
            || (Count == 0) != (_head is null))
            throw new InvalidOperationException(
                "ZLink serial work queue state is corrupt.");
    }

    internal struct Enumerator : IEnumerator<ZLinkSerialWorkItem>
    {
        private readonly ZLinkSerialWorkQueue _queue;
        private readonly int _version;
        private ZLinkSerialWorkItem? _next;
        private ZLinkSerialWorkItem? _current;

        internal Enumerator(ZLinkSerialWorkQueue queue)
        {
            _queue = queue;
            _version = queue._version;
            _next = queue._head;
            _current = null;
        }

        public ZLinkSerialWorkItem Current =>
            _current
            ?? throw new InvalidOperationException(
                "The ZLink serial work queue enumerator is not positioned on an item.");

        object IEnumerator.Current => Current;

        public bool MoveNext()
        {
            if (_version != _queue._version)
                throw new InvalidOperationException(
                    "The ZLink serial work queue changed during enumeration.");
            if (_next is null)
            {
                _current = null;
                return false;
            }

            _current = _next;
            var link = _current.Next
                       ?? throw new InvalidOperationException(
                           "ZLink serial work queue link is corrupt.");
            _next = ReferenceEquals(link, _current) ? null : link;
            return true;
        }

        public void Reset() => throw new NotSupportedException();

        public void Dispose() { }
    }
}
