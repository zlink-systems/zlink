using System.Collections.Concurrent;
using System.Diagnostics;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// Persistent application workers claim the existing node/owner mailboxes and
// invoke node/channel handlers directly. The shared ready mask reserves wakeups;
// no per-batch supervisor task or second application queue is involved. Claims
// are released in finally, while suspended handler results remain worker-owned.
// Spot pull consumers and completion callbacks retain their existing contracts.
internal sealed class ZLinkMeshDispatchPump : IAsyncDisposable
{
    private readonly IMeshNode _node;
    private readonly ZLinkMeshCompletionTable _completions;
    private readonly ZLinkApplicationJobQueue? _applicationJobQueue;
    private readonly ConcurrentDictionary<ZLinkSpotId, SpotDispatchState> _spots = new();
    private readonly ConcurrentDictionary<
        (RoutingId NodeRid, ulong NodeGeneration),
        ZLinkServiceWireCodec.RequestSourceFence> _requestSources = new();

    private Func<IReadOnlyList<ZLinkBackendRouteReceived>, CancellationToken, ValueTask>? _nodeRouteHandler;
    private ZLinkRuntimeTaskRunner? _applicationTaskRunner;
    private readonly ZLinkStateLane _lane = new();
    private readonly SemaphoreSlim _signal = new(0);
    private CancellationTokenSource? _stop;
    private Task? _loop;
    private ZLinkApplicationJobQueueLease? _reservedApplicationAdmission;
    private int _applicationAdmissionWaitActive;
    private int _pendingReadyDomains;
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
        {
            var workers = new Task[Math.Max(2, Environment.ProcessorCount)];
            for (var index = 0; index < workers.Length; index++)
                workers[index] = _applicationTaskRunner is { } runner
                    ? runner.Run("mesh-application-worker",
                        ct => RunAsync(_stop.Token, ct))
                    : Task.Run(() => RunAsync(_stop.Token, CancellationToken.None).AsTask());
            _loop = Task.WhenAll(workers);
        }
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
    public void SetNodeRouteHandler(
        Func<IReadOnlyList<ZLinkBackendRouteReceived>, CancellationToken, ValueTask> handler,
        ZLinkRuntimeTaskRunner? taskRunner = null)
    {
        _nodeRouteHandler = handler;
        _applicationTaskRunner = taskRunner;
    }

    private MeshReadyDomains OnReady(MeshReadyDomains readyDomains)
    {
        SignalReady(readyDomains);
        return readyDomains;
    }

    private void SignalReady(MeshReadyDomains readyDomains)
    {
        if (readyDomains == MeshReadyDomains.None || Volatile.Read(ref _disposed))
            return;
        // The pending-domain mask is also the wake reservation. Only its empty
        // to nonempty transition publishes a signal; no state-lane turn is needed.
        if (Interlocked.Or(ref _pendingReadyDomains, (int)readyDomains) != 0)
            return;
        try
        {
            _signal.Release();
        }
        catch (ObjectDisposedException)
        {
        }
    }

    private MeshReadyDomains TakePendingReadyDomains() =>
        (MeshReadyDomains)Interlocked.Exchange(ref _pendingReadyDomains, 0);

    private async ValueTask RunAsync(
        CancellationToken stopToken,
        CancellationToken runtimeToken)
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(stopToken, runtimeToken);
        var cancellationToken = stop.Token;
        using var readyBatch = new MeshReadyBatch { MaximumRecords = 1 };
        using var receiveBatch = new MeshReceiveBatch();
        // These are handler-returned asynchronous results, not another queue of
        // records. Keep their lifetime inside the persistent worker registration.
        var pending = new List<Task>();
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                for (var index = pending.Count - 1; index >= 0; index--)
                {
                    if (!pending[index].IsCompleted)
                        continue;
                    ObserveDispatchResult(pending[index]);
                    pending.RemoveAt(index);
                }
                await _signal.WaitAsync(cancellationToken).ConfigureAwait(false);
                DrainResidue(readyBatch, receiveBatch, pending, cancellationToken);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            if (pending.Count != 0)
                await Task.WhenAll(pending).ConfigureAwait(false);
        }
    }

    private void ObserveDispatchResult(Task result)
    {
        try
        {
            result.GetAwaiter().GetResult();
        }
        catch (Exception exception)
        {
            if (_applicationTaskRunner is null)
                throw;
            _applicationTaskRunner.ErrorSink.ReportRuntimeTaskException(
                "mesh-application-worker", exception);
        }
    }

    private void DrainResidue(
        MeshReadyBatch readyBatch,
        MeshReceiveBatch receiveBatch,
        List<Task> pending,
        CancellationToken cancellationToken)
    {
        var requestedDomains = TakePendingReadyDomains();
        while (requestedDomains != MeshReadyDomains.None)
        {
            var domains = requestedDomains;
            readyBatch.Reset();
            readyBatch.RequireReservedApplicationAdmission =
                Volatile.Read(ref _applicationAdmissionWaitActive) != 0
                && Volatile.Read(ref _reservedApplicationAdmission) is null;
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

            if (residue)
                SignalReady(domains);
            for (var i = 0; i < readyBatch.Count; i++)
                DrainClaim(
                    readyBatch,
                    i,
                    receiveBatch,
                    pending,
                    cancellationToken);

            // Even a claim that could not obtain admission must be released
            // before this worker sleeps: its permit wake may go to another worker.
            readyBatch.Reset();

            // Re-enter through the shared ready signal so another waiting
            // worker can acquire a different owner during a suspended handler.
            requestedDomains = MeshReadyDomains.None;
        }
    }

    private void DrainClaim(
        MeshReadyBatch readyBatch,
        int index,
        MeshReceiveBatch receiveBatch,
        List<Task> pending,
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
        ZLinkApplicationJobQueueLease?[] admissions = [];
        var admissionCount = 0;
        if (readyRecord.Domain == MeshReadyDomains.Application
            && !readyRecord.ApplicationAdmissionReserved
            && _applicationJobQueue is not null)
        {
            var first = TryTakeApplicationAdmission(cancellationToken);
            if (first is null)
                return;
            var admissionBudget = Math.Min(
                readyRecord.AvailableRecords,
                ZLinkReceiveBatchBudget.MaximumRecords);
            admissions = new ZLinkApplicationJobQueueLease?[admissionBudget];
            admissions[0] = first;
            admissionCount = 1 + _applicationJobQueue.TryAcquireBatch(
                admissions, 1, admissionBudget - 1);
        }
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
            DisposeAdmissions(admissions);
            return;
        }

        List<ZLinkBackendRouteReceived>? nodeRoutes = new(
            ZLinkReceiveBatchBudget.MaximumRecords);
        try
        {
            receiveBatch.Reset();
            // Release every claim after one bounded turn. Application-domain
            // claims reserve only the number of records this turn can publish;
            // a record that already carries receive-side admission returns the
            // corresponding extra reservation below.
            receiveBatch.MaximumRecords = admissionCount == 0
                ? ZLinkReceiveBatchBudget.MaximumRecords
                : admissionCount;
            receiveBatch.MaximumBytes = ZLinkReceiveBatchBudget.MaximumBytes;
            receiveBatch.StartedAt = Stopwatch.GetTimestamp();
            if (!claim.Receive(receiveBatch, RecvFlags.DontWait))
                return;

            var count = receiveBatch.Count;
            var externalAdmissions = 0;
            if (admissionCount != 0)
            {
                for (var record = 0; record < count; record++)
                    if (RequiresApplicationAdmission(receiveBatch[record].Kind)
                        && receiveBatch.GetApplicationJobAdmission(record) is null)
                        externalAdmissions++;
                _applicationJobQueue!.MarkQueuedBatch(admissions, externalAdmissions);
            }
            // Reservations have no record identity. Assign the required prefix
            // to application records; unused and embedded-owner duplicates stay
            // in this same array for one ReleaseBatch below.
            var externalIndex = 0;
            for (var record = 0; record < count; record++)
            {
                ZLinkApplicationJobQueueLease? admission = null;
                if (admissionCount != 0
                    && RequiresApplicationAdmission(receiveBatch[record].Kind)
                    && receiveBatch.GetApplicationJobAdmission(record) is null)
                {
                    admission = admissions[externalIndex];
                    admissions[externalIndex++] = null;
                }
                try
                {
                    if (DispatchRecord(
                            receiveBatch,
                            record,
                            ownerSpotId,
                            readyRecord.Actor,
                            admission,
                            nodeRoutes))
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
            // Start this owner's records in claim order. A suspended handler's
            // result remains with the worker, while the claim returns for the
            // next bounded batch and other owners run on their own workers.
            var dispatch = DispatchNodeRoutes(nodeRoutes, cancellationToken);
            // The asynchronous handler owns this list until its result completes.
            // Clearing it here invalidates an enumerator suspended inside the handler.
            nodeRoutes = null;
            if (dispatch.IsCompletedSuccessfully)
                dispatch.GetAwaiter().GetResult();
            else
                pending.Add(dispatch.AsTask());
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (ObjectDisposedException)
        {
        }
        catch (Exception exception)
        {
            if (_applicationTaskRunner is null)
                throw;
            _applicationTaskRunner.ErrorSink.ReportRuntimeTaskException(
                "mesh-application-worker", exception);
        }
        finally
        {
            if (nodeRoutes is not null)
                foreach (var received in nodeRoutes)
                    received.Dispose();
            DisposeAdmissions(admissions);
            claim.Dispose();
        }
    }

    private void DisposeAdmissions(IReadOnlyList<ZLinkApplicationJobQueueLease?> admissions)
    {
        if (admissions.Count != 0)
            _applicationJobQueue!.ReleaseBatch(admissions);
    }

    internal static bool RequiresApplicationAdmission(
        MeshRecordKind recordKind) =>
        recordKind is not MeshRecordKind.Completion
            and not MeshRecordKind.SendReady;

    private ZLinkApplicationJobQueueLease? TryTakeApplicationAdmission(
        CancellationToken cancellationToken)
    {
        var reserved = Interlocked.Exchange(
            ref _reservedApplicationAdmission,
            null);
        if (reserved is not null)
        {
            Volatile.Write(ref _applicationAdmissionWaitActive, 0);
            return reserved;
        }
        var queue = _applicationJobQueue;
        if (queue is not null && queue.TryAcquire(out var immediate))
            return immediate;
        if (queue is not null
            && Interlocked.CompareExchange(
                ref _applicationAdmissionWaitActive,
                1,
                0) == 0)
        {
            using (ExecutionContext.SuppressFlow())
                _ = Task.Run(() => WaitForApplicationAdmissionAsync(
                    queue,
                    cancellationToken));
        }
        return null;
    }

    private async Task WaitForApplicationAdmissionAsync(
        ZLinkApplicationJobQueue queue,
        CancellationToken cancellationToken)
    {
        ZLinkApplicationJobQueueLease? admission = null;
        var transferred = false;
        try
        {
            admission = await queue.AcquireAsync(cancellationToken)
                .ConfigureAwait(false);
            if (cancellationToken.IsCancellationRequested)
                return;
            Interlocked.Exchange(
                ref _reservedApplicationAdmission,
                admission)?.Dispose();
            transferred = true;
            admission = null;
            SignalReady(MeshReadyDomains.Application);
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            admission?.Dispose();
            // A consumer can already have taken the published reservation and
            // registered its successor while SignalReady is still returning.
            if (!transferred)
                Volatile.Write(ref _applicationAdmissionWaitActive, 0);
        }
    }

    private bool DispatchRecord(
        MeshReceiveBatch batch,
        int index,
        string ownerSpotId,
        ActorRef ownerActor,
        ZLinkApplicationJobQueueLease? admission,
        List<ZLinkBackendRouteReceived> nodeRoutes)
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
                return EnqueueNodeRoute(
                    batch,
                    index,
                    record,
                    admission,
                    nodeRoutes);
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
        ZLinkApplicationJobQueueLease? admission,
        List<ZLinkBackendRouteReceived> nodeRoutes)
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
        var parts = record.ApplicationPayloadView is null
            ? RetainParts(batch, index)
            : Array.Empty<Message>();
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
            payloadOwner: payloadOwner,
            applicationPayloadView: record.ApplicationPayloadView);
        if (_nodeRouteHandler is null)
        {
            received.Dispose();
            return admission is not null;
        }

        nodeRoutes.Add(received);
        return admission is not null;
    }

    private ValueTask DispatchNodeRoutes(
        List<ZLinkBackendRouteReceived> nodeRoutes,
        CancellationToken cancellationToken)
    {
        if (nodeRoutes.Count == 0)
            return ValueTask.CompletedTask;
        var handler = _nodeRouteHandler;
        if (handler is not null)
        {
            return handler(nodeRoutes, cancellationToken);
        }

        foreach (var received in nodeRoutes)
            received.Dispose();
        return ValueTask.CompletedTask;
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
        failures.Capture(() => Interlocked.Exchange(
            ref _reservedApplicationAdmission,
            null)?.Dispose());
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
