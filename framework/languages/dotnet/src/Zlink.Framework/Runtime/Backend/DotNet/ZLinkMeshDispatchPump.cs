using System.Collections.Concurrent;
using System.Diagnostics;
using Zlink.Framework.Contracts.Streams;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Messaging;
using static Zlink.Framework.Runtime.Execution.ZLinkStateLaneWait;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// The process pool schedules turns of the existing node/owner mailboxes.
// Domain readiness is the wake reservation, and async claims retain their
// batches while the physical worker returns to the common capacity.
internal sealed class ZLinkMeshDispatchPump : IAsyncDisposable
{
    private const int ReadyOwnersPerBatch = 1;

    internal static int ApplicationWorkerCount => ZLinkProcessExecutionPool.ApplicationWorkerCount;

    private readonly IMeshNode _node;
    private readonly ZLinkMeshCompletionTable _completions;
    private readonly ZLinkApplicationJobQueue? _applicationJobQueue;
    private readonly ConcurrentDictionary<ZLinkSpotId, SpotDispatchState> _spots = new();
    private readonly ConcurrentDictionary<
        (RoutingId NodeRid, ulong NodeGeneration),
        ZLinkServiceWireCodec.RequestSourceFence
    > _requestSources = new();
    private IZLinkBackendRequestSourceAcceptance? _requestSourceAcceptance;

    private Func<
        IReadOnlyList<ZLinkBackendRouteReceived>,
        CancellationToken,
        ValueTask
    >? _nodeRouteHandler;
    private ZLinkRuntimeTaskRunner? _applicationTaskRunner;
    private readonly ZLinkStateLane _lane = new();
    private ZLinkProcessExecutionPool.ApplicationSource? _applicationSource;
    private ZLinkRuntimeTaskRunner.ExecutionLease? _sourceExecution;
    private CancellationTokenSource? _stop;
    private int _pendingReadyDomains;
    private bool _started;
    private bool _disposed;

    public ZLinkMeshDispatchPump(
        IMeshNode node,
        ZLinkMeshCompletionTable completions,
        ZLinkApplicationJobQueue? applicationJobQueue = null
    )
    {
        _node = node;
        _completions = completions;
        _applicationJobQueue = applicationJobQueue;
    }

    internal void ObserveRequestSourceFence(ZLinkServiceWireCodec.RequestSourceFence source)
    {
        if (
            source.NodeRid.IsEmpty
            || source.NodeGeneration == 0
            || string.IsNullOrWhiteSpace(source.OwnerId)
            || source.LeaseGeneration == 0
        )
            throw new ArgumentOutOfRangeException(nameof(source));
        foreach (var key in _requestSources.Keys)
            if (key.NodeRid == source.NodeRid && key.NodeGeneration != source.NodeGeneration)
                _requestSources.TryRemove(key, out _);
        _requestSources[(source.NodeRid, source.NodeGeneration)] = source;
    }

    internal ZLinkServiceWireCodec.RequestSourceFence? ResolveRequestSourceFence(
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration
    ) =>
        _requestSources.TryGetValue((sourceNodeRid, sourceNodeGeneration), out var source)
            ? source
            : null;

    internal void SetRequestSourceAcceptance(IZLinkBackendRequestSourceAcceptance acceptance) =>
        Volatile.Write(ref _requestSourceAcceptance, acceptance);

    // Location runtime §9.3: the first accepting owner reads the source owner values once.
    // The polled descriptors answer first; a sender they do not contain is read from the
    // Store once and kept with them, so later requests do not read the Store again.
    internal async ValueTask<ZLinkServiceWireCodec.RequestSourceFence?> AcceptRequestSourceAsync(
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        CancellationToken cancellationToken
    )
    {
        var polled = ResolveRequestSourceFence(sourceNodeRid, sourceNodeGeneration);
        if (Volatile.Read(ref _requestSourceAcceptance) is not { } acceptance)
            return polled;
        var accepted = await acceptance
            .AcceptAsync(polled, sourceNodeRid, sourceNodeGeneration, cancellationToken)
            .ConfigureAwait(false);
        if (polled is null && accepted is { } read)
            ObserveRequestSourceFence(read);
        return accepted;
    }

    public void EnsureStarted()
    {
        AwaitStateLane(_lane.RunAsync(EnsureStartedOnLane));
    }

    private void EnsureStartedOnLane()
    {
        if (_started || _disposed)
            return;
        _started = true;
        _stop = CancellationTokenSource.CreateLinkedTokenSource(
            _applicationTaskRunner?.ShutdownToken ?? CancellationToken.None
        );
        _node.SetReadyHandler(OnReady);
        _applicationSource = ZLinkProcessExecutionPool.CreateApplicationSource(
            worker =>
                _applicationTaskRunner is { } runner
                    ? runner.RunSourceTurn(
                        _sourceExecution!,
                        static (state, token) => state.Pump.RunTurn(state.Worker, token),
                        (Pump: this, Worker: worker)
                    )
                    : RunTurn(worker, _stop.Token),
            error => ObserveDispatchResult(Task.FromException(error)),
            maximumConcurrentTurns: ApplicationWorkerCount
        );
        if (_applicationTaskRunner is { } taskRunner)
            _sourceExecution = taskRunner.RegisterSource(
                "mesh-application-source",
                _applicationSource.Completion
            );
        _applicationSource.StopOnCancellation(_stop.Token);
        if (Volatile.Read(ref _pendingReadyDomains) != 0)
            _applicationSource.Signal();
    }

    // Registers (or replaces) the per-spot dispatch-event handler and returns the
    // spot's dispatch state so the spot wrapper can pull decoded records.
    public SpotDispatchState RegisterSpot(string spotId)
    {
        return RegisterSpot(ZLinkSpotId.FromBoundary(spotId, nameof(spotId)));
    }

    private SpotDispatchState RegisterSpot(ZLinkSpotId spotId) =>
        _spots.GetOrAdd(spotId, static _ => new SpotDispatchState());

    internal void RekeySpot(string previousSpotId, string currentSpotId, SpotDispatchState state)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(previousSpotId);
        ArgumentException.ThrowIfNullOrWhiteSpace(currentSpotId);
        ArgumentNullException.ThrowIfNull(state);
        var previous = ZLinkSpotId.FromBoundary(previousSpotId, nameof(previousSpotId));
        var current = ZLinkSpotId.FromBoundary(currentSpotId, nameof(currentSpotId));
        if (previous == current)
            return;

        if (
            !_spots.TryGetValue(previous, out var registered) || !ReferenceEquals(registered, state)
        )
            throw new InvalidOperationException(
                $"Spot dispatch state '{previousSpotId}' is not registered."
            );
        if (_spots.TryGetValue(current, out var existing) && !ReferenceEquals(existing, state))
            throw new InvalidOperationException(
                $"Spot dispatch state '{currentSpotId}' is already registered."
            );

        if (!RemoveSpotRegistration(previous, state))
            throw new InvalidOperationException(
                $"Spot dispatch state '{previousSpotId}' could not be rekeyed."
            );
        if (_spots.TryAdd(current, state))
            return;

        _spots.TryAdd(previous, state);
        throw new InvalidOperationException(
            $"Spot dispatch state '{currentSpotId}' could not be rekeyed."
        );
    }

    private bool RemoveSpotRegistration(ZLinkSpotId spotId, SpotDispatchState state) =>
        ((ICollection<KeyValuePair<ZLinkSpotId, SpotDispatchState>>)_spots).Remove(
            new(spotId, state)
        );

    internal async ValueTask UnregisterSpotAsync(string spotId, SpotDispatchState state)
    {
        RemoveSpotRegistration(ZLinkSpotId.FromBoundary(spotId, nameof(spotId)), state);
        var failures = new ZLinkFailureCollector();
        var pending = new List<Task>();
        if (!state.Routes.IsEmpty && state.DispatchHandler is not null)
            failures.Capture(() =>
                ObserveDispatchResult(
                    state.Raise(ZLinkBackendSpotDispatchEvent.RouteReadable),
                    pending
                )
            );
        foreach (var completion in pending)
            await failures.CaptureAsync(() => new ValueTask(completion)).ConfigureAwait(false);
        while (state.Routes.TryDequeue(out var route))
            failures.Capture(route.Dispose);
        while (state.Subscriptions.TryDequeue(out var subscription))
            failures.Capture(subscription.Dispose);
        while (state.ActorJoins.TryDequeue(out var join))
            failures.Capture(join.Dispose);
        while (state.Lifecycles.TryDequeue(out var lifecycle))
            failures.Capture(() => lifecycle.ApplicationJobAdmission?.Dispose());
        failures.ThrowIfAny();
    }

    public void SetDispatchHandler(
        string spotId,
        Func<
            ZLinkBackendSpotDispatchInfo,
            (ValueTask Completion, Func<CancellationToken, ValueTask>? Drain)
        > handler
    )
    {
        var state = RegisterSpot(spotId);
        state.DispatchHandler = handler;
        if (!state.Routes.IsEmpty || !state.Subscriptions.IsEmpty)
        {
            var runner =
                _applicationTaskRunner
                ?? throw new InvalidOperationException(
                    "Retained Spot ingress requires the existing application execution owner."
                );
            ZLinkRuntimeTaskRunner.WithoutExecutionContextFlow(() =>
                runner.TryRunDetached(
                    "mesh-spot-dispatch-registration",
                    async _ =>
                    {
                        var pending = new List<Task>();
                        if (!state.Routes.IsEmpty)
                            ObserveDispatchResult(
                                state.Raise(ZLinkBackendSpotDispatchEvent.RouteReadable),
                                pending
                            );
                        if (!state.Subscriptions.IsEmpty)
                            ObserveDispatchResult(
                                state.Raise(ZLinkBackendSpotDispatchEvent.SubscribeReadable),
                                pending
                            );
                        if (pending.Count != 0)
                            await Task.WhenAll(pending).ConfigureAwait(false);
                    }
                )
            );
        }
    }

    // Registers the node-level route/channel dispatch sink. Node-addressed
    // (NodeSend/NodeRequest) and channel-addressed (ChannelSend/ChannelRequest)
    // records are owned by the node (ready-record OwnerKind == Node) — their
    // source spot rid is the remote sender's, so they cannot key a local per-spot
    // queue. They are delivered to this single node-level consumer, which routes
    // them to the MeshNode builder's registered route/channel handlers.
    public void SetNodeRouteHandler(
        Func<IReadOnlyList<ZLinkBackendRouteReceived>, CancellationToken, ValueTask> handler,
        ZLinkRuntimeTaskRunner? taskRunner = null
    )
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
        _applicationSource?.Signal();
    }

    private MeshReadyDomains TakePendingReadyDomains() =>
        (MeshReadyDomains)Interlocked.Exchange(ref _pendingReadyDomains, 0);

    private ValueTask RunTurn(
        ZLinkProcessExecutionPool.ApplicationTurn worker,
        CancellationToken cancellationToken
    )
    {
        if (cancellationToken.IsCancellationRequested)
            return ValueTask.CompletedTask;
        worker.Ready.MaximumRecords = ReadyOwnersPerBatch;
        var draining = DrainResidueAsync(
            worker.Ready,
            worker.Received,
            worker.Pending,
            cancellationToken
        );
        if (!draining.IsCompletedSuccessfully)
        {
            var completion = CompleteSuspendedTurnAsync(
                draining,
                worker.Ready,
                worker.Received,
                worker.Pending
            );
            worker.Transfer();
            _applicationSource!.Retain(completion);
            return ValueTask.CompletedTask;
        }
        (worker.Ready, worker.Received, worker.Pending) = draining.Result;
        for (var index = worker.Pending.Count - 1; index >= 0; index--)
        {
            if (!worker.Pending[index].IsCompleted)
                continue;
            ObserveDispatchResult(worker.Pending[index]);
            worker.Pending.RemoveAt(index);
        }
        if (worker.Pending.Count == 0)
            return ValueTask.CompletedTask;
        var pending = worker.Pending;
        worker.Pending = [];
        _applicationSource!.Retain(new ValueTask(Task.WhenAll(pending)));
        return ValueTask.CompletedTask;
    }

    private static async ValueTask CompleteSuspendedTurnAsync(
        ValueTask<(MeshReadyBatch, MeshReceiveBatch, List<Task>)> draining,
        MeshReadyBatch ready,
        MeshReceiveBatch received,
        List<Task> pending
    )
    {
        try
        {
            (ready, received, pending) = await draining.ConfigureAwait(false);
        }
        finally
        {
            ready.Dispose();
            received.Dispose();
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
                "mesh-application-worker",
                exception
            );
        }
    }

    private static void ObserveDispatchResult(ValueTask result, List<Task> pending)
    {
        if (result.IsCompletedSuccessfully)
            result.GetAwaiter().GetResult();
        else
            pending.Add(result.AsTask());
    }

    private static void ObserveDispatchResult(
        (ValueTask Completion, Func<CancellationToken, ValueTask>? Drain) result,
        List<Task> pending
    )
    {
        // Submission has returned. The queue reserved this wake while committing
        // its first item; this worker now owns that same drain and its async result.
        if (result.Drain is { } drain)
            ObserveDispatchResult(drain(CancellationToken.None), pending);
        ObserveDispatchResult(result.Completion, pending);
    }

    private async ValueTask<(MeshReadyBatch, MeshReceiveBatch, List<Task>)> DrainResidueAsync(
        MeshReadyBatch readyBatch,
        MeshReceiveBatch receiveBatch,
        List<Task> pending,
        CancellationToken cancellationToken
    )
    {
        var domains = TakePendingReadyDomains();
        if (domains != MeshReadyDomains.None)
        {
            readyBatch.Reset();
            bool residue;
            try
            {
                // Claims are nonblocking; source readiness reserves the next turn.
                // A claim never occupies a physical worker while waiting for ingress.
                residue = await _node
                    .DrainReadyAsync(domains, readyBatch, RecvFlags.DontWait)
                    .ConfigureAwait(false);
            }
            catch (ObjectDisposedException)
            {
                return (readyBatch, receiveBatch, pending);
            }
            catch (ZlinkException)
            {
                return (readyBatch, receiveBatch, pending);
            }

            if (residue)
                SignalReady(domains);
            for (var i = 0; i < readyBatch.Count; i++)
            {
                var dispatch = DrainClaimAsync(
                    readyBatch,
                    i,
                    receiveBatch,
                    pending,
                    cancellationToken
                );
                if (dispatch.IsCompletedSuccessfully)
                    dispatch.GetAwaiter().GetResult();
                else
                {
                    // Preparation may read the Store. Transfer its batches and results
                    // together: its continuation must not mutate the worker's next
                    // batch/list. The retained claim preserves this owner's order.
                    var suspended = JoinClaimAsync(dispatch, readyBatch, receiveBatch, pending);
                    readyBatch = new MeshReadyBatch { MaximumRecords = ReadyOwnersPerBatch };
                    receiveBatch = new MeshReceiveBatch();
                    pending = new List<Task>();
                    ObserveDispatchResult(suspended, pending);
                }
            }
            readyBatch.Reset();
        }
        return (readyBatch, receiveBatch, pending);
    }

    private static async ValueTask JoinClaimAsync(
        ValueTask dispatch,
        MeshReadyBatch readyBatch,
        MeshReceiveBatch receiveBatch,
        List<Task> pending
    )
    {
        try
        {
            await dispatch.ConfigureAwait(false);
        }
        finally
        {
            readyBatch.Dispose();
            receiveBatch.Dispose();
            if (pending.Count != 0)
                await Task.WhenAll(pending).ConfigureAwait(false);
        }
    }

    private async ValueTask DrainClaimAsync(
        MeshReadyBatch readyBatch,
        int index,
        MeshReceiveBatch receiveBatch,
        List<Task> pending,
        CancellationToken cancellationToken
    )
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
        if (
            string.IsNullOrEmpty(ownerSpotId)
            && readyRecord.OwnerKind == MeshOwnerKind.Actor
            && readyRecord.Actor.ActorId is { Length: > 0 } ownerActorId
        )
            try
            {
                if (_node.ActorLookup(ownerActorId, out var ownerLocation))
                    ownerSpotId = ownerLocation.SpotId;
            }
            catch (ZlinkException) { }
        var claim = readyBatch.TakeClaim(index);

        List<ZLinkBackendRouteReceived>? nodeRoutes = null;
        SpotDispatchState? routeDispatch = null;
        try
        {
            receiveBatch.Reset();
            // Release every claim after one bounded turn. Each application
            // record already carries the permit its source obtained before
            // posting it to the owner mailbox.
            receiveBatch.MaximumRecords = ZLinkReceiveBatchBudget.MaximumRecords;
            receiveBatch.MaximumBytes = ZLinkReceiveBatchBudget.MaximumBytes;
            receiveBatch.StartedAt = Stopwatch.GetTimestamp();
            if (!await claim.ReceiveAsync(receiveBatch, RecvFlags.DontWait).ConfigureAwait(false))
                return;

            try
            {
                for (var record = 0; record < receiveBatch.Count; record++)
                {
                    var current = receiveBatch[record];
                    var requestSource = AcceptsRequestSource(current.Kind)
                        ? await AcceptRequestSourceAsync(
                                current.SourceNodeRid,
                                current.SourceBindingGeneration,
                                cancellationToken
                            )
                            .ConfigureAwait(false)
                        : null;
                    DispatchRecord(
                        receiveBatch,
                        record,
                        current,
                        requestSource,
                        ownerSpotId,
                        readyRecord.Actor,
                        ref nodeRoutes,
                        pending,
                        ref routeDispatch
                    );

                    // A malformed or unsupported record may not transfer its
                    // owner. Return both the payload owner and its permit in
                    // this same bounded turn.
                    if (receiveBatch.GetApplicationJobAdmission(record) is not null)
                        receiveBatch.TakePayloadOwner(record)?.Dispose();
                }
            }
            finally
            {
                if (routeDispatch is not null)
                    ObserveDispatchResult(
                        routeDispatch.Raise(ZLinkBackendSpotDispatchEvent.RouteReadable),
                        pending
                    );
            }
            // Start this owner's records in claim order. A suspended handler's
            // result remains with the worker, while the claim returns for the
            // next bounded batch and other owners run on their own workers.
            var dispatch = DispatchNodeRoutes(nodeRoutes, cancellationToken);
            // The asynchronous handler owns this list until its result completes.
            // Clearing it here invalidates an enumerator suspended inside the handler.
            nodeRoutes = null;
            ObserveDispatchResult(dispatch, pending);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested) { }
        catch (ObjectDisposedException) { }
        catch (Exception exception)
        {
            if (_applicationTaskRunner is null)
                throw;
            _applicationTaskRunner.ErrorSink.ReportRuntimeTaskException(
                "mesh-application-worker",
                exception
            );
        }
        finally
        {
            if (nodeRoutes is not null)
                foreach (var received in nodeRoutes)
                    received.Dispose();
        }
    }

    private static bool AcceptsRequestSource(MeshRecordKind kind) =>
        kind
            is MeshRecordKind.SpotSend
                or MeshRecordKind.SpotRequest
                or MeshRecordKind.ActorSend
                or MeshRecordKind.ActorRequest;

    private void DispatchRecord(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource,
        string ownerSpotId,
        ActorRef ownerActor,
        ref List<ZLinkBackendRouteReceived>? nodeRoutes,
        List<Task> pending,
        ref SpotDispatchState? routeDispatch
    )
    {
        switch (record.Kind)
        {
            case MeshRecordKind.Completion:
                ResolveCompletion(batch, index, record);
                return;
            case MeshRecordKind.NodeSend:
            case MeshRecordKind.NodeRequest:
            case MeshRecordKind.ChannelSend:
            case MeshRecordKind.ChannelRequest:
                EnqueueNodeRoute(batch, index, record, ref nodeRoutes);
                return;
            case MeshRecordKind.SpotSend:
            case MeshRecordKind.SpotRequest:
                EnqueueRoute(batch, index, record, requestSource, ownerSpotId, ref routeDispatch);
                return;
            case MeshRecordKind.SpotMulticast:
                EnqueueSubscribe(batch, index, record, ownerSpotId, pending);
                return;
            case MeshRecordKind.SpotControl:
                EnqueueSpotControl(batch, index, record, ownerSpotId, pending);
                return;
            case MeshRecordKind.ActorSend:
            case MeshRecordKind.ActorRequest:
                EnqueueActor(batch, index, record, requestSource, ownerSpotId, ownerActor, pending);
                return;
        }
    }

    private void ResolveCompletion(MeshReceiveBatch batch, int index, MeshReceiveRecord record)
    {
        var parts = record.PartCount > 0 ? batch.RetainMessage(index) : Array.Empty<Message>();
        _completions.Complete(record, parts);
    }

    private void EnqueueRoute(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource,
        string ownerSpotId,
        ref SpotDispatchState? routeDispatch
    )
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // and do not deliver it to a handler (spec 03 §3). The batch reset
        // releases the Core-owned parts we never retained.
        if (!TryDecodeMetadata(record, out var metadata))
            return;

        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId
        );
        var reply = record.Kind
            is MeshRecordKind.NodeRequest
                or MeshRecordKind.ChannelRequest
                or MeshRecordKind.SpotRequest
            ? record.CaptureReplyRoute()
            : null;
        var parts = RetainParts(batch, index);
        var payloadOwner = batch.TakePayloadOwner(index);
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
            requestSource: requestSource,
            deadlineUnixMs: ZLinkMeshRecordAdapters.NormalizeDeadline(record.DeadlineUnixMs),
            payloadOwner: payloadOwner,
            instanceIntent: record.InstanceIntent
        );
        state.Routes.Enqueue(route);
        routeDispatch = state;
    }

    // Node/channel-addressed records (owned by the node). Requests carry the reply
    // token exactly like the per-spot route plane; channel records also carry the
    // addressed channel name so the node dispatcher can select the channel
    // membership's handler set. Delivered to the node-level route consumer; if none
    // is registered (no MeshNode route/channel handlers), the retained parts are
    // released so a dropped record cannot leak.
    private void EnqueueNodeRoute(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        ref List<ZLinkBackendRouteReceived>? nodeRoutes
    )
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // (spec 03 §3). No parts are retained before this point.
        if (!TryDecodeMetadata(record, out var metadata))
            return;

        var reply = record.Kind is MeshRecordKind.NodeRequest or MeshRecordKind.ChannelRequest
            ? record.CaptureReplyRoute()
            : null;
        var parts = record.ApplicationPayloadView is null
            ? RetainParts(batch, index)
            : Array.Empty<Message>();
        var payloadOwner = batch.TakePayloadOwner(index);
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
            applicationPayloadView: record.ApplicationPayloadView
        );
        if (_nodeRouteHandler is null)
        {
            received.Dispose();
            return;
        }

        (nodeRoutes ??= new(batch.Count)).Add(received);
    }

    private ValueTask DispatchNodeRoutes(
        List<ZLinkBackendRouteReceived>? nodeRoutes,
        CancellationToken cancellationToken
    )
    {
        if (nodeRoutes is null)
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

    private void EnqueueSubscribe(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        string ownerSpotId,
        List<Task> pending
    )
    {
        // Malformed application metadata is a protocol error: reject the ingress
        // (spec 03 §3). The same publish snapshot is delivered to every matching
        // Spot handler, so the decoded view is immutable and shared.
        if (!TryDecodeMetadata(record, out var metadata))
            return;

        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId
        );
        var parts = RetainParts(batch, index);
        var message = new ZLinkBackendSubscribeMessage(
            record.ChannelName ?? string.Empty,
            record.Topic ?? string.Empty,
            parts,
            metadata,
            batch.TakePayloadOwner(index)
        );
        state.Subscriptions.Enqueue(message);
        ObserveDispatchResult(
            state.Raise(ZLinkBackendSpotDispatchEvent.SubscribeReadable),
            pending
        );
    }

    // Decodes the record's application-metadata frame into an immutable snapshot.
    // Returns false only when the frame is present but malformed, so callers
    // drop the record as a protocol error rather than deliver it.
    private static bool TryDecodeMetadata(
        MeshReceiveRecord record,
        out ZLinkMessageMetadata metadata
    )
    {
        var frame = record.ApplicationMetadata;
        if (frame is null || frame.Length == 0)
        {
            metadata = ZLinkMessageMetadata.Empty;
            return true;
        }

        return ZLinkMeshMetadataCodec.TryDecode(frame, out metadata);
    }

    private void EnqueueSpotControl(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        string ownerSpotId,
        List<Task> pending
    )
    {
        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId
        );
        if (record.OperationKind == MeshOperationKind.ActorJoin)
        {
            // Actor-join admission record: build a framework join request.
            var join = ZLinkMeshRecordAdapters.ToActorJoinRequest(batch, index, record);
            if (batch.TakePayloadOwner(index) is { } payloadOwner)
                join.AttachPayloadOwner(payloadOwner);
            state.ActorJoins.Enqueue(join);
            ObserveDispatchResult(
                state.Raise(ZLinkBackendSpotDispatchEvent.ActorJoinReadable),
                pending
            );
            return;
        }

        if (record.ActorControl is { } control)
        {
            var lifecycle = ZLinkMeshRecordAdapters.ToLifecycleEvent(control);
            if (lifecycle is { } value)
            {
                state.Lifecycles.Enqueue(value);
                ObserveDispatchResult(
                    state.Raise(ZLinkBackendSpotDispatchEvent.ActorLifecycleReadable),
                    pending
                );
            }
        }
    }

    private void EnqueueActor(
        MeshReceiveBatch batch,
        int index,
        MeshReceiveRecord record,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource,
        string ownerSpotId,
        ActorRef ownerActor,
        List<Task> pending
    )
    {
        var requestId = record.Kind == MeshRecordKind.ActorRequest ? record.ReplyRouteId : 0;
        var state = ResolveSpotState(
            string.IsNullOrEmpty(ownerSpotId) ? record.SourceSpotId : ownerSpotId
        );
        var directReply = requestId == 0 ? null : record.CaptureReplyRoute();
        var parts = ZLinkMeshRecordAdapters.ToActorParts(
            batch,
            index,
            record,
            ownerActor,
            requestId,
            requestSource,
            directReply
        );
        if (parts.Count == 0)
            return;
        // The record's permit carries its origin. Without a Framework queue the
        // record has no permit, and binding ingress is remote.
        var origin =
            batch.GetApplicationJobAdmission(index)?.Origin ?? ZLinkApplicationJobOrigin.Remote;
        ObserveDispatchResult(
            state.RaiseActor(origin, parts, batch.TakePayloadOwner(index)),
            pending
        );
    }

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
        if (stopped is null)
            return;
        _node.CloseSubmissions();
        var (stop, applicationSource) = stopped.Value;
        stop?.Cancel();

        Exception? loopFailure = null;
        if (applicationSource is not null)
            try
            {
                await applicationSource.DisposeAsync().ConfigureAwait(false);
            }
            catch (OperationCanceledException) { }
            catch (Exception exception)
            {
                loopFailure = exception;
            }

        var failures = new ZLinkFailureCollector(loopFailure);
        failures.Capture(() => _completions.FailAll(RequestResult.Terminated));
        await failures
            .CaptureAsync(() => new ValueTask(_completions.CompletionDrained))
            .ConfigureAwait(false);
        failures.Capture(() => stop?.Dispose());
        failures.ThrowIfAny();
    }

    private (
        CancellationTokenSource? Stop,
        ZLinkProcessExecutionPool.ApplicationSource? ApplicationSource
    )? StopOnLane()
    {
        if (_disposed)
            return null;
        _disposed = true;
        return (_stop, _applicationSource);
    }

    // Per-spot decoded-record queues plus the registered dispatch-event handler.
    internal sealed class SpotDispatchState
    {
        public Func<
            ZLinkBackendSpotDispatchInfo,
            (ValueTask Completion, Func<CancellationToken, ValueTask>? Drain)
        >? DispatchHandler { get; set; }

        public ConcurrentQueue<ZLinkBackendRouteReceived> Routes { get; } = new();

        public ConcurrentQueue<ZLinkBackendSubscribeMessage> Subscriptions { get; } = new();

        public ConcurrentQueue<ZLinkBackendActorJoinRequest> ActorJoins { get; } = new();

        public ConcurrentQueue<ZLinkBackendSpotActorLifecycleEvent> Lifecycles { get; } = new();

        public (ValueTask Completion, Func<CancellationToken, ValueTask>? Drain) Raise(
            ZLinkBackendSpotDispatchEvent kind
        )
        {
            return DispatchHandler?.Invoke(new ZLinkBackendSpotDispatchInfo(null, kind))
                ?? (ValueTask.CompletedTask, null);
        }

        public (ValueTask Completion, Func<CancellationToken, ValueTask>? Drain) RaiseActor(
            ZLinkApplicationJobOrigin origin,
            IReadOnlyList<ZLinkBackendActorPart> parts,
            IDisposable? payloadOwner
        )
        {
            var handler = DispatchHandler;
            if (handler is null)
            {
                foreach (var part in parts)
                    part.Message.Dispose();
                payloadOwner?.Dispose();
                return (ValueTask.CompletedTask, null);
            }
            try
            {
                return handler(
                    new ZLinkBackendSpotDispatchInfo(
                        origin,
                        ZLinkBackendSpotDispatchEvent.ActorReadable,
                        ActorParts: parts,
                        ActorPayloadOwner: payloadOwner
                    )
                );
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
