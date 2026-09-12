using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
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
    private readonly ZLinkStateLane _lane = new();
    private ulong _pendingBytes;
    private int _applicationAdmissionRecords;
    // An active claim owns only the FIFO prefix present when it was granted.
    // Zero still holds that claim until Release; -1 means no active claim.
    private int _claimedRecordCount = -1;

    internal bool HasRecords => AwaitStateLane(
        _lane.RunAsync(() => _records.Count != 0));

    internal int Count => AwaitStateLane(_lane.RunAsync(() => _records.Count));

    internal bool TryEnqueue(ZLinkMeshQueuedRecord record)
    {
        var pendingBytes = record.PendingBytes;
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (pendingBytes == ulong.MaxValue)
                return false;
            _records.Enqueue(record);
            if (record.HasApplicationJobAdmission)
                _applicationAdmissionRecords++;
            _pendingBytes = checked(_pendingBytes + pendingBytes);

            // Publish accounting before another mailbox turn can dequeue
            // this record. Readiness reads this existing aggregate directly.
            onRecordEnqueued(pendingBytes);
            return true;
        }));
    }

    internal bool TryClaim(bool requireApplicationAdmission, bool claim, out int count,
        out bool applicationAdmissionReserved)
    {
        var result = AwaitStateLane(_lane.RunAsync(() =>
        {
            var admitted = _applicationAdmissionRecords == _records.Count;
            if (_claimedRecordCount >= 0 || _records.Count == 0
                || (requireApplicationAdmission && !admitted))
                return (Ready: false, Count: 0, Admitted: false);
            if (claim)
                _claimedRecordCount = _records.Count;
            return (Ready: true, Count: _records.Count, Admitted: admitted);
        }));
        count = result.Count;
        applicationAdmissionReserved = result.Admitted;
        return result.Ready;
    }

    internal bool Drain(MeshReceiveBatch batch, int maximumRecords)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            var count = 0;
            var limit = Math.Min(maximumRecords, _claimedRecordCount);
            while (count < limit && _records.Count != 0)
            {
                var candidate = _records.Peek();
                if (!batch.CanAdd(checked((long)candidate.PayloadBytes)))
                    break;
                var record = _records.Dequeue();
                _claimedRecordCount--;
                if (record.HasApplicationJobAdmission)
                    _applicationAdmissionRecords--;
                _pendingBytes -= record.PendingBytes;
                onRecordDequeued(record.PendingBytes);
                batch.Add(record.Record, record.TakeParts(), record.TakePayloadOwner());
                count++;
            }
            return count != 0;
        }));
    }

    internal bool Release()
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            _claimedRecordCount = -1;
            return _records.Count != 0;
        }));
    }

    internal void Dispose()
    {
        List<ZLinkMeshQueuedRecord> removed = [];
        AwaitStateLane(_lane.RunAsync(() =>
        {
            while (_records.Count != 0)
            {
                var record = _records.Dequeue();
                removed.Add(record);
                onRecordDequeued(record.PendingBytes);
            }
            _pendingBytes = 0;
            _applicationAdmissionRecords = 0;
            _claimedRecordCount = -1;
        }));

        foreach (var record in removed)
            record.Dispose();
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();
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

    internal bool HasApplicationJobAdmission =>
        _payloadOwner is ZLinkApplicationJobQueueRecordOwner;

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
