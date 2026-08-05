using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Service;

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

    internal bool IsClaimed
    {
        get
        {
            lock (_gate)
                return _claimed;
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

    internal bool TryClaim(ZLinkInboundDispatchBudget? inboundDispatchBudget)
    {
        ZLinkMeshQueuedRecord? candidate;
        lock (_gate)
        {
            if (_claimed || _records.Count == 0)
                return false;
            _claimed = true;
            candidate = _records.Peek();
            if (!candidate.Record.RequiresApplicationDispatchLease
                || candidate.Record.InboundDispatchLease is not null
                || inboundDispatchBudget is null)
                return true;
        }

        if (!inboundDispatchBudget.TryTrack(
                candidate.PayloadBytes,
                out var lease))
        {
            lock (_gate)
            {
                if (_records.Count != 0
                    && ReferenceEquals(_records.Peek(), candidate))
                    _claimed = false;
            }
            return false;
        }

        lock (_gate)
        {
            if (_records.Count == 0
                || !ReferenceEquals(_records.Peek(), candidate))
            {
                _claimed = false;
                lease!.Dispose();
                return false;
            }

            candidate.AttachLease(lease!);
            return true;
        }
    }

    internal bool TryDequeue(
        MeshReceiveBatch batch,
        ZLinkInboundDispatchBudget? inboundDispatchBudget,
        out ZLinkMeshQueuedRecord record)
    {
        ZLinkMeshQueuedRecord candidate;
        ZLinkInboundDispatchLease? lease = null;
        ulong pendingBytes = 0;
        var requiresLease = false;
        record = null!;
        lock (_gate)
        {
            if (_records.Count == 0)
            {
                record = null!;
                return false;
            }
            candidate = _records.Peek();
            if (!batch.CanAdd(checked((long)candidate.PayloadBytes)))
            {
                record = null!;
                return false;
            }

            requiresLease = candidate.Record.RequiresApplicationDispatchLease
                && candidate.Record.InboundDispatchLease is null
                && inboundDispatchBudget is not null;
            if (!requiresLease)
            {
                record = _records.Dequeue();
                pendingBytes = record.PendingBytes;
                _pendingBytes -= pendingBytes;
            }
        }

        if (!requiresLease)
        {
            onRecordDequeued(pendingBytes);
            return true;
        }

        // A mailbox can contain more than one application record. Acquire a
        // lease for the record at the dequeue boundary so every record in a
        // drained batch remains accounted for. If HWM admission is closed,
        // leave the head in place and let the ready signal retry it later.
        if (!inboundDispatchBudget!.TryTrack(
                candidate.PayloadBytes,
                out lease))
        {
            record = null!;
            return false;
        }

        lock (_gate)
        {
            if (_records.Count == 0
                || !ReferenceEquals(_records.Peek(), candidate))
            {
                lease!.Dispose();
                record = null!;
                return false;
            }

            candidate.AttachLease(lease!);
            record = _records.Dequeue();
            pendingBytes = record.PendingBytes;
            _pendingBytes -= pendingBytes;
            lease = null;
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
    private readonly ulong _payloadBytes;
    private readonly ulong _pendingBytes;
    internal MeshReceiveRecord Record { get; private set; }

    internal ZLinkMeshQueuedRecord(
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts,
        ulong? applicationPayloadBytes = null)
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
        _pendingBytes = ComputePendingBytes(
            _payloadBytes,
            (ulong)(record.ApplicationMetadata?.Length ?? 0));
    }
    internal ulong PayloadBytes => _payloadBytes;

    internal ulong PendingBytes => _pendingBytes;

    internal IReadOnlyList<Message> TakeParts() =>
        Interlocked.Exchange(ref _parts, null) ?? Array.Empty<Message>();

    internal void AttachLease(ZLinkInboundDispatchLease lease)
    {
        var current = Record;
        current.InboundDispatchLease = lease;
        Record = current;
    }

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
        Record.InboundDispatchLease?.Dispose();
    }
}
