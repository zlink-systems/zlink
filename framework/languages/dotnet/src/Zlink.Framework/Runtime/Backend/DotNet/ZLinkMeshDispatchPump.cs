using System.Collections.Concurrent;
using System.Diagnostics;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// RouteMesh 10.0.0 node-level pull-dispatch pump (Option B, S8-06).
//
// A single background loop drains the node ready index (SetReadyHandler signals →
// DrainReady(All) until residue is exhausted, infrastructure domain first). Each
// MeshReadyRecord is claimed, its messages pulled into a receive batch, and each
// receive record dispatched by Kind. Claims are always released in finally so a
// dropped claim cannot pin an owner. Records are fanned out to per-owner state
// (keyed by spot rid) that the framework's existing per-spot pull-drain consumers
// (RecvRoute/Subscribe/RecvActorJoin/RecvActorLifecycle) read, and the per-spot
// dispatch-event handler registered via IZLinkBackendSpot.OnDispatchEvent is
// invoked so the framework schedules its drains. Completion records resolve the
// request/reply completion table.
internal sealed class ZLinkMeshDispatchPump : IAsyncDisposable
{
    private readonly IMeshNode _node;
    private readonly ZLinkMeshCompletionTable _completions;
    private readonly ConcurrentDictionary<string, SpotDispatchState> _spots = new();
    private readonly ConcurrentDictionary<
        (RoutingId NodeRid, ulong NodeGeneration),
        ZLinkServiceWireCodec.RequestSourceFence> _requestSources = new();

    private Action<ZLinkBackendRouteReceived>? _nodeRouteHandler;
    private Action? _nodeSendReadyHandler;
    private readonly object _lifecycleGate = new();
    private readonly SemaphoreSlim _signal = new(0);
    private CancellationTokenSource? _stop;
    private Task? _loop;
    private IDisposable? _capacityRegistration;
    private ZLinkInboundDispatchBudget? _inboundDispatchBudget;
    private MeshReadyDomains _pendingReadyDomains;
    private bool _started;
    private bool _disposed;

    public ZLinkMeshDispatchPump(IMeshNode node, ZLinkMeshCompletionTable completions)
    {
        _node = node;
        _completions = completions;
    }

    internal void ObserveRequestSourceFence(
        ZLinkServiceWireCodec.RequestSourceFence source)
    {
        if (source.NodeRid.IsEmpty || source.NodeGeneration == 0
            || string.IsNullOrWhiteSpace(source.OwnerId)
            || source.LeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(source));
        foreach (var key in _requestSources.Keys)
            if (key.NodeRid == source.NodeRid
                && key.NodeGeneration != source.NodeGeneration)
                _requestSources.TryRemove(key, out _);
        _requestSources[(source.NodeRid, source.NodeGeneration)] = source;
    }

    internal ZLinkServiceWireCodec.RequestSourceFence?
        ResolveRequestSourceFence(
            RoutingId sourceNodeRid,
            ulong sourceNodeGeneration) =>
        _requestSources.TryGetValue(
            (sourceNodeRid, sourceNodeGeneration),
            out var source)
            ? source
            : null;

    public void EnsureStarted()
    {
        lock (_lifecycleGate)
        {
            if (_started || _disposed) return;
            _started = true;
            _stop = new CancellationTokenSource();
            _node.SetReadyHandler(OnReady);
            _loop = Task.Run(() => RunAsync(_stop.Token));
        }
    }

    // Registers (or replaces) the per-spot dispatch-event handler and returns the
    // spot's dispatch state so the spot wrapper can pull decoded records.
    public SpotDispatchState RegisterSpot(string spotId)
    {
        return _spots.GetOrAdd(spotId, static _ => new SpotDispatchState());
    }

    internal void RekeySpot(
        string previousSpotId,
        string currentSpotId,
        SpotDispatchState state)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(previousSpotId);
        ArgumentException.ThrowIfNullOrWhiteSpace(currentSpotId);
        ArgumentNullException.ThrowIfNull(state);
        if (string.Equals(previousSpotId, currentSpotId, StringComparison.Ordinal))
            return;

        if (!_spots.TryGetValue(previousSpotId, out var registered)
            || !ReferenceEquals(registered, state))
            throw new InvalidOperationException(
                $"Spot dispatch state '{previousSpotId}' is not registered.");
        if (_spots.TryGetValue(currentSpotId, out var existing)
            && !ReferenceEquals(existing, state))
            throw new InvalidOperationException(
                $"Spot dispatch state '{currentSpotId}' is already registered.");

        if (!((ICollection<KeyValuePair<string, SpotDispatchState>>)_spots)
                .Remove(new(previousSpotId, state)))
            throw new InvalidOperationException(
                $"Spot dispatch state '{previousSpotId}' could not be rekeyed.");
        if (_spots.TryAdd(currentSpotId, state))
            return;

        _spots.TryAdd(previousSpotId, state);
        throw new InvalidOperationException(
            $"Spot dispatch state '{currentSpotId}' could not be rekeyed.");
    }

    public void SetDispatchHandler(
        string spotId,
        Action<ZLinkBackendSpotDispatchInfo> handler)
    {
        var state = RegisterSpot(spotId);
        state.DispatchHandler = handler;
        EnsureStarted();
    }

    public void SetInboundDispatchBudget(ZLinkInboundDispatchBudget budget)
    {
        ArgumentNullException.ThrowIfNull(budget);
        lock (_lifecycleGate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            _capacityRegistration?.Dispose();
            _inboundDispatchBudget = budget;
            _capacityRegistration = budget.RegisterCapacityAvailable(
                SignalApplicationCapacityAvailable);
        }
    }

    // Registers the node-level route/channel dispatch sink. Node-addressed
    // (NodeSend/NodeRequest) and channel-addressed (ChannelSend/ChannelRequest)
    // records are owned by the node (ready-record OwnerKind == Node) — their
    // source spot rid is the remote sender's, so they cannot key a local per-spot
    // queue. They are delivered to this single node-level consumer, which routes
    // them to the MeshNode builder's registered route/channel handlers.
    public void SetNodeRouteHandler(Action<ZLinkBackendRouteReceived> handler)
    {
        _nodeRouteHandler = handler;
        EnsureStarted();
    }

    public void SetNodeSendReadyHandler(Action handler)
    {
        _nodeSendReadyHandler = handler;
        EnsureStarted();
    }

    private MeshReadyDomains OnReady(MeshReadyDomains readyDomains)
    {
        SignalReady(readyDomains);
        return readyDomains;
    }

    private void SignalApplicationCapacityAvailable() =>
        SignalReady(MeshReadyDomains.Application);

    private void SignalReady(MeshReadyDomains readyDomains)
    {
        if (readyDomains == MeshReadyDomains.None) return;
        lock (_lifecycleGate)
        {
            if (_disposed) return;
            _pendingReadyDomains |= readyDomains;
        }
        try
        {
            _signal.Release();
        }
        catch (ObjectDisposedException)
        {
        }
    }

    private MeshReadyDomains TakePendingReadyDomains()
    {
        lock (_lifecycleGate)
        {
            var pending = _pendingReadyDomains;
            _pendingReadyDomains = MeshReadyDomains.None;
            return pending;
        }
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        using var readyBatch = new MeshReadyBatch();
        using var receiveBatch = new MeshReceiveBatch();
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await _signal.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }

            DrainResidue(readyBatch, receiveBatch);
        }
    }

    private void DrainResidue(MeshReadyBatch readyBatch, MeshReceiveBatch receiveBatch)
    {
        var requestedDomains = TakePendingReadyDomains();
        while (requestedDomains != MeshReadyDomains.None)
        {
            var domains = requestedDomains;
            readyBatch.Reset();
            bool residue;
            try
            {
                // Non-blocking: the native drain/claim receives block indefinitely
                // by default, which would park this pump thread inside one claim
                // and starve every other owner. The signal semaphore provides the
                // wakeups; the pump itself must never wait inside the native API.
                residue = _node.DrainReady(
                    domains, readyBatch, RecvFlags.DontWait);
            }
            catch (ObjectDisposedException)
            {
                return;
            }
            catch (ZlinkException)
            {
                return;
            }

            for (var i = 0; i < readyBatch.Count; i++)
                DrainClaim(readyBatch, i, receiveBatch);

            requestedDomains = TakePendingReadyDomains();
            if (residue)
                requestedDomains |= domains;
        }
    }

    private void DrainClaim(MeshReadyBatch readyBatch, int index, MeshReceiveBatch receiveBatch)
    {
        // The claim owner identifies the local consumer the records belong to.
        // Spot owners carry the hosting spot's rid directly; actor owners carry
        // only the actor identity (core leaves their spot_rid empty), so the
        // hosting spot is resolved through the node's actor table. Receive
        // records key per-spot dispatch by this owner rid: their own
        // SourceSpotId is the remote sender's spot (or empty for
        // session-relayed actor sends), so it cannot address the local consumer.
        var readyRecord = readyBatch[index];
        var ownerSpotId = readyRecord.SpotId;
        if (string.IsNullOrEmpty(ownerSpotId)
            && readyRecord.OwnerKind == MeshOwnerKind.Actor
            && readyRecord.Actor.ActorId is { Length: > 0 } ownerActorId)
            try
            {
                if (_node.ActorLookup(ownerActorId, out var ownerLocation))
                    ownerSpotId = ownerLocation.SpotId;
            }
            catch (ZlinkException)
            {
            }
        MeshClaim claim;
        try
        {
            claim = readyBatch.TakeClaim(index);
        }
        catch (ZlinkException)
        {
            return;
        }

        try
        {
            receiveBatch.Reset();
            // Application admission is already reserved when the record enters
            // its owner mailbox. Keep the receive turn bounded by the shared
            // count/byte/time budget instead of reducing it to one record; a
            // single-record turn starves later owners under sustained actor and
            // timer traffic while providing no additional HWM guarantee.
            receiveBatch.MaximumRecords = ZLinkReceiveBatchBudget.MaximumRecords;
            receiveBatch.MaximumBytes = ZLinkReceiveBatchBudget.MaximumBytes;
            receiveBatch.StartedAt = Stopwatch.GetTimestamp();
            if (!claim.Receive(receiveBatch, RecvFlags.DontWait))
                return;

            var count = receiveBatch.Count;
            for (var record = 0; record < count; record++)
                DispatchRecord(receiveBatch, record, ownerSpotId, readyRecord.Actor);

            // Release every claim after one bounded turn. The mailbox signals
            // again when residue remains, so the next turn returns to the
            // ready-owner rotation instead of letting one owner monopolize the
            // pump.
        }
        catch (Exception)
        {
            // A failed pull or a poison record must not kill the pump loop: the
            // pump is the node's only dispatch thread, so surviving and moving to
            // the next claim keeps every other owner (and the completion table)
            // alive.
        }
        finally
        {
            claim.Dispose();
        }
    }

    private void DispatchRecord(
        MeshReceiveBatch batch, int index, string ownerSpotId, ActorRef ownerActor)
    {
        var record = batch[index];
        switch (record.Kind)
        {
            case MeshRecordKind.Completion:
                ResolveCompletion(batch, index, record);
                return;
            case MeshRecordKind.NodeSend:
            case MeshRecordKind.NodeRequest:
            case MeshRecordKind.ChannelSend:
            case MeshRecordKind.ChannelRequest:
                EnqueueNodeRoute(batch, index, record);
                return;
            case MeshRecordKind.SpotSend:
            case MeshRecordKind.SpotRequest:
                EnqueueRoute(batch, index, record, ownerSpotId);
                return;
            case MeshRecordKind.SpotMulticast:
                EnqueueSubscribe(batch, index, record, ownerSpotId);
                return;
            case MeshRecordKind.SpotControl:
                EnqueueSpotControl(batch, index, record, ownerSpotId);
                return;
            case MeshRecordKind.ActorSend:
            case MeshRecordKind.ActorRequest:
                EnqueueActor(batch, index, record, ownerSpotId, ownerActor);
                return;
            case MeshRecordKind.SendReady:
                RaiseSendReady(record);
                return;
        }
    }

    private void ResolveCompletion(MeshReceiveBatch batch, int index, MeshReceiveRecord record)
    {
        var parts = record.PartCount > 0
            ? batch.RetainMessage(index)
            : Array.Empty<Message>();
        _completions.Complete(record, parts);
    }

    private void EnqueueRoute(MeshReceiveBatch batch, int index, MeshReceiveRecord record, string ownerSpotId)
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // and do not deliver it to a handler (spec 03 §3). The batch reset
        // releases the Core-owned parts we never retained.
        if (!TryDecodeMetadata(record, out var metadata))
            return;

        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId);
        var replyRecord = record;
        var reply = record.Kind is MeshRecordKind.NodeRequest
            or MeshRecordKind.ChannelRequest or MeshRecordKind.SpotRequest
            ? new Func<IReadOnlyList<Message>, SendFlags, SubmitResult>(
                (parts, flags) => replyRecord.Reply(parts, flags))
            : null;
        _requestSources.TryGetValue(
            (record.SourceNodeRid, record.SourceBindingGeneration),
            out var requestSource);
        var parts = RetainParts(batch, index);
        var inboundDispatchLease = TrackApplication(batch, index, record.Domain, parts);
        var route = new ZLinkBackendRouteReceived(
            parts,
            record.SourceNodeRid,
            record.SourceSpotId,
            record.OperationId == default ? null : record.OperationId.Low,
            reply,
            metadata: metadata,
            operationId: record.OperationId,
            targetNodeGeneration: record.TargetNodeGeneration,
            authorityOwnerGeneration: record.AuthorityOwnerGeneration,
            ownerLeaseGeneration: record.OwnerLeaseGeneration,
            messageFollowHopCount: record.MessageFollowHopCount,
            sourceNodeGeneration: record.SourceBindingGeneration,
            requestSource: requestSource == default ? null : requestSource,
            deadlineUnixMs:
                ZLinkMeshRecordAdapters.NormalizeDeadline(record.DeadlineUnixMs),
            inboundDispatchLease: inboundDispatchLease);
        state.Routes.Enqueue(route);
        state.Raise(ZLinkBackendSpotDispatchEvent.RouteReadable);
    }

    // Node/channel-addressed records (owned by the node). Requests carry the reply
    // token exactly like the per-spot route plane; channel records also carry the
    // addressed channel name so the node dispatcher can select the channel
    // membership's handler set. Delivered to the node-level route consumer; if none
    // is registered (no MeshNode route/channel handlers), the retained parts are
    // released so a dropped record cannot leak.
    private void EnqueueNodeRoute(MeshReceiveBatch batch, int index, MeshReceiveRecord record)
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // (spec 03 §3). No parts are retained before this point.
        if (!TryDecodeMetadata(record, out var metadata))
            return;

        var replyRecord = record;
        var reply = record.Kind is MeshRecordKind.NodeRequest or MeshRecordKind.ChannelRequest
            ? new Func<IReadOnlyList<Message>, SendFlags, SubmitResult>(
                (parts, flags) => replyRecord.Reply(parts, flags))
            : null;
        var parts = RetainParts(batch, index);
        var inboundDispatchLease = TrackApplication(batch, index, record.Domain, parts);
        var received = new ZLinkBackendRouteReceived(
            parts,
            record.SourceNodeRid,
            record.SourceSpotId,
            record.OperationId == default ? null : record.OperationId.Low,
            reply,
            record.Kind is MeshRecordKind.ChannelSend or MeshRecordKind.ChannelRequest
                ? record.ChannelName
                : null,
            metadata,
            record.OperationId,
            record.TargetNodeGeneration,
            record.AuthorityOwnerGeneration,
            record.OwnerLeaseGeneration,
            record.MessageFollowHopCount,
            record.SourceBindingGeneration,
            inboundDispatchLease: inboundDispatchLease);
        var handler = _nodeRouteHandler;
        if (handler is null)
        {
            received.Dispose();
            return;
        }

        handler(received);
    }

    private void EnqueueSubscribe(MeshReceiveBatch batch, int index, MeshReceiveRecord record, string ownerSpotId)
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // (spec 03 §3). The same publish snapshot is delivered to every matching
        // Spot handler, so the decoded view is immutable and shared.
        if (!TryDecodeMetadata(record, out var metadata))
            return;

        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId);
        var parts = RetainParts(batch, index);
        var inboundDispatchLease = TrackApplication(batch, index, record.Domain, parts);
        var message = new ZLinkBackendSubscribeMessage(
            record.ChannelName ?? string.Empty,
            record.Topic ?? string.Empty,
            parts,
            metadata,
            inboundDispatchLease);
        state.Subscriptions.Enqueue(message);
        state.Raise(ZLinkBackendSpotDispatchEvent.SubscribeReadable);
    }

    // Decodes the record's application-metadata frame into an immutable snapshot.
    // Returns false only when the frame is present but malformed, so callers
    // drop the record as a protocol error rather than deliver it.
    private static bool TryDecodeMetadata(
        MeshReceiveRecord record, out ZLinkMessageMetadata metadata)
    {
        var frame = record.ApplicationMetadata;
        if (frame is null || frame.Length == 0)
        {
            metadata = ZLinkMessageMetadata.Empty;
            return true;
        }

        return ZLinkMeshMetadataCodec.TryDecode(frame, out metadata);
    }

    private void EnqueueSpotControl(MeshReceiveBatch batch, int index, MeshReceiveRecord record, string ownerSpotId)
    {
        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId);
        if (record.OperationKind == MeshOperationKind.ActorJoin)
        {
            // Actor-join admission record: build a framework join request.
            var join = ZLinkMeshRecordAdapters.ToActorJoinRequest(batch, index, record);
            join.DispatchLease = TrackApplication(
                batch,
                index,
                record.Domain,
                join.Parts);
            state.ActorJoins.Enqueue(join);
            state.Raise(ZLinkBackendSpotDispatchEvent.ActorJoinReadable);
            return;
        }

        if (record.ActorControl is { } control)
        {
            var lifecycle = ZLinkMeshRecordAdapters.ToLifecycleEvent(control);
            if (lifecycle is { } value)
            {
                state.Lifecycles.Enqueue(value);
                state.Raise(ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable);
            }
        }
    }

    private void EnqueueActor(
        MeshReceiveBatch batch, int index, MeshReceiveRecord record,
        string ownerSpotId, ActorRef ownerActor)
    {
        var requestId = record.Kind == MeshRecordKind.ActorRequest
            ? record.ReplyRouteId
            : 0;
        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId);
        _requestSources.TryGetValue(
            (record.SourceNodeRid, record.SourceBindingGeneration),
            out var requestSource);
        var directReply = requestId == 0
            ? null
            : record.CaptureReplyRoute();
        var parts = ZLinkMeshRecordAdapters.ToActorParts(
            batch,
            index,
            record,
            ownerActor,
            requestId,
            requestSource == default ? null : requestSource,
            directReply);
        if (parts.Count == 0) return;
        state.RaiseActor(
            parts,
            TrackApplication(
                batch,
                index,
                record.Domain,
                parts.Select(static part => part.Message).ToArray()));
    }

    private ZLinkInboundDispatchLease? TrackApplication(
        MeshReceiveBatch batch,
        int index,
        MeshReadyDomains domain,
        IReadOnlyList<Message> parts)
    {
        var admitted = batch.TakeInboundDispatchLease(index);
        return admitted
            ?? (domain == MeshReadyDomains.Application
                ? _inboundDispatchBudget?.Track(parts)
                : null);
    }

    private void RaiseSendReady(MeshReceiveRecord record)
    {
        if (record.SendReady is not { } ready) return;
        if (string.IsNullOrEmpty(ready.TargetSpotId))
        {
            _nodeSendReadyHandler?.Invoke();
            return;
        }
        var state = _spots.GetValueOrDefault(ready.TargetSpotId);
        state?.RaiseSendReady();
    }

    private SpotDispatchState ResolveSpotState(string spotId)
    {
        return _spots.GetOrAdd(spotId, static _ => new SpotDispatchState());
    }

    private static IReadOnlyList<Message> RetainParts(MeshReceiveBatch batch, int index)
    {
        return batch.RetainMessage(index);
    }

    public async ValueTask DisposeAsync()
    {
        Task? loop;
        lock (_lifecycleGate)
        {
            if (_disposed) return;
            _disposed = true;
            _stop?.Cancel();
            loop = _loop;
            _capacityRegistration?.Dispose();
            _capacityRegistration = null;
        }

        if (loop is not null)
            try
            {
                await loop.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }

        _completions.FailAll(RequestResult.Terminated);
        _stop?.Dispose();
        _signal.Dispose();
    }

    // Per-spot decoded-record queues plus the registered dispatch-event handler.
    internal sealed class SpotDispatchState
    {
        public Action<ZLinkBackendSpotDispatchInfo>? DispatchHandler { get; set; }

        public Action? SendReadyHandler { get; set; }

        public ConcurrentQueue<ZLinkBackendRouteReceived> Routes { get; } = new();

        public ConcurrentQueue<ZLinkBackendSubscribeMessage> Subscriptions { get; } = new();

        public ConcurrentQueue<ZLinkBackendActorJoinRequest> ActorJoins { get; } = new();

        public ConcurrentQueue<ZLinkBackendSpotActorLifecycleEvent> Lifecycles { get; } = new();

        public void Raise(ZLinkBackendSpotDispatchEvent kind)
        {
            DispatchHandler?.Invoke(new ZLinkBackendSpotDispatchInfo(kind));
        }

        public void RaiseActor(
            IReadOnlyList<ZLinkBackendActorPart> parts,
            ZLinkInboundDispatchLease? inboundDispatchLease)
        {
            DispatchHandler?.Invoke(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ActorReadable,
                ActorParts: parts,
                ActorDispatchLease: inboundDispatchLease));
        }

        public void RaiseSendReady()
        {
            SendReadyHandler?.Invoke();
        }
    }
}
