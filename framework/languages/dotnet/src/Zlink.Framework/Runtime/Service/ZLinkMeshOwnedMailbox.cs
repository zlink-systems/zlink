using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

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
            if (
                Interlocked.CompareExchange(ref _references, checked(current + 1), current)
                == current
            )
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
/// Mailbox for records owned by one Node, ChannelName, Spot or Actor.
/// </summary>
internal sealed class ZLinkMeshNodeOwnedMailbox(
    Action<ulong> onRecordEnqueued,
    Action<ulong> onRecordDequeued,
    Action? onReady = null,
    Action<bool>? onReadinessChanged = null,
    Action? onReadyChanged = null
)
{
    private readonly Queue<ZLinkMeshQueuedRecord> _records = new();
    private readonly ZLinkStateLane _lane = new();

    // Only gated owners retain a FIFO prefix until release.
    private int _claimedRecordCount = -1;

    internal MeshClaim CreateClaim(MeshOwnerKind ownerKind)
    {
        // ChannelName mailboxes also use Node, with the channel as their identity.
        if (ownerKind == MeshOwnerKind.Node)
            return new(singleReceive: true) { Receiver = TakeRecordAsync, Releaser = Release };
        return new()
        {
            Receiver = (batch, _) =>
                ClaimAndDrainAsync(
                    batch,
                    Math.Min(ZLinkReceiveBatchBudget.MaximumRecords, batch.MaximumRecords)
                ),
            Releaser = Release,
        };
    }

    private async ValueTask<bool> TakeRecordAsync(MeshReceiveBatch batch, RecvFlags flags)
    {
        var result = await _lane
            .RunAsync(
                static state =>
                {
                    var received = state.Mailbox.DrainOnLane(state.Batch, 1);
                    var ready = state.Mailbox.UpdateReadinessOnLane();
                    return (Received: received != 0, Ready: ready);
                },
                (Mailbox: this, Batch: batch)
            )
            .ConfigureAwait(false);
        // Publish the successor after the mailbox state turn has ended.
        if (result.Ready)
            onReadyChanged?.Invoke();
        return result.Received;
    }

    internal bool HasRecords => AwaitStateLane(_lane.RunAsync(() => _records.Count != 0));

    internal int Count => AwaitStateLane(_lane.RunAsync(() => _records.Count));

    // The admission decision depends only on the record, so the producer does
    // not wait for the mailbox turn. The record joins the mailbox FIFO before
    // this returns; readiness is published in that turn after the record is visible,
    // so a readiness signal can never precede the record it announces.
    internal bool TryEnqueue(ZLinkMeshQueuedRecord record)
    {
        if (record.PendingBytes == ulong.MaxValue)
            return false;
        return _lane.TryPost(
            static state => state.Mailbox.EnqueueOnLane(state.Record),
            (Mailbox: this, Record: record)
        );
    }

    private void EnqueueOnLane(ZLinkMeshQueuedRecord record)
    {
        var pendingBytes = record.PendingBytes;
        _records.Enqueue(record);

        // Publish accounting before another mailbox turn can dequeue
        // this record. Readiness reads this existing aggregate directly.
        onRecordEnqueued(pendingBytes);
        if (_records.Count == 1)
            onReady?.Invoke();
    }

    internal ValueTask<bool> ClaimAndDrainAsync(MeshReceiveBatch batch, int maximumRecords) =>
        _lane.RunAsync(
            static state => state.Mailbox.ClaimAndDrainOnLane(state.Batch, state.MaximumRecords),
            (Mailbox: this, Batch: batch, MaximumRecords: maximumRecords)
        );

    private bool ClaimAndDrainOnLane(MeshReceiveBatch batch, int maximumRecords)
    {
        if (_claimedRecordCount < 0)
            _claimedRecordCount = _records.Count;
        var count = DrainOnLane(batch, Math.Min(maximumRecords, _claimedRecordCount));
        _claimedRecordCount -= count;
        return count != 0;
    }

    private int DrainOnLane(MeshReceiveBatch batch, int limit)
    {
        var count = 0;
        while (count < limit && _records.Count != 0)
        {
            var candidate = _records.Peek();
            if (!batch.CanAdd(checked((long)candidate.PayloadBytes)))
                break;
            var record = _records.Dequeue();
            onRecordDequeued(record.PendingBytes);
            batch.Add(record.Record, record.TakeParts(), record.TakePayloadOwner());
            count++;
        }
        return count;
    }

    // Releasing a claim decides nothing for the releasing worker. The release
    // joins the mailbox FIFO before this returns, so the next claim observes it.
    internal void Release() => _lane.TryPost(static mailbox => mailbox.ReleaseOnLane(), this);

    private void ReleaseOnLane()
    {
        _claimedRecordCount = -1;
        if (UpdateReadinessOnLane())
            onReadyChanged?.Invoke();
    }

    private bool UpdateReadinessOnLane()
    {
        var ready = _records.Count != 0;
        onReadinessChanged?.Invoke(ready);
        return ready;
    }

    internal void Dispose()
    {
        List<ZLinkMeshQueuedRecord> removed = [];
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                while (_records.Count != 0)
                {
                    var record = _records.Dequeue();
                    removed.Add(record);
                    onRecordDequeued(record.PendingBytes);
                }
                _claimedRecordCount = -1;
            })
        );

        foreach (var record in removed)
            record.Dispose();
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
        IDisposable? payloadOwner = null
    )
    {
        _parts = parts;
        _payloadBytes =
            applicationPayloadBytes
            ?? record.ApplicationPayloadBytes
            ?? (
                parts is IZLinkApplicationPayloadSized sized
                    ? sized.ApplicationPayloadBytes
                    : throw new InvalidOperationException(
                        "Queued mesh records must carry application payload bytes."
                    )
            );
        record.ApplicationPayloadBytes = _payloadBytes;
        Record = record;
        _payloadOwner = payloadOwner;
        _pendingBytes = ComputePendingBytes(
            _payloadBytes,
            (ulong)(record.ApplicationMetadata?.Length ?? 0)
        );
    }

    internal ulong PayloadBytes => _payloadBytes;

    internal ulong PendingBytes => _pendingBytes;

    internal IReadOnlyList<Message> TakeParts() =>
        Interlocked.Exchange(ref _parts, null) ?? Array.Empty<Message>();

    internal IDisposable? TakePayloadOwner() => Interlocked.Exchange(ref _payloadOwner, null);

    private static ulong ComputePendingBytes(ulong payload, ulong metadata)
    {
        if (payload > ulong.MaxValue - FixedRecordBytes)
            return ulong.MaxValue;
        var total = payload + FixedRecordBytes;
        return metadata > ulong.MaxValue - total ? ulong.MaxValue : total + metadata;
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
