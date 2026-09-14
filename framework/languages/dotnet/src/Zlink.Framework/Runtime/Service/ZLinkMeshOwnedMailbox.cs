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
internal sealed class ZLinkMeshNodeOwnedMailbox
{
    private readonly Action<ulong> _onRecordEnqueued;
    private readonly Action<ulong> _onRecordDequeued;
    private readonly Action<ZLinkMeshNodeOwnedMailbox>? _onReady;
    private readonly Queue<ZLinkMeshQueuedRecord> _records = new();
    private readonly ZLinkStateLane _lane = new();
    private ulong _pendingBytes;
    private int _applicationAdmissionRecords;
    // An active claim owns only the FIFO prefix present when it was granted.
    // Zero still holds that claim until Release; -1 means no active claim.
    private int _claimedRecordCount = -1;

    internal ZLinkMeshNodeOwnedMailbox(Action<ulong> onRecordEnqueued,
        Action<ulong> onRecordDequeued, Action<ZLinkMeshNodeOwnedMailbox>? onReady = null)
    {
        _onRecordEnqueued = onRecordEnqueued;
        _onRecordDequeued = onRecordDequeued;
        _onReady = onReady;
    }

    internal bool HasRecords => AwaitStateLane(
        _lane.RunAsync(this, static mailbox => mailbox._records.Count != 0));

    internal int Count => AwaitStateLane(_lane.RunAsync(this, static mailbox => mailbox._records.Count));

    internal bool TryEnqueue(ZLinkMeshQueuedRecord record)
    {
        return AwaitStateLane(_lane.RunAsync((Mailbox: this, Record: record), static state =>
        {
            var mailbox = state.Mailbox;
            var record = state.Record;
            var pendingBytes = record.PendingBytes;
            if (pendingBytes == ulong.MaxValue)
                return false;
            mailbox._records.Enqueue(record);
            if (record.HasApplicationJobAdmission)
                mailbox._applicationAdmissionRecords++;
            mailbox._pendingBytes = checked(mailbox._pendingBytes + pendingBytes);

            // Publish accounting before another mailbox turn can dequeue
            // this record. Readiness reads this existing aggregate directly.
            mailbox._onRecordEnqueued(pendingBytes);
            // Only the empty -> ready transition posts an owner. An active
            // claim posts its residue when released, not on every arrival.
            if (mailbox._records.Count == 1 && mailbox._claimedRecordCount < 0)
                mailbox._onReady?.Invoke(mailbox);
            return true;
        }));
    }

    internal bool TryClaim(bool requireApplicationAdmission, bool claim, out int count,
        out bool applicationAdmissionReserved)
    {
        var result = AwaitStateLane(_lane.RunAsync(
            (Mailbox: this, RequireAdmission: requireApplicationAdmission, Claim: claim), static state =>
        {
            var mailbox = state.Mailbox;
            var admitted = mailbox._applicationAdmissionRecords == mailbox._records.Count;
            if (mailbox._claimedRecordCount >= 0 || mailbox._records.Count == 0)
                return (Ready: false, Count: 0, Admitted: false);
            if (state.RequireAdmission && !admitted)
                return (Ready: false, Count: mailbox._records.Count, Admitted: false);
            if (state.Claim)
                mailbox._claimedRecordCount = mailbox._records.Count;
            return (Ready: true, Count: mailbox._records.Count, Admitted: admitted);
        }));
        count = result.Count;
        applicationAdmissionReserved = result.Admitted;
        return result.Ready;
    }

    internal bool Drain(MeshReceiveBatch batch, int maximumRecords)
    {
        return AwaitStateLane(_lane.RunAsync((Mailbox: this, Batch: batch, Maximum: maximumRecords), static state =>
        {
            var mailbox = state.Mailbox;
            var batch = state.Batch;
            var count = 0;
            var limit = Math.Min(state.Maximum, mailbox._claimedRecordCount);
            while (count < limit && mailbox._records.Count != 0)
            {
                var candidate = mailbox._records.Peek();
                if (!batch.CanAdd(checked((long)candidate.PayloadBytes)))
                    break;
                var record = mailbox._records.Dequeue();
                mailbox._claimedRecordCount--;
                if (record.HasApplicationJobAdmission)
                    mailbox._applicationAdmissionRecords--;
                mailbox._pendingBytes -= record.PendingBytes;
                mailbox._onRecordDequeued(record.PendingBytes);
                batch.Add(record.Record, record.TakeParts(), record.TakePayloadOwner());
                count++;
            }
            return count != 0;
        }));
    }

    internal bool Release()
    {
        return AwaitStateLane(_lane.RunAsync(this, static mailbox =>
        {
            mailbox._claimedRecordCount = -1;
            if (mailbox._records.Count != 0)
                mailbox._onReady?.Invoke(mailbox);
            return mailbox._records.Count != 0;
        }));
    }

    internal void Dispose()
    {
        List<ZLinkMeshQueuedRecord> removed = [];
        AwaitStateLane(_lane.RunAsync((Mailbox: this, Removed: removed), static state =>
        {
            var mailbox = state.Mailbox;
            while (mailbox._records.Count != 0)
            {
                var record = mailbox._records.Dequeue();
                state.Removed.Add(record);
                mailbox._onRecordDequeued(record.PendingBytes);
            }
            mailbox._pendingBytes = 0;
            mailbox._applicationAdmissionRecords = 0;
            mailbox._claimedRecordCount = -1;
            return true;
        }));

        foreach (var record in removed)
            record.Dispose();
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
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
    private readonly ulong _pendingBytes;
    internal MeshReceiveRecord Record { get; }

    internal ZLinkMeshQueuedRecord(
        MeshReceiveRecord record,
        IReadOnlyList<Message> parts,
        ulong? applicationPayloadBytes = null,
        IDisposable? payloadOwner = null)
    {
        _parts = parts;
        var payloadBytes = applicationPayloadBytes
                        ?? record.ApplicationPayloadBytes
                        ?? (parts is IZLinkApplicationPayloadSized sized
                            ? sized.ApplicationPayloadBytes
                            : throw new InvalidOperationException(
                                "Queued mesh records must carry application payload bytes."));
        record.ApplicationPayloadBytes = payloadBytes;
        Record = record;
        _payloadOwner = payloadOwner;
        _pendingBytes = ComputePendingBytes(
            payloadBytes,
            (ulong)(record.ApplicationMetadata?.Length ?? 0));
    }
    internal ulong PayloadBytes => Record.ApplicationPayloadBytes!.Value;

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
