using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Service;

// A single binding envelope can fan out to more than one framework mailbox.
// Each mailbox receives a terminal owner; the underlying envelope is released
// only after the final consumer.
internal sealed class ZLinkSharedEnvelopeOwner : IDisposable
{
    private IDisposable? _owner;
    private int _references = 1;

    internal ZLinkSharedEnvelopeOwner(IDisposable owner)
    {
        _owner = owner ?? throw new ArgumentNullException(nameof(owner));
    }

    internal IDisposable Retain()
    {
        while (true)
        {
            var current = Volatile.Read(ref _references);
            if (current == 0)
                throw new ObjectDisposedException(nameof(ZLinkSharedEnvelopeOwner));
            if (Interlocked.CompareExchange(
                    ref _references,
                    checked(current + 1),
                    current) == current)
                return new Lease(this);
        }
    }

    public void Dispose()
    {
        if (Interlocked.Decrement(ref _references) != 0)
            return;
        Interlocked.Exchange(ref _owner, null)?.Dispose();
    }

    private sealed class Lease(ZLinkSharedEnvelopeOwner owner) : IDisposable
    {
        private ZLinkSharedEnvelopeOwner? _owner = owner;

        public void Dispose() => Interlocked.Exchange(ref _owner, null)?.Dispose();
    }
}

/// <summary>
/// Bounded mailbox for records owned by one node, Spot or Actor.
/// </summary>
internal sealed class ZLinkMeshNodeOwnedMailbox(
    Action<ulong> onRecordEnqueued,
    Action<ulong> onRecordDequeued)
{
    private readonly Queue<ZLinkMeshQueuedRecord> _records = new();
    private readonly object _gate = new();
    private ulong _pendingBytes;
    private bool _claimed;

    internal bool HasRecords
    {
        get
        {
            lock (_gate)
                return _records.Count != 0;
        }
    }

    internal int Count
    {
        get
        {
            lock (_gate)
                return _records.Count;
        }
    }

    internal bool TryEnqueue(
        ZLinkMeshQueuedRecord record,
        ulong messageBudget,
        ulong byteBudget)
    {
        var pendingBytes = record.PendingBytes;
        lock (_gate)
        {
            if (pendingBytes == ulong.MaxValue)
                return false;
            if ((ulong)_records.Count >= messageBudget
                || pendingBytes > byteBudget - Math.Min(
                    _pendingBytes,
                    byteBudget))
                return false;
            _records.Enqueue(record);
            _pendingBytes = checked(_pendingBytes + pendingBytes);

            // Enqueue accounting must precede the lock release. A dequeue can
            // otherwise publish its decrement before the corresponding
            // increment and expose a transient negative global count.
            onRecordEnqueued(pendingBytes);
        }
        return true;
    }

    internal bool TryClaim()
    {
        lock (_gate)
        {
            if (_claimed || _records.Count == 0)
                return false;
            _claimed = true;
            return true;
        }
    }

    internal bool TryDequeue(
        MeshReceiveBatch batch,
        out ZLinkMeshQueuedRecord record)
    {
        ulong pendingBytes = 0;
        record = null!;
        lock (_gate)
        {
            if (_records.Count == 0)
            {
                record = null!;
                return false;
            }
            var candidate = _records.Peek();
            if (!batch.CanAdd(checked((long)candidate.PayloadBytes)))
            {
                record = null!;
                return false;
            }

            record = _records.Dequeue();
            pendingBytes = record.PendingBytes;
            _pendingBytes -= pendingBytes;
        }

        onRecordDequeued(pendingBytes);
        return true;
    }

    internal void Release()
    {
        lock (_gate)
            _claimed = false;
    }

    internal void Dispose()
    {
        List<(ZLinkMeshQueuedRecord Record, ulong PendingBytes)> removed = [];
        lock (_gate)
        {
            while (_records.Count != 0)
            {
                var record = _records.Dequeue();
                removed.Add((record, record.PendingBytes));
            }
            _pendingBytes = 0;
            _claimed = false;
        }

        foreach (var (record, pendingBytes) in removed)
        {
            onRecordDequeued(pendingBytes);
            record.Dispose();
        }
    }
}

internal sealed class ZLinkMeshQueuedRecord : IDisposable
{
    // The mailbox contract accounts for the retained payload, application
    // metadata, and a fixed envelope/queue-node cost. The inbound application
    // HWM uses PayloadBytes separately because the wire receive contract counts
    // payload bytes only.
    internal const ulong FixedRecordBytes = 256;
    private IReadOnlyList<Message>? _parts;
    private IDisposable? _payloadOwner;
    private readonly ulong _payloadBytes;
    private readonly ulong _pendingBytes;
    internal MeshReceiveRecord Record { get; private set; }

    internal ZLinkMeshQueuedRecord(
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts,
        ulong? applicationPayloadBytes = null,
        IDisposable? payloadOwner = null)
    {
        _parts = parts;
        _payloadBytes = applicationPayloadBytes
                        ?? record.ApplicationPayloadBytes
                        ?? (parts is IZLinkApplicationPayloadSized sized
                            ? sized.ApplicationPayloadBytes
                            : throw new InvalidOperationException(
                                "Queued mesh records must carry application payload bytes."));
        record.ApplicationPayloadBytes = _payloadBytes;
        Record = record;
        _payloadOwner = payloadOwner;
        _pendingBytes = ComputePendingBytes(
            _payloadBytes,
            (ulong)(record.ApplicationMetadata?.Length ?? 0));
    }
    internal ulong PayloadBytes => _payloadBytes;

    internal ulong PendingBytes => _pendingBytes;

    internal IReadOnlyList<Message> TakeParts() =>
        Interlocked.Exchange(ref _parts, null) ?? Array.Empty<Message>();

    internal IDisposable? TakePayloadOwner() =>
        Interlocked.Exchange(ref _payloadOwner, null);

    private static ulong ComputePendingBytes(ulong payload, ulong metadata)
    {
        if (payload > ulong.MaxValue - FixedRecordBytes)
            return ulong.MaxValue;
        var total = payload + FixedRecordBytes;
        return metadata > ulong.MaxValue - total
            ? ulong.MaxValue
            : total + metadata;
    }

    public void Dispose()
    {
        var owned = Interlocked.Exchange(ref _parts, null);
        if (owned is not null)
            foreach (var part in owned)
                part.Dispose();
            Interlocked.Exchange(ref _payloadOwner, null)?.Dispose();
    }
}
