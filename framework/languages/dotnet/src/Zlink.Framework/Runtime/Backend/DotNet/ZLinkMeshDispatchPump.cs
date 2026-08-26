using System.Collections.Concurrent;
using System.Diagnostics;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Messaging;

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
    private readonly ZLinkApplicationJobQueue? _applicationJobQueue;
    private readonly ConcurrentDictionary<ZLinkSpotId, SpotDispatchState> _spots = new();
    private readonly ConcurrentDictionary<
        (RoutingId NodeRid, ulong NodeGeneration),
        ZLinkServiceWireCodec.RequestSourceFence> _requestSources = new();

    private Action<ZLinkBackendRouteReceived>? _nodeRouteHandler;
    private readonly ZLinkStateLane _lane = new();
    private readonly SemaphoreSlim _signal = new(0);
    private CancellationTokenSource? _stop;
    private Task? _loop;
    private MeshReadyDomains _pendingReadyDomains;
    private bool _started;
    private bool _disposed;

    public ZLinkMeshDispatchPump(
        IMeshNode node,
        ZLinkMeshCompletionTable completions,
        ZLinkApplicationJobQueue? applicationJobQueue = null)
    {
        _node = node;
        _completions = completions;
        _applicationJobQueue = applicationJobQueue;
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
        AwaitStateLane(_lane.RunAsync(EnsureStartedOnLane));
    }

    private void EnsureStartedOnLane()
    {
        if (_started || _disposed) return;
        _started = true;
        _stop = new CancellationTokenSource();
        _node.SetReadyHandler(OnReady);
        using (ExecutionContext.SuppressFlow())
            _loop = Task.Run(() => RunAsync(_stop.Token));
    }

    // Registers (or replaces) the per-spot dispatch-event handler and returns the
    // spot's dispatch state so the spot wrapper can pull decoded records.
    public SpotDispatchState RegisterSpot(string spotId)
    {
        return RegisterSpot(
            ZLinkSpotId.FromBoundary(spotId, nameof(spotId)));
    }

    private SpotDispatchState RegisterSpot(ZLinkSpotId spotId) =>
        _spots.GetOrAdd(spotId, static _ => new SpotDispatchState());

    internal void RekeySpot(
        string previousSpotId,
        string currentSpotId,
        SpotDispatchState state)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(previousSpotId);
        ArgumentException.ThrowIfNullOrWhiteSpace(currentSpotId);
        ArgumentNullException.ThrowIfNull(state);
        var previous = ZLinkSpotId.FromBoundary(
            previousSpotId,
            nameof(previousSpotId));
        var current = ZLinkSpotId.FromBoundary(
            currentSpotId,
            nameof(currentSpotId));
        if (previous == current)
            return;

        if (!_spots.TryGetValue(previous, out var registered)
            || !ReferenceEquals(registered, state))
            throw new InvalidOperationException(
                $"Spot dispatch state '{previousSpotId}' is not registered.");
        if (_spots.TryGetValue(current, out var existing)
            && !ReferenceEquals(existing, state))
            throw new InvalidOperationException(
                $"Spot dispatch state '{currentSpotId}' is already registered.");

        if (!((ICollection<KeyValuePair<ZLinkSpotId, SpotDispatchState>>)_spots)
                .Remove(new(previous, state)))
            throw new InvalidOperationException(
                $"Spot dispatch state '{previousSpotId}' could not be rekeyed.");
        if (_spots.TryAdd(current, state))
            return;

        _spots.TryAdd(previous, state);
        throw new InvalidOperationException(
            $"Spot dispatch state '{currentSpotId}' could not be rekeyed.");
    }

    public void SetDispatchHandler(
        string spotId,
        Action<ZLinkBackendSpotDispatchInfo> handler)
    {
        var state = RegisterSpot(spotId);
        state.DispatchHandler = handler;
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
    }

    private MeshReadyDomains OnReady(MeshReadyDomains readyDomains)
    {
        SignalReady(readyDomains);
        return readyDomains;
    }

    private void SignalReady(MeshReadyDomains readyDomains)
    {
        if (readyDomains == MeshReadyDomains.None) return;
        if (_lane.IsOnLane)
            SignalReadyOnLane(readyDomains);
        else
            AwaitStateLane(_lane.RunAsync(() => SignalReadyOnLane(readyDomains)));
    }

    private void SignalReadyOnLane(MeshReadyDomains readyDomains)
    {
        if (_disposed) return;
        _pendingReadyDomains |= readyDomains;
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
        return AwaitStateLane(_lane.RunAsync(TakePendingReadyDomainsOnLane));
    }

    private MeshReadyDomains TakePendingReadyDomainsOnLane()
    {
        var pending = _pendingReadyDomains;
        _pendingReadyDomains = MeshReadyDomains.None;
        return pending;
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

            await DrainResidueAsync(
                    readyBatch,
                    receiveBatch,
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask DrainResidueAsync(
        MeshReadyBatch readyBatch,
        MeshReceiveBatch receiveBatch,
        CancellationToken cancellationToken)
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
                await DrainClaimAsync(
                        readyBatch,
                        i,
                        receiveBatch,
                        cancellationToken)
                    .ConfigureAwait(false);

            requestedDomains = TakePendingReadyDomains();
            if (residue)
                requestedDomains |= domains;
        }
    }

    private async ValueTask DrainClaimAsync(
        MeshReadyBatch readyBatch,
        int index,
        MeshReceiveBatch receiveBatch,
        CancellationToken cancellationToken)
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
            // Release every claim after one bounded turn. Raw ingress already
            // carries its pre-receive admission; locally produced records use
            // the fallback below after their mailbox claim is materialized.
            receiveBatch.MaximumRecords = 1;
            receiveBatch.MaximumBytes = ZLinkReceiveBatchBudget.MaximumBytes;
            receiveBatch.StartedAt = Stopwatch.GetTimestamp();
            if (!claim.Receive(receiveBatch, RecvFlags.DontWait))
                return;

            var count = receiveBatch.Count;
            for (var record = 0; record < count; record++)
            {
                ZLinkApplicationJobQueueLease? admission = null;
                try
                {
                    var embeddedAdmission = receiveBatch
                        .GetApplicationJobAdmission(record);
                    if (embeddedAdmission is null
                        && RequiresApplicationAdmission(
                            receiveBatch[record].Kind)
                        && _applicationJobQueue is { } applicationJobQueue)
                        admission = await applicationJobQueue
                            .AcquireAsync(cancellationToken)
                            .ConfigureAwait(false);

                    if (DispatchRecord(
                            receiveBatch,
                            record,
                            ownerSpotId,
                            readyRecord.Actor,
                            admission))
                        admission = null;

                    // A malformed or unsupported pre-admitted record may not
                    // transfer its owner. Return both the payload owner and the queued
                    // permit in this same bounded turn.
                    if (receiveBatch.GetApplicationJobAdmission(record) is not null)
                        receiveBatch.TakePayloadOwner(record)?.Dispose();
                }
                finally
                {
                    admission?.Dispose();
                }
            }
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException)
        {
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

    internal static bool RequiresApplicationAdmission(
        MeshRecordKind recordKind) =>
        recordKind is not MeshRecordKind.Completion
            and not MeshRecordKind.SendReady;

    private bool DispatchRecord(
        MeshReceiveBatch batch,
        int index,
        string ownerSpotId,
        ActorRef ownerActor,
        ZLinkApplicationJobQueueLease? admission)
    {
        var record = batch[index];
        switch (record.Kind)
        {
            case MeshRecordKind.Completion:
                ResolveCompletion(batch, index, record);
                return false;
            case MeshRecordKind.NodeSend:
            case MeshRecordKind.NodeRequest:
            case MeshRecordKind.ChannelSend:
            case MeshRecordKind.ChannelRequest:
                return EnqueueNodeRoute(batch, index, record, admission);
            case MeshRecordKind.SpotSend:
            case MeshRecordKind.SpotRequest:
                return EnqueueRoute(batch, index, record, ownerSpotId, admission);
            case MeshRecordKind.SpotMulticast:
                return EnqueueSubscribe(batch, index, record, ownerSpotId, admission);
            case MeshRecordKind.SpotControl:
                return EnqueueSpotControl(batch, index, record, ownerSpotId, admission);
            case MeshRecordKind.ActorSend:
            case MeshRecordKind.ActorRequest:
                return EnqueueActor(
                    batch,
                    index,
                    record,
                    ownerSpotId,
                    ownerActor,
                    admission);
            default:
                return false;
        }
    }

    private void ResolveCompletion(MeshReceiveBatch batch, int index, MeshReceiveRecord record)
    {
        var parts = record.PartCount > 0
            ? batch.RetainMessage(index)
            : Array.Empty<Message>();
        _completions.Complete(record, parts);
    }

    private bool EnqueueRoute(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        string ownerSpotId,
        ZLinkApplicationJobQueueLease? admission)
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // and do not deliver it to a handler (spec 03 §3). The batch reset
        // releases the Core-owned parts we never retained.
        if (!TryDecodeMetadata(record, out var metadata))
            return false;

        admission?.MarkQueued();

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
        var payloadOwner = AttachAdmission(
            batch.TakePayloadOwner(index),
            admission);
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
            payloadOwner: payloadOwner);
        state.Routes.Enqueue(route);
        state.Raise(ZLinkBackendSpotDispatchEvent.RouteReadable);
        return admission is not null;
    }

    // Node/channel-addressed records (owned by the node). Requests carry the reply
    // token exactly like the per-spot route plane; channel records also carry the
    // addressed channel name so the node dispatcher can select the channel
    // membership's handler set. Delivered to the node-level route consumer; if none
    // is registered (no MeshNode route/channel handlers), the retained parts are
    // released so a dropped record cannot leak.
    private bool EnqueueNodeRoute(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        ZLinkApplicationJobQueueLease? admission)
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // (spec 03 §3). No parts are retained before this point.
        if (!TryDecodeMetadata(record, out var metadata))
            return false;

        admission?.MarkQueued();

        var replyRecord = record;
        var reply = record.Kind is MeshRecordKind.NodeRequest or MeshRecordKind.ChannelRequest
            ? new Func<IReadOnlyList<Message>, SendFlags, SubmitResult>(
                (parts, flags) => replyRecord.Reply(parts, flags))
            : null;
        var parts = RetainParts(batch, index);
        var payloadOwner = AttachAdmission(
            batch.TakePayloadOwner(index),
            admission);
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
            payloadOwner: payloadOwner);
        var handler = _nodeRouteHandler;
        if (handler is null)
        {
            received.Dispose();
            return admission is not null;
        }

        handler(received);
        return admission is not null;
    }

    private bool EnqueueSubscribe(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        string ownerSpotId,
        ZLinkApplicationJobQueueLease? admission)
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // (spec 03 §3). The same publish snapshot is delivered to every matching
        // Spot handler, so the decoded view is immutable and shared.
        if (!TryDecodeMetadata(record, out var metadata))
            return false;

        admission?.MarkQueued();

        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId);
        var parts = RetainParts(batch, index);
        var message = new ZLinkBackendSubscribeMessage(
            record.ChannelName ?? string.Empty,
            record.Topic ?? string.Empty,
            parts,
            metadata,
            AttachAdmission(batch.TakePayloadOwner(index), admission));
        state.Subscriptions.Enqueue(message);
        state.Raise(ZLinkBackendSpotDispatchEvent.SubscribeReadable);
        return admission is not null;
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

    private bool EnqueueSpotControl(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        string ownerSpotId,
        ZLinkApplicationJobQueueLease? admission)
    {
        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId);
        if (record.OperationKind == MeshOperationKind.ActorJoin)
        {
            // Actor-join admission record: build a framework join request.
            admission?.MarkQueued();
            var join = ZLinkMeshRecordAdapters.ToActorJoinRequest(batch, index, record);
            var payloadOwner = AttachAdmission(
                batch.TakePayloadOwner(index),
                admission);
            if (payloadOwner is not null)
                join.AttachPayloadOwner(payloadOwner);
            state.ActorJoins.Enqueue(join);
            state.Raise(ZLinkBackendSpotDispatchEvent.ActorJoinReadable);
            return admission is not null;
        }

        if (record.ActorControl is { } control)
        {
            var lifecycle = ZLinkMeshRecordAdapters.ToLifecycleEvent(control);
            if (lifecycle is { } value)
            {
                admission?.MarkQueued();
                state.Lifecycles.Enqueue(value with
                {
                    ApplicationJobAdmission = admission
                });
                state.Raise(ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable);
                return admission is not null;
            }
        }
        return false;
    }

    private bool EnqueueActor(
        MeshReceiveBatch batch, int index, MeshReceiveRecord record,
        string ownerSpotId,
        ActorRef ownerActor,
        ZLinkApplicationJobQueueLease? admission)
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
        if (parts.Count == 0) return false;
        admission?.MarkQueued();
        state.RaiseActor(
            parts,
            AttachAdmission(batch.TakePayloadOwner(index), admission));
        return admission is not null;
    }

    private static IDisposable? AttachAdmission(
        IDisposable? payloadOwner,
        ZLinkApplicationJobQueueLease? admission) =>
        admission is null
            ? payloadOwner
            : new ZLinkApplicationJobQueueRecordOwner(
                payloadOwner,
                admission);

    private SpotDispatchState ResolveSpotState(string spotId)
    {
        return RegisterSpot(spotId);
    }

    private static IReadOnlyList<Message> RetainParts(MeshReceiveBatch batch, int index)
    {
        return batch.RetainMessage(index);
    }

    public async ValueTask DisposeAsync()
    {
        var stopped = await _lane.RunAsync(StopOnLane).ConfigureAwait(false);
        if (stopped is null) return;
        var (stop, loop) = stopped.Value;
        stop?.Cancel();

        Exception? loopFailure = null;
        if (loop is not null)
            try
            {
                await loop.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
            catch (Exception exception)
            {
                loopFailure = exception;
            }

        var failures = new ZLinkFailureCollector(loopFailure);
        failures.Capture(() =>
            _completions.FailAll(RequestResult.Terminated));
        await failures.CaptureAsync(() =>
                new ValueTask(_completions.CompletionDrained))
            .ConfigureAwait(false);
        failures.Capture(() => stop?.Dispose());
        failures.Capture(_signal.Dispose);
        failures.ThrowIfAny();
    }

    private (CancellationTokenSource? Stop, Task? Loop)? StopOnLane()
    {
        if (_disposed) return null;
        _disposed = true;
        return (_stop, _loop);
    }

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    // Per-spot decoded-record queues plus the registered dispatch-event handler.
    internal sealed class SpotDispatchState
    {
        public Action<ZLinkBackendSpotDispatchInfo>? DispatchHandler { get; set; }

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
            IDisposable? payloadOwner)
        {
            var handler = DispatchHandler;
            if (handler is null)
            {
                foreach (var part in parts)
                    part.Message.Dispose();
                payloadOwner?.Dispose();
                return;
            }
            try
            {
                handler(new ZLinkBackendSpotDispatchInfo(
                    ZLinkBackendSpotDispatchEvent.ActorReadable,
                    ActorParts: parts,
                    ActorPayloadOwner: payloadOwner));
            }
            catch
            {
                foreach (var part in parts)
                    part.Message.Dispose();
                payloadOwner?.Dispose();
                throw;
            }
        }

    }
}
