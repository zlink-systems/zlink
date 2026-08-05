using System.Diagnostics;
using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Spots;

internal sealed record ZLinkSpotRelocationSeal(
    ZLinkSpotExecutionRelocationSeal QueueSeal,
    IReadOnlyList<ZLinkRelocationLogicalTimer> LogicalTimers);

internal sealed record ZLinkSpotRelocationApplicationState(
    ReadOnlyMemory<byte> SpotState,
    IReadOnlyDictionary<string, ReadOnlyMemory<byte>> ActorStates);

internal sealed partial class ZLinkSpotActivation
{
    private const int MaxConcurrentRelocationAdapterCallbacks = 8;
    private readonly object _relocationReadyGate = new();
    private readonly object _messageFollowPendingGate = new();
    private readonly Queue<PendingMessageFollowRoute> _messageFollowPending = new();
    private ZLinkSpotMessageFollow? _messageFollow;
    private long _messageFollowPendingBytes;
    private bool _holdIngressForMessageFollow;
    private RelocationReadySealRequest? _relocationReadyRequest;
    private bool _relocationReadyCompletionPending;

    internal object RuntimeExecutionOwner => _runtime.ExecutionOwner;

    public CancellationToken StopToken => _stopSource.Token;

    internal void CancelActiveOperations()
    {
        _stopSource.Cancel();
    }

    internal void RequestStop()
    {
        _serial.RequestStop();
        _stopSource.Cancel();
    }

    public ValueTask DisposeAsync()
    {
        TaskCompletionSource completion;
        lock (_lifecycleGate)
        {
            if (_finalization is not null) return new ValueTask(_finalization);

            Volatile.Write(ref _disposed, 1);
            completion = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            _finalization = completion.Task;
        }

        _ = CompleteFinalizationAsync(completion);
        return new ValueTask(completion.Task);
    }

    private async Task CompleteFinalizationAsync(TaskCompletionSource completion)
    {
        try
        {
            await FinalizeAsync().ConfigureAwait(false);
            completion.TrySetResult();
        }
        catch (Exception exception)
        {
            completion.TrySetException(exception);
        }
    }

    private async Task FinalizeAsync()
    {
        var failures = new List<Exception>();
        Capture(RequestStop);
        Capture(DisposePendingMessageFollowRoutes);
        await CaptureAsync(_timers.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_serial.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_outbound.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(NativeSpot.DisposeAsync).ConfigureAwait(false);
        Capture(_stopSource.Dispose);
        await CaptureAsync(_handlerInstances.DisposeAsync).ConfigureAwait(false);
        await CaptureAsync(_scope.DisposeAsync).ConfigureAwait(false);
        ThrowFailures(failures);

        async ValueTask CaptureAsync(Func<ValueTask> cleanup)
        {
            try
            {
                await cleanup().ConfigureAwait(false);
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }

        void Capture(Action cleanup)
        {
            try
            {
                cleanup();
            }
            catch (Exception exception)
            {
                failures.Add(exception);
            }
        }
    }

    private static void ThrowFailures(IReadOnlyList<Exception> failures)
    {
        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1) throw new AggregateException(failures);
    }

    public ValueTask<IZLinkTimer> AddTimer<THandler>(
        string name,
        TimeSpan period,
        ZLinkTimerOptions? options = null,
        CancellationToken cancellationToken = default)
        where THandler : class
    {
        EnsureContextOperationAllowed();
        return _timers.AddAsync(
            name,
            period,
            options,
            typeof(THandler),
            Spot.GetType(),
            StopToken,
            DispatchTimerAsync,
            PublishTimerFailureAsync,
            cancellationToken);
    }

    public IZLinkWorkerCall<TResult> RunCpuWorker<TResult>(
        Func<CancellationToken, TResult> work)
    {
        EnsureContextOperationAllowed();
        ArgumentNullException.ThrowIfNull(work);
        return new ZLinkWorkerCall<TResult>(
            _runtime.WorkerPool,
            work,
            _runtime.ErrorSink);
    }

    public IZLinkWorkerCall<TResult> RunIoWorker<TResult>(
        Func<CancellationToken, ValueTask<TResult>> work)
    {
        EnsureContextOperationAllowed();
        ArgumentNullException.ThrowIfNull(work);
        return new ZLinkIoWorkerCall<TResult>(
            _runtime.WorkerPool.ShutdownToken,
            work,
            _runtime.ErrorSink);
    }

    internal Task<ZLinkSpotRelocationSeal> WaitForRelocationReadyTurnAsync(
        CancellationToken cancellationToken)
    {
        if (RelocationReadiness
            != ZLinkSpotRelocationReadinessMode.ApplicationSignaled)
            throw new InvalidOperationException(
                "Only ApplicationSignaled readiness waits for an application turn.");

        Task<ZLinkSpotRelocationSeal> signal;
        lock (_relocationReadyGate)
        {
            if (_relocationReadyRequest is not null)
                throw new InvalidOperationException(
                    "A relocation-ready turn is already pending.");
            _relocationReadyRequest = new RelocationReadySealRequest(
                cancellationToken);
            signal = _relocationReadyRequest.Completion.Task;
        }
        return signal.WaitAsync(cancellationToken);
    }

    internal void CompleteRelocationReadyTurn()
    {
        RelocationReadySealRequest? pending;
        lock (_relocationReadyGate)
        {
            pending = _relocationReadyRequest;
            if (pending is not null)
            {
                _relocationReadyCompletionPending = true;
                _relocationReadyRequest = null;
            }
        }

        if (pending is not null)
        {
            var seal = SealRelocationAsync(
                    allowActorClaims: false,
                    pending.CancellationToken)
                .AsTask();
            _ = CompleteRelocationReadySealAsync(pending, seal);
            return;
        }

        _ = QueueApplicationSerializedNext(
            static (activation, ct) =>
                activation.InvokeRelocationReadyCompletedAsync(
                    ZLinkSpotRelocationReadyOutcome.Continued,
                    ct),
            countAsRequest: false);
    }

    private static async Task CompleteRelocationReadySealAsync(
        RelocationReadySealRequest request,
        Task<ZLinkSpotRelocationSeal> seal)
    {
        try
        {
            request.Completion.TrySetResult(
                await seal.ConfigureAwait(false));
        }
        catch (OperationCanceledException)
            when (request.CancellationToken.IsCancellationRequested)
        {
            request.Completion.TrySetCanceled(request.CancellationToken);
        }
        catch (Exception exception)
        {
            request.Completion.TrySetException(exception);
        }
    }

    internal async ValueTask CompleteRelocationReadyAsync(
        ZLinkSpotRelocationReadyOutcome outcome,
        CancellationToken cancellationToken)
    {
        lock (_relocationReadyGate)
        {
            if (!_relocationReadyCompletionPending)
                return;
            _relocationReadyCompletionPending = false;
        }
        await InvokeRelocationReadyCompletedAsync(outcome, cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask CompleteRelocationReadyBeforeAbortAsync(
        ZLinkSpotRelocationSeal admissionSeal,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(admissionSeal);
        lock (_relocationReadyGate)
        {
            if (!_relocationReadyCompletionPending)
                return;
            _relocationReadyCompletionPending = false;
        }
        await _serial.ExecuteSealedRelocationAsync(
                admissionSeal.QueueSeal,
                static (activation, ct) =>
                    activation.InvokeRelocationReadyCompletedAsync(
                        ZLinkSpotRelocationReadyOutcome.Continued,
                        ct),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask InvokeTargetRelocationReadyCompletedAsync(
        ZLinkSpotRelocationSeal admissionSeal,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(admissionSeal);
        if (RelocationReadiness
            != ZLinkSpotRelocationReadinessMode.ApplicationSignaled)
            return;
        await _serial.ExecuteSealedRelocationAsync(
                admissionSeal.QueueSeal,
                static (activation, ct) =>
                    activation.InvokeRelocationReadyCompletedAsync(
                        ZLinkSpotRelocationReadyOutcome.Relocated,
                        ct),
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal void CancelRelocationReadyWait()
    {
        lock (_relocationReadyGate)
        {
            _relocationReadyRequest = null;
            _relocationReadyCompletionPending = false;
        }
    }

    private ValueTask InvokeRelocationReadyCompletedAsync(
        ZLinkSpotRelocationReadyOutcome outcome,
        CancellationToken cancellationToken) =>
        UserSpot.OnRelocationReadyCompletedAsync(
            new ZLinkSpotRelocationReadyCompletion(outcome),
            cancellationToken);

    private sealed class RelocationReadySealRequest(
        CancellationToken cancellationToken)
    {
        internal CancellationToken CancellationToken { get; } =
            cancellationToken;

        internal TaskCompletionSource<ZLinkSpotRelocationSeal> Completion
            { get; } = new(
                TaskCreationOptions.RunContinuationsAsynchronously);
    }

    ValueTask<bool> IZLinkSpotContext.CloseAsync(CancellationToken cancellationToken)
    {
        EnsureContextOperationAllowed();
        return _runtime.CloseCurrentSpotAsync(SpotId, cancellationToken);
    }

    ValueTask<bool> IZLinkInstanceSpotContext.CloseAsync(CancellationToken cancellationToken)
    {
        EnsureContextOperationAllowed();
        return _runtime.CloseCurrentSpotAsync(SpotId, cancellationToken);
    }

    internal void AttachNativeDispatch()
    {
        if (Interlocked.CompareExchange(
                ref _nativeDispatchAttached,
                1,
                0) != 0)
            return;

        RegisterWithoutSynchronizationContext(() =>
        {
            ZLinkSpotNativeDispatchRouter.Attach(
                NativeSpot,
                receivedMessages =>
                {
                    if (receivedMessages.Count == 0)
                    {
                        DrainNativeRoutes();
                        return;
                    }

                    foreach (var received in receivedMessages)
                        AdmitNativeRoute(received);
                },
                drain => drain?.Invoke(),
                () => QueueApplicationSerialized(
                    static (activation, ct) => activation.DispatchSubscriptionsAsync(ct),
                    countAsRequest: false,
                    () => QueueSerialized(
                        static (activation, ct) => activation._dispatcher.DiscardSubscriptionsAsync(ct))),
                () => QueueSerialized(static (activation, ct) =>
                    activation.DispatchActorJoinDrainTurnAsync(ct)),
                () => QueueSerialized(static (activation, ct) =>
                    activation.DispatchActorLifecycleDrainAsync(ct)),
                (actorParts, inboundDispatchLease) =>
                {
                    var dispatchable = ZLinkActorHandoffIngress.CaptureMovingFrames(
                        _runtime,
                        actorParts,
                        inboundDispatchLease);
                    if (dispatchable.Count == 0)
                    {
                        dispatchable.Dispose();
                        return;
                    }

                    if (!QueueActorFrames(dispatchable))
                        dispatchable.Dispose();
                });

            return 0;
        });
    }

    private void DrainNativeRoutes()
    {
        while (NativeSpot.RecvRoute(RecvFlags.DontWait) is { } received)
            AdmitNativeRoute(received);
    }

    private void AdmitNativeRoute(ZLinkBackendRouteReceived received)
    {
        if (ZLinkSpotActivationDispatcher.IsInfrastructureRoute(received))
        {
            QueueSerialized(
                static (activation, state, ct) =>
                    activation._dispatcher.DispatchRouteAsync(state, ct),
                received,
                received.Dispose);
            return;
        }

        switch (TryMessageFollow(received))
        {
            case ZLinkSpotMessageFollowResult.Followed:
                return;
            case ZLinkSpotMessageFollowResult.StaleRejected:
                ZLinkSpotActivationDispatcher
                    .RejectApplicationRouteForStaleMessageFollow(
                        received,
                        ChannelName);
                return;
            case ZLinkSpotMessageFollowResult.Full:
                ZLinkSpotActivationDispatcher
                    .RejectApplicationRouteForRelocation(
                        received,
                        ChannelName);
                return;
            case ZLinkSpotMessageFollowResult.NotApplicable:
                QueueApplicationRouteSerialized(received);
                return;
            default:
                throw new InvalidOperationException(
                    "Unknown Spot Message Follow result.");
        }
    }

    public async ValueTask<ZLinkSpotCreateResponse> InitializeAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        AttachNativeDispatch();
        var create = new SpotCreateCallState(request);
        await ExecuteSerializedAsync(
            static async (activation, state, ct) =>
            {
                state.Response = await activation.UserSpot.OnCreateAsync(state.Request, ct);
                if (!state.Response.Accepted) return;

                await activation.UserSpot.OnInitializeAsync(ct);
            },
            create,
            cancellationToken);
        return create.Response;
    }

    internal ValueTask InitializeInstanceAsync(CancellationToken cancellationToken)
    {
        if (Spot is not IZLinkInstanceSpot instance)
            throw new InvalidOperationException("The current activation is not an Instance Spot.");
        AttachNativeDispatch();
        return ExecuteSerializedAsync(
            static (activation, state, ct) => state.OnInitializeAsync(ct),
            instance,
            cancellationToken);
    }

    internal async ValueTask<InstanceSpotActivationTerminal>
        DispatchDurableActivationAsync(
            MeshOperationId operationId,
            RoutingId sourceNodeRid,
            string sourceSpotId,
            ZLinkServiceWireCodec.RequestSourceFence requestSource,
            ulong targetNodeGeneration,
            ulong authorityOwnerGeneration,
            ulong ownerLeaseGeneration,
            IReadOnlyList<ReadOnlyMemory<byte>> payload,
            ReadOnlyMemory<byte>? metadata,
            bool request,
            CancellationToken cancellationToken)
    {
        if (operationId == default
            || targetNodeGeneration == 0
            || authorityOwnerGeneration == 0
            || ownerLeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(
                nameof(operationId),
                "Durable Instance Spot dispatch requires an exact operation and authority fence.");
        if (requestSource.NodeRid != sourceNodeRid
            || requestSource.NodeGeneration == 0
            || string.IsNullOrWhiteSpace(requestSource.OwnerId)
            || requestSource.LeaseGeneration == 0)
            throw new ArgumentException(
                "The durable Instance Spot request-source fence does not match its source node.",
                nameof(requestSource));
        var parts = payload.Select(Message.From).ToArray();
        var completion = new TaskCompletionSource<InstanceSpotActivationTerminal>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var metadataBytes = metadata.HasValue
            ? metadata.Value
            : ReadOnlyMemory<byte>.Empty;
        if (!ZLinkMeshMetadataCodec.TryDecode(
                metadataBytes.Span,
                out var decodedMetadata))
            throw new ArgumentException(
                "Application metadata is malformed.",
                nameof(metadata));
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? replyCallback = null;
        if (request)
            replyCallback = (reply, _) =>
            {
                completion.TrySetResult(new InstanceSpotActivationTerminal(
                    RequestResult.Ok,
                    Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants
                        .FrameworkErrorCode.None,
                    reply.Select(static item =>
                            (ReadOnlyMemory<byte>)item.ToArray())
                        .ToArray()));
                return SubmitResult.Ok;
            };
        var received = new ZLinkBackendRouteReceived(
            parts,
            sourceNodeRid,
            sourceSpotId,
            request ? operationId.Low : null,
            replyCallback,
            metadata: decodedMetadata,
            operationId: operationId,
            targetNodeGeneration: targetNodeGeneration,
            authorityOwnerGeneration: authorityOwnerGeneration,
            ownerLeaseGeneration: ownerLeaseGeneration,
            sourceNodeGeneration: requestSource.NodeGeneration,
            requestSource: requestSource);
        int acceptedJournalLength;
        try
        {
            acceptedJournalLength = ZLinkSpotAcceptedJournal.MeasureEncodedLength(
                received,
                request ? operationId.Low : 0);
        }
        catch
        {
            received.Dispose();
            throw;
        }
        Func<ReadOnlyMemory<byte>> acceptedJournalFactory =
            () => ZLinkSpotAcceptedJournal.Encode(
                received,
                request ? operationId.Low : 0);

        var queued = QueueApplicationSerialized(
            static async (activation, state, ct) =>
            {
                try
                {
                    await activation._dispatcher.DispatchRouteAsync(
                            state.Received,
                            ct)
                        .ConfigureAwait(false);
                    if (!state.Request)
                        state.Completion.TrySetResult(new InstanceSpotActivationTerminal(
                            RequestResult.Ok,
                            Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants
                                .FrameworkErrorCode.None,
                            []));
                    else if (!state.Completion.Task.IsCompleted)
                        state.Completion.TrySetResult(new InstanceSpotActivationTerminal(
                            RequestResult.InternalError,
                            Systems.Zlink.Framework.Runtime.Protocol.ServiceWireConstants
                                .FrameworkErrorCode.RequestFailed,
                            []));
                }
                catch (Exception error)
                {
                    state.Completion.TrySetException(error);
                }
            },
            new DurableActivationDispatch(received, completion, request),
            acceptedJournalLength,
            acceptedJournalFactory,
            false,
            request,
            _ =>
            {
                received.Dispose();
                completion.TrySetException(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ShuttingDown,
                    "The Instance Spot activation queue stopped before admission."));
            },
            () =>
            {
                received.Dispose();
                completion.TrySetException(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Unavailable,
                    "The Instance Spot activation queue is relocating.",
                    ZLinkRetryAdvice.RetryAfterBackoff));
            },
            received.Dispose);
        if (!queued)
            return await completion.Task.ConfigureAwait(false);
        // Admission is durable at this point. Caller cancellation no longer
        // removes the accepted queue record or prevents terminal publication.
        return await completion.Task.ConfigureAwait(false);
    }

    public ValueTask SubmitActorAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message body,
        ZLinkSpotRelocationReplayAdmission? replayAdmission,
        CancellationToken cancellationToken)
    {
        var validatedReplay = ValidateRelocationReplayAdmission(
            runtimeState,
            replayAdmission);
        return _actorDispatchSubmitter.Async(
            actor,
            runtimeState,
            header,
            body,
            validatedReplay?.Seal.QueueSeal,
            validatedReplay?.QueueReservation,
            cancellationToken);
    }

    private sealed record DurableActivationDispatch(
        ZLinkBackendRouteReceived Received,
        TaskCompletionSource<InstanceSpotActivationTerminal> Completion,
        bool Request);

    public ValueTask<ZLinkActorReply> SubmitActorForReplyAsync(
        IZLinkActor actor,
        ZLinkActorRuntimeState runtimeState,
        ZlinkStreamHeader header,
        Message body,
        ZLinkSpotRelocationReplayAdmission? replayAdmission,
        CancellationToken cancellationToken)
    {
        var validatedReplay = ValidateRelocationReplayAdmission(
            runtimeState,
            replayAdmission);
        return _actorDispatchSubmitter.SubmitForReplyAsync(
            actor,
            runtimeState,
            header,
            body,
            validatedReplay?.Seal.QueueSeal,
            validatedReplay?.QueueReservation,
            cancellationToken);
    }

    internal ZLinkSpotRelocationActorQueueReservation
        ReserveRelocationActorReplay(
            ZLinkSpotRelocationSeal seal,
            string actorId)
    {
        ArgumentNullException.ThrowIfNull(seal);
        return _serial.ReserveRelocationActorQueue(
            seal.QueueSeal,
            actorId);
    }

    private ZLinkSpotRelocationReplayAdmission?
        ValidateRelocationReplayAdmission(
            ZLinkActorRuntimeState runtimeState,
            ZLinkSpotRelocationReplayAdmission? replayAdmission)
    {
        if (replayAdmission is null)
            return null;
        if (!ReferenceEquals(replayAdmission.Activation, this)
            || !runtimeState.Handoff.IsAuthorityCommitted(
                replayAdmission.HandoffId))
            throw new InvalidOperationException(
                "SPOT relocation replay admission does not match the target activation and Actor handoff.");
        return replayAdmission;
    }

    public async ValueTask CloseAsync(CancellationToken cancellationToken)
    {
        await CloseAsync(
                ZLinkSpotCloseReason.ExplicitClose,
                DateTimeOffset.UtcNow + DefaultRequestTimeout,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal async ValueTask CloseAsync(
        ZLinkSpotCloseReason reason,
        DateTimeOffset deadline,
        CancellationToken cancellationToken)
    {
        if (Interlocked.Exchange(ref _closingInvoked, 1) != 0)
            return;
        _ = await _serial.ExecuteQuiescentLifecycleAsync(
                async (activation, ct) =>
                {
                    await activation.InvokeClosingAsync(reason, deadline)
                        .ConfigureAwait(false);
                    return true;
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal ValueTask InvokeRelocationClosingAfterCommitAsync(
        DateTimeOffset deadline)
    {
        if (Interlocked.Exchange(ref _closingInvoked, 1) != 0)
            return ValueTask.CompletedTask;
        return InvokeClosingAsync(
            ZLinkSpotCloseReason.RelocationOut,
            deadline);
    }

    internal async ValueTask
        InvokePerActorRelocationClosingAfterDrainAsync(
            Task messageFollowDrained,
            CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(messageFollowDrained);
        var plan = PerActorShellRelocationPlan
                   ?? throw new InvalidOperationException(
                       "Only a relocated PerActor User Spot can complete source shell closing.");
        try
        {
            await Task.WhenAll(
                    messageFollowDrained,
                    WaitForPerActorMembersDrainedAsync(cancellationToken))
                .WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
        catch (OperationCanceledException)
            when (cancellationToken.IsCancellationRequested)
        {
            // Detached runtime cleanup may cancel before Message Follow or the
            // last member drains. The source shell is still valid here, so its
            // terminal lifecycle callback must precede scope teardown.
            await InvokeRelocationClosingAfterCommitAsync(plan.ClosingDeadline)
                .ConfigureAwait(false);
            return;
        }
        await InvokeRelocationClosingAfterCommitAsync(plan.ClosingDeadline)
            .ConfigureAwait(false);
    }

    internal async ValueTask<bool> TryCloseIfNoActorsAsync(
        ZLinkSpotCloseReason reason,
        DateTimeOffset deadline,
        bool requireNoActors,
        CancellationToken cancellationToken)
    {
        if (reason == ZLinkSpotCloseReason.IdleEvicted
            && (JoinedActorCount != 0 || HasIdleRelocationParticipation))
            return false;

        return await _serial.ExecuteQuiescentLifecycleAsync(
                async (activation, ct) =>
                {
                    //  Shutdown은 member actor가 남아 있어도 closing callback을
                    //  알리고 정리해야 한다(spec 28 §178). 가드를 무조건 걸면
                    //  callback조차 불리지 않는다. Relocate와 일반 close는
                    //  actor를 옮긴 뒤 닫으므로 가드를 그대로 유지한다.
                    if (requireNoActors && activation._actors.Count > 0)
                        return false;
                    if (reason == ZLinkSpotCloseReason.IdleEvicted
                        && activation._serial.HasPendingApplicationWork)
                        return false;

                    if (Interlocked.Exchange(ref activation._closingInvoked, 1) == 0)
                        await activation.InvokeClosingAsync(reason, deadline)
                            .ConfigureAwait(false);
                    return true;
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal bool HasIdleRelocationParticipation
    {
        get
        {
            if (_serial.HasRelocationBarrier
                || PerActorShellRelocationPlan is not null
                || Volatile.Read(ref _messageFollow) is not null)
                return true;
            lock (_messageFollowPendingGate)
                return _holdIngressForMessageFollow
                       || _messageFollowPending.Count != 0;
        }
    }

    internal bool HasPendingApplicationWork => _serial.HasPendingApplicationWork;

    internal bool HasRelocationBarrier => _serial.HasRelocationBarrier;

    internal long LastApplicationWorkCompletedAt =>
        _serial.LastApplicationWorkCompletedAt;

    private ValueTask InvokeClosingAsync(
        ZLinkSpotCloseReason reason,
        DateTimeOffset deadline)
    {
        return Spot switch
        {
            IZLinkSpot user => ZLinkSpotClosingInvocation.InvokeAsync(
                user.OnClosingAsync,
                reason,
                deadline),
            IZLinkInstanceSpot instance => ZLinkSpotClosingInvocation.InvokeAsync(
                instance.OnClosingAsync,
                reason,
                deadline),
            _ => throw new InvalidOperationException("The SPOT lifecycle is not attached.")
        };
    }

    private ValueTask ExecuteSerializedAsync(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        CancellationToken cancellationToken)
    {
        return _serial.ExecuteAsync(operation, cancellationToken);
    }

    private ValueTask ExecuteSerializedAsync<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        return _serial.ExecuteAsync(operation, state, cancellationToken);
    }

    private async ValueTask ExecuteApplicationSerializedAsync<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        CancellationToken cancellationToken)
    {
        if (!_runtime.TryEnterInboundOperation(countAsRequest: false, out var lease))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "SPOT application admission is sealed for drain.");
        using (lease)
            await _serial.ExecuteAsync(operation, state, cancellationToken).ConfigureAwait(false);
    }

    private bool QueueSerialized(Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation)
    {
        return _serial.QueueLifecycle(operation);
    }

    private bool QueueSerialized<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        Action? onSkipped = null)
    {
        var capturedOp = operation;
        var capturedState = state;
        return _serial.QueueLifecycle(
            (activation, ct) => capturedOp(activation, capturedState, ct),
            onSkipped);
    }

    private bool QueueApplicationSerialized(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        bool countAsRequest,
        Action? onRejected = null)
    {
        if (!_runtime.TryEnterInboundOperation(countAsRequest, out var lease))
        {
            onRejected?.Invoke();
            ReportUnobservedInboundAdmission(
                "spot-inbound-admission",
                onRejected is null);
            return false;
        }

        var admission = _serial.QueueWithAdmission(
            async (activation, ct) =>
            {
                using (lease)
                    await operation(activation, ct).ConfigureAwait(false);
            },
            () =>
            {
                lease.Dispose();
                onRejected?.Invoke();
            },
            reportUnobservedAdmission: onRejected is null);
        if (admission != ZLinkSerialPostAdmission.Accepted)
            lease.Dispose();
        return admission == ZLinkSerialPostAdmission.Accepted;
    }

    private bool QueueApplicationSerializedNext(
        Func<ZLinkSpotActivation, CancellationToken, ValueTask> operation,
        bool countAsRequest,
        Action? onRejected = null)
    {
        if (!_runtime.TryEnterInboundOperation(countAsRequest, out var lease))
        {
            onRejected?.Invoke();
            ReportUnobservedInboundAdmission(
                "spot-inbound-admission-next",
                onRejected is null);
            return false;
        }

        var admission = _serial.QueueNextWithAdmission(
            async (activation, ct) =>
            {
                using (lease)
                    await operation(activation, ct).ConfigureAwait(false);
            },
            () =>
            {
                lease.Dispose();
                onRejected?.Invoke();
            },
            reportUnobservedAdmission: onRejected is null);
        if (admission != ZLinkSerialPostAdmission.Accepted)
            lease.Dispose();
        return admission == ZLinkSerialPostAdmission.Accepted;
    }

    private bool QueueApplicationSerialized<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        bool countAsRequest,
        Action? onRejected = null)
    {
        if (!_runtime.TryEnterInboundOperation(countAsRequest, out var lease))
        {
            onRejected?.Invoke();
            return false;
        }

        var queued = QueueSerialized(
            async (activation, captured, ct) =>
            {
                using (lease)
                    await operation(activation, captured, ct).ConfigureAwait(false);
            },
            state,
            () =>
            {
                lease.Dispose();
                onRejected?.Invoke();
            });
        if (!queued) lease.Dispose();
        return queued;
    }

    private void ReportUnobservedInboundAdmission(
        string operation,
        bool unobserved)
    {
        if (!unobserved)
            return;
        _runtime.ErrorSink.ReportRuntimeTaskException(
            operation,
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "SPOT application admission was closed before the work was queued."));
    }

    private bool QueueApplicationSerialized<TState>(
        Func<ZLinkSpotActivation, TState, CancellationToken, ValueTask> operation,
        TState state,
        int acceptedJournalLength,
        Func<ReadOnlyMemory<byte>> acceptedJournalFactory,
        bool previousOwnerMessageFollow,
        bool countAsRequest,
        Action<ZLinkAcceptedWorkAdmission> onRejected,
        Action onMoving,
        Action relocationRelease)
    {
        if (!_runtime.TryEnterInboundOperation(countAsRequest, out var lease))
        {
            onRejected(ZLinkAcceptedWorkAdmission.Closed);
            return false;
        }

        var capturedOperation = operation;
        var capturedState = state;
        var released = 0;
        void ReleaseForRelocation()
        {
            if (Interlocked.Exchange(ref released, 1) != 0) return;
            lease.Dispose();
            relocationRelease();
        }

        var admission = _serial.QueueAccepted(
            acceptedJournalLength,
            acceptedJournalFactory,
            async (activation, ct) =>
            {
                using (lease)
                    await capturedOperation(activation, capturedState, ct).ConfigureAwait(false);
            },
            ReleaseForRelocation,
            previousOwnerMessageFollow,
            out _);
        if (admission == ZLinkAcceptedWorkAdmission.Accepted)
            return true;

        lease.Dispose();
        if (admission == ZLinkAcceptedWorkAdmission.RelocationMoving)
            onMoving();
        else
            onRejected(admission);
        return false;
    }

    private bool QueueApplicationRouteSerialized(
        ZLinkBackendRouteReceived received)
    {
        var replyRouteId = 0UL;
        if (received.CanReply)
        {
            if (received.OperationId == default
                || received.RequestSeq is not { } correlation
                || correlation == 0
                || correlation != received.OperationId.Low)
            {
                received.Dispose();
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    "A Spot request has no source-owned reply correlation.");
            }
            replyRouteId = correlation;
        }
        int acceptedJournalLength;
        try
        {
            acceptedJournalLength = ZLinkSpotAcceptedJournal.MeasureEncodedLength(
                received,
                replyRouteId);
        }
        catch
        {
            received.Dispose();
            throw;
        }
        Func<ReadOnlyMemory<byte>> acceptedJournalFactory =
            () => ZLinkSpotAcceptedJournal.Encode(received, replyRouteId);

        return QueueApplicationSerialized(
            static (activation, state, ct) =>
                activation.DispatchQueuedApplicationRouteAsync(state, ct),
            received,
            acceptedJournalLength,
            acceptedJournalFactory,
            received.MessageFollowHopCount != 0,
            received.CanReply,
            admission =>
            {
                ZLinkSpotActivationDispatcher.RejectApplicationRouteForDrain(
                    received,
                    ChannelName,
                    admission,
                    received.SourceNodeRid is null
                    || received.SourceNodeRid == NodeRid);
            },
            () =>
            {
                if (!TryHoldForMessageFollow(
                        received,
                        acceptedJournalLength))
                    ZLinkSpotActivationDispatcher
                        .RejectApplicationRouteForRelocation(
                            received,
                            ChannelName);
            },
            received.Dispose);
    }

    private ValueTask DispatchQueuedApplicationRouteAsync(
        ZLinkBackendRouteReceived received,
        CancellationToken cancellationToken)
    {
        // A route can enter the serial queue immediately before relocation
        // seals the Spot. Re-evaluate Message Follow at execution time so
        // that queued work cannot run on the source after authority moved.
        switch (TryMessageFollow(received))
        {
            case ZLinkSpotMessageFollowResult.Followed:
                return ValueTask.CompletedTask;
            case ZLinkSpotMessageFollowResult.StaleRejected:
                ZLinkSpotActivationDispatcher
                    .RejectApplicationRouteForStaleMessageFollow(
                        received,
                        ChannelName);
                return ValueTask.CompletedTask;
            case ZLinkSpotMessageFollowResult.Full:
                ZLinkSpotActivationDispatcher
                    .RejectApplicationRouteForRelocation(
                        received,
                        ChannelName);
                return ValueTask.CompletedTask;
            case ZLinkSpotMessageFollowResult.NotApplicable:
                return _dispatcher.DispatchRouteAsync(
                    received,
                    cancellationToken);
            default:
                throw new InvalidOperationException(
                    "Unknown Spot Message Follow result.");
        }
    }

    private bool QueueActorFrames(ZLinkSpotActorFrameBatch frames)
    {
        if (!_runtime.TryEnterInboundOperation(countAsRequest: false, out var lease))
            return false;

        if (_serial.TryRunDetached(
                "user-spot-actor-frames",
                async ct =>
                {
                    using (lease)
                        await _dispatcher.DispatchActorFramesAsync(frames, _serial, ct)
                            .ConfigureAwait(false);
                }))
            return true;

        lease.Dispose();
        return false;
    }


    private async ValueTask DispatchSubscriptionsAsync(CancellationToken cancellationToken)
    {
        await _dispatcher
            .DispatchSubscriptionsAsync(cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask DispatchActorLifecycleDrainAsync(CancellationToken cancellationToken)
    {
        var startedAt = Stopwatch.GetTimestamp();
        var count = 0;
        long bytes = 0;
        while (true)
        {
            if (ZLinkReceiveBatchBudget.IsExhausted(count, bytes, startedAt))
            {
                if (!cancellationToken.IsCancellationRequested)
                    QueueSerialized(static (activation, ct) =>
                        activation.DispatchActorLifecycleDrainAsync(ct));
                return;
            }
            var lifecycle = NativeSpot.RecvActorLifecycle(RecvFlags.DontWait);
            if (lifecycle is null) return;
            count++;
            bytes = checked(
                bytes + (lifecycle.Value.Info.CurrentActor?.ActorId?.Length ?? 0));
            if (lifecycle.Value.Kind != ZLinkBackendActorLifecycleEventKind.Disconnected)
                continue;

            var actorId = lifecycle.Value.Info.CurrentActor?.ActorId;
            if (actorId is null) continue;

            if (_actors.TryGetActor(actorId, out var actor) && actor is not null)
            {
                await NotifyActorDisconnectedCoreAsync(actor, cancellationToken)
                    .ConfigureAwait(false);
                continue;
            }

            await _runtime.NotifyActorDisconnectedByIdAsync(actorId, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask DispatchActorJoinDrainTurnAsync(
        CancellationToken cancellationToken)
    {
        if (await _dispatcher.DispatchActorJoinDrainAsync(cancellationToken)
                .ConfigureAwait(false)
            && !cancellationToken.IsCancellationRequested)
            QueueSerialized(static (activation, ct) =>
                activation.DispatchActorJoinDrainTurnAsync(ct));
    }

    private async ValueTask<bool> InvokeTimerAsync(
        ZLinkSpotTimerDescriptor descriptor,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        if (_timers.IsFrozen) return false;
        await HandlerInvoker.InvokeTimerAsync(descriptor, tick, cancellationToken).ConfigureAwait(false);
        return true;
    }

    internal async ValueTask<ZLinkSpotRelocationSeal> SealRelocationAsync(
        CancellationToken cancellationToken)
        => await SealRelocationAsync(
                allowActorClaims: false,
                cancellationToken)
            .ConfigureAwait(false);

    internal async ValueTask<ZLinkSpotRelocationSeal> SealRelocationAsync(
        bool allowActorClaims,
        CancellationToken cancellationToken)
    {
        IReadOnlyList<ZLinkRelocationLogicalTimer>? boundaryTimers = null;
        ZLinkSpotExecutionRelocationSeal? queueSeal = null;
        try
        {
            queueSeal = await _serial
                .SealRelocationAsync(
                    allowActorClaims,
                    () =>
                    {
                        boundaryTimers = _timers.FreezeRelocation();
                        if (allowActorClaims)
                            return 0;
                        // Reserve pending-tick sequences at the same queue
                        // boundary. A dispatch that finishes immediately after
                        // the freeze may remove its pending tick, but the
                        // frozen timer cannot create another pending tick and
                        // no later held ingress can overtake a retained tick.
                        return boundaryTimers.Count(static timer =>
                            ZLinkSpotTimerRelocationCodec.Decode(timer)
                                .Timer.PendingTick.HasValue);
                    },
                    cancellationToken)
                .ConfigureAwait(false);
            _ = boundaryTimers
                ?? throw new InvalidOperationException(
                    "SPOT relocation timer boundary snapshot was not created.");
            if (allowActorClaims)
                return new ZLinkSpotRelocationSeal(queueSeal, []);
            var logicalTimers = await _timers
                .SnapshotFrozenRelocationAfterDispatchesAsync(
                    cancellationToken)
                .ConfigureAwait(false);
            var pendingTimerCount = logicalTimers.Count(static timer =>
                ZLinkSpotTimerRelocationCodec.Decode(timer)
                    .Timer.PendingTick.HasValue);
            if (pendingTimerCount
                > queueSeal.QueueSeal.ReservedAcceptedSequences)
                throw new InvalidOperationException(
                    "SPOT relocation produced more pending timer ticks after its frozen boundary.");
            var nextPendingSequence =
                queueSeal.QueueSeal.FirstReservedSequence;
            logicalTimers = logicalTimers.Select(timer =>
            {
                var snapshot = ZLinkSpotTimerRelocationCodec.Decode(timer);
                return snapshot.Timer.PendingTick.HasValue
                    ? timer with
                    {
                        PendingAcceptedSequence = nextPendingSequence++
                    }
                    : timer;
            }).ToArray();
            return new ZLinkSpotRelocationSeal(queueSeal, logicalTimers);
        }
        catch
        {
            if (queueSeal is not null)
                _serial.TryAbortRelocation(queueSeal);
            _timers.Resume();
            throw;
        }
    }

    internal bool IsRelocationReady => _serial.IsRelocationReady;

    internal bool IsPerActorShellRelocationReady =>
        _serial.IsPerActorShellRelocationReady;

    internal ZLinkRelocationInterruptionOperation
        StartRelocationInterruption(bool instanceSpot) =>
        _runtime.RelocationInterruption.Start(
            instanceSpot
                ? ZLinkRelocationUnitKind.InstanceSpot
                : ZLinkRelocationUnitKind.UserSpot,
            instanceSpot
                ? null
                : ExecutionMode == ZLinkUserSpotExecutionMode.PerActor
                    ? "per_actor"
                    : "spot_wide");

    internal ulong SourceNodeLifecycleGeneration =>
        _runtime.GetSpotNodeRuntime(NodeRid).Node.MeshStatus()
            .LifecycleGeneration;

    internal ZLinkLocationOwnerToken SourceOwnerToken =>
        _runtime.LocationLifecycle?.OwnerToken
        ?? throw new ZLinkConfigurationException(
            "Location runtime is required for SPOT relocation.");

    internal ZLinkRemoteActorBoundSessionRoute
        CaptureActorBoundSessionRouteForRetire(string actorId)
    {
        var actorState = _runtime.GetOrCreateActorState(actorId);
        if (!actorState.TryGetBoundSession(out var session)
            || session.SessionNodeRid is not { } sessionNodeRid)
            return default;
        return new ZLinkRemoteActorBoundSessionRoute(
            NodeRid: sessionNodeRid,
            SessionRid: session.SessionRid,
            BindingToken: session.BindingToken,
            BindingGeneration: session.BindingGeneration,
            ObjectGeneration: session.ObjectGeneration,
            AuthorityOwnerGeneration:
                session.AuthorityOwnerGeneration,
            MeshName: session.MeshName,
            TargetNodeGeneration: session.TargetNodeGeneration,
            OwnerLeaseGeneration: session.OwnerLeaseGeneration,
            SessionOwnerNodeGeneration:
                session.SessionOwnerNodeGeneration,
            AcceptedHighWater: session.AcceptedHighWater);
    }

    internal async ValueTask<ZLinkRemoteActorBoundSessionRoute>
        SealActorBoundSessionRouteForRetireAsync(
            string actorId,
            string handoffId,
            CancellationToken cancellationToken)
    {
        var route = CaptureActorBoundSessionRouteForRetire(actorId);
        if (!route.IsBound)
            return route;
        var seal = new ZLinkSessionRouteSeal(
            actorId,
            route.BindingToken!,
            route.BindingGeneration,
            route.ObjectGeneration,
            route.AuthorityOwnerGeneration,
            route.MeshName!,
            route.TargetNodeGeneration,
            route.OwnerLeaseGeneration,
            route.SessionOwnerNodeGeneration,
            handoffId);
        ZLinkSessionRouteSealResult result;
        var routeNodeRid = route.NodeRid
                           ?? throw new InvalidOperationException(
                               "A bound Session route requires a target NodeRid.");
        if (routeNodeRid
            == _runtime.GetMeshNodeRuntime(route.MeshName!).Node.RoutingId)
        {
            result = await _runtime.SealSessionActorRouteAsync(
                    seal,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        else
        {
            var reply = await _runtime
                .RequestSessionRouteSealAsync(
                    route.MeshName!,
                    routeNodeRid,
                    new ZLinkSessionRouteSealRequest(
                        actorId,
                        route.BindingToken!,
                        route.BindingGeneration,
                        route.ObjectGeneration,
                        route.AuthorityOwnerGeneration,
                        route.MeshName!,
                        route.TargetNodeGeneration,
                        route.OwnerLeaseGeneration,
                        route.SessionOwnerNodeGeneration,
                        handoffId),
                    cancellationToken)
                .ConfigureAwait(false);
            result = new ZLinkSessionRouteSealResult(
                reply.Acknowledged,
                reply.AcceptedHighWater);
        }
        if (!result.Acknowledged)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' session ingress seal was fenced.");
        return route with { AcceptedHighWater = result.AcceptedHighWater };
    }

    internal async ValueTask AbortActorBoundSessionRouteSealForRetireAsync(
        string actorId,
        ZLinkRemoteActorBoundSessionRoute route,
        string handoffId,
        CancellationToken cancellationToken)
    {
        if (!route.IsBound)
            return;
        var seal = new ZLinkSessionRouteSeal(
            actorId,
            route.BindingToken!,
            route.BindingGeneration,
            route.ObjectGeneration,
            route.AuthorityOwnerGeneration,
            route.MeshName!,
            route.TargetNodeGeneration,
            route.OwnerLeaseGeneration,
            route.SessionOwnerNodeGeneration,
            handoffId);
        bool acknowledged;
        var routeNodeRid = route.NodeRid
                           ?? throw new InvalidOperationException(
                               "A bound Session route requires a target NodeRid.");
        if (routeNodeRid
            == _runtime.GetMeshNodeRuntime(route.MeshName!).Node.RoutingId)
        {
            acknowledged = _runtime.AbortSessionActorRouteSeal(seal);
        }
        else
        {
            var reply = await _runtime
                .RequestSessionRouteAbortAsync(
                    route.MeshName!,
                    routeNodeRid,
                    new ZLinkSessionRouteAbortRequest(
                        actorId,
                        route.BindingToken!,
                        route.BindingGeneration,
                        route.ObjectGeneration,
                        route.AuthorityOwnerGeneration,
                        route.MeshName!,
                        route.TargetNodeGeneration,
                        route.OwnerLeaseGeneration,
                        route.SessionOwnerNodeGeneration,
                        handoffId),
                    cancellationToken)
                .ConfigureAwait(false);
            acknowledged = reply.Acknowledged;
        }
        if (!acknowledged)
            throw new ZLinkRelocationDataLostException(
                $"Actor '{actorId}' source session route seal was not restored.");
    }

    internal void BeginMessageFollow(
        RoutingId targetNodeRid,
        ulong targetNodeGeneration,
        ulong sourceAuthorityOwnerGeneration,
        ulong targetAuthorityOwnerGeneration,
        ZLinkLocationOwnerToken targetOwner)
    {
        var duration = _runtime.Registration.Locations.Options
            .MessageFollowDuration;
        if (duration <= TimeSpan.Zero)
        {
            RejectPendingMessageFollowRoutes();
            return;
        }
        var messageFollow = new ZLinkSpotMessageFollow(
                targetNodeRid,
                ObjectGeneration,
                SourceNodeLifecycleGeneration,
                targetNodeGeneration,
                sourceAuthorityOwnerGeneration,
                targetAuthorityOwnerGeneration,
                SourceOwnerToken,
                targetOwner,
                DateTimeOffset.UtcNow + duration);
        Volatile.Write(ref _messageFollow, messageFollow);
        ZLinkFrameworkDebugLog.SpotDiscovery(
            $"message_follow_registered target_rid={targetNodeRid}");
        RelayPendingMessageFollowRoutes();
    }

    internal ValueTask WaitForMessageFollowDrainedAsync(
        CancellationToken cancellationToken)
    {
        var messageFollow = Volatile.Read(ref _messageFollow);
        return messageFollow is null
            ? ValueTask.CompletedTask
            : messageFollow.WaitForExpiryAndDrainAsync(cancellationToken);
    }

    private ZLinkSpotMessageFollowResult TryMessageFollow(
        ZLinkBackendRouteReceived received)
    {
        var messageFollow = Volatile.Read(ref _messageFollow);
        if (messageFollow is null)
            return ZLinkSpotMessageFollowResult.NotApplicable;
        var now = DateTimeOffset.UtcNow;
        var currentSourceOwner =
            _runtime.LocationLifecycle?.OwnerToken;
        if (!messageFollow.MatchesSourceRoute(
                received,
                ObjectGeneration,
                currentSourceOwner,
                now))
        {
            // A mismatched frame is stale by itself. It must not revoke the
            // bounded Message Follow route for later frames that still carry
            // the exact source authority generations.
            var removeRoute =
                messageFollow.ShouldRemoveAfterRejectedFrame(now);
            if (removeRoute)
                _ = Interlocked.CompareExchange(
                    ref _messageFollow,
                    null,
                    messageFollow);
            ZLinkFrameworkDebugLog.SpotDiscovery(
                removeRoute
                    ? "message_follow_expired"
                    : "message_follow_rejected");
            return ZLinkSpotMessageFollowResult.StaleRejected;
        }
        ReadOnlyMemory<byte> metadata;
        long bytes;
        try
        {
            metadata = ZLinkMeshMetadataCodec.Encode(received.Metadata);
            bytes = ZLinkServiceWireCodec.MeasureSpotMessageFollowEncodedBytes(
                received.CanReply,
                received.OperationId,
                SpotId,
                SpotId,
                ObjectGeneration,
                messageFollow.TargetNodeRid,
                messageFollow.TargetNodeGeneration,
                messageFollow.TargetAuthorityOwnerGeneration,
                checked((ulong)messageFollow.TargetOwner.LeaseGeneration),
                checked((byte)(received.MessageFollowHopCount + 1)),
                received.Parts,
                metadata);
        }
        catch
        {
            received.Dispose();
            throw;
        }
        if (!messageFollow.TryAcquire(bytes, out var lease))
            return ZLinkSpotMessageFollowResult.Full;
        lease = lease
                ?? throw new InvalidOperationException(
                    "Spot Message Follow admission did not return a lease.");
        if (!received.CanReply)
        {
            var retained = received.Parts.Select(Message.From).ToArray();
            var operationId = received.OperationId;
            var messageFollowHopCount =
                checked((byte)(received.MessageFollowHopCount + 1));
            received.Dispose();
            _ = CompleteOneWayMessageFollowAsync(
                messageFollow,
                operationId,
                messageFollowHopCount,
                received.SourceNodeRid,
                received.RequestSeq ?? 0,
                retained,
                metadata,
                lease);
            return ZLinkSpotMessageFollowResult.Followed;
        }

        if (NativeSpot is not IZLinkBackendSpotMessageFollower requestRelay)
        {
            lease.Dispose();
            return ZLinkSpotMessageFollowResult.StaleRejected;
        }
        var remainingTimeout = RemainingRequestTimeout(received, DateTimeOffset.UtcNow);
        if (remainingTimeout == TimeSpan.Zero)
        {
            lease.Dispose();
            received.Dispose();
            return ZLinkSpotMessageFollowResult.Followed;
        }
        var followed = SubmitSpotMessageFollowRequest(
            received,
            lease,
            callback => requestRelay.MessageFollowRequestToSpot(
                messageFollow.TargetNodeRid,
                SpotId,
                ObjectGeneration,
                received.OperationId,
                messageFollow.TargetNodeGeneration,
                messageFollow.TargetAuthorityOwnerGeneration,
                checked((ulong)messageFollow.TargetOwner.LeaseGeneration),
                checked((byte)(received.MessageFollowHopCount + 1)),
                received.DeadlineUnixMs,
                received.Parts,
                callback,
                SendFlags.DontWait,
                timeout: remainingTimeout,
                metadata: metadata))
            ? ZLinkSpotMessageFollowResult.Followed
            : ZLinkSpotMessageFollowResult.Full;
        if (followed == ZLinkSpotMessageFollowResult.Followed)
        {
            TrySendMessageFollowNotification(
                messageFollow,
                received.SourceNodeRid,
                received.RequestSeq ?? 0,
                received.OperationId,
                checked((byte)(received.MessageFollowHopCount + 1)));
            ZLinkFrameworkDebugLog.SpotDiscovery("message_follow_relay");
        }
        return followed;
    }

    private async Task CompleteOneWayMessageFollowAsync(
        ZLinkSpotMessageFollow messageFollow,
        MeshOperationId operationId,
        byte messageFollowHopCount,
        RoutingId? sourceNodeRid,
        ulong replyRouteId,
        IReadOnlyList<Message> retained,
        ReadOnlyMemory<byte> metadata,
        ZLinkSpotMessageFollow.AdmissionLease admission)
    {
        try
        {
            var result = await _outbound.SendMessageFollowToSpotAsync(
                    messageFollow.TargetNodeRid,
                    SpotId,
                    ObjectGeneration,
                    operationId,
                    messageFollow.TargetNodeGeneration,
                    messageFollow.TargetAuthorityOwnerGeneration,
                    checked((ulong)messageFollow.TargetOwner.LeaseGeneration),
                    messageFollowHopCount,
                    retained,
                    StopToken,
                    metadata)
                .ConfigureAwait(false);
            if (result.Status == ZLinkOneWaySubmitStatus.Submitted)
            {
                TrySendMessageFollowNotification(
                    messageFollow,
                    sourceNodeRid,
                    replyRouteId,
                    operationId,
                    messageFollowHopCount);
                ZLinkFrameworkDebugLog.SpotDiscovery("message_follow_relay");
                return;
            }

            _runtime.ErrorSink.ReportRuntimeTaskException(
                "spot-message-follow",
                new ZLinkRelocationDataLostException(
                    $"Message Follow for Spot '{SpotId}' ended with '{result.Status}'."));
        }
        catch (Exception exception)
        {
            _runtime.ErrorSink.ReportRuntimeTaskException(
                "spot-message-follow",
                exception);
        }
        finally
        {
            admission.Dispose();
        }
    }

    private void TrySendMessageFollowNotification(
        ZLinkSpotMessageFollow messageFollow,
        RoutingId? sourceNodeRid,
        ulong replyRouteId,
        MeshOperationId operationId,
        byte hopCount)
    {
        if (!messageFollow.TryClaimMessageFollowNotice())
            return;
        if (sourceNodeRid is not { } source
            || source.IsEmpty
            || operationId == default
            || hopCount is 0 or > ZLinkServiceWireCodec.MessageFollowMaximumHopCount)
        {
            messageFollow.ReleaseMessageFollowNoticeClaim();
            return;
        }

        try
        {
            var admission = messageFollow.AdmissionSnapshot();
            var record = new ZLinkServiceWireCodec.MessageFollowRecord(
                new ZLinkServiceWireCodec.MessageFollowRoute(
                    ZLinkServiceWireCodec.MessageFollowSpotKind,
                    SpotId,
                    ObjectGeneration,
                    NodeRid,
                    SourceNodeLifecycleGeneration,
                    messageFollow.SourceAuthorityOwnerGeneration,
                    checked((ulong)messageFollow.SourceOwner.LeaseGeneration)),
                new ZLinkServiceWireCodec.MessageFollowRoute(
                    ZLinkServiceWireCodec.MessageFollowSpotKind,
                    SpotId,
                    ObjectGeneration,
                    messageFollow.TargetNodeRid,
                    messageFollow.TargetNodeGeneration,
                    messageFollow.TargetAuthorityOwnerGeneration,
                    checked((ulong)messageFollow.TargetOwner.LeaseGeneration)),
                hopCount,
                checked((uint)admission.Records),
                checked((uint)admission.Bytes),
                operationId,
                replyRouteId);
            var node = _runtime.GetMeshNodeRuntime(MeshName).Node;
            if (node is not IZLinkBackendMessageFollowNotifications sender
                || !sender.TrySendMessageFollowNotification(source, record))
                messageFollow.ReleaseMessageFollowNoticeClaim();
        }
        catch (Exception exception)
            when (exception is InvalidOperationException
                or ZlinkException
                or ZLinkFrameworkException)
        {
            messageFollow.ReleaseMessageFollowNoticeClaim();
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"spot message follow notification failed: {exception.Message}");
        }
    }

    private bool TryHoldForMessageFollow(
        ZLinkBackendRouteReceived received,
        long encodedBytes)
    {
        lock (_messageFollowPendingGate)
        {
            if (Volatile.Read(ref _messageFollow) is null)
            {
                if (!_holdIngressForMessageFollow
                    || _messageFollowPending.Count
                    >= ZLinkSerialExecutionQueue.RelocationHoldMessageLimit
                    || encodedBytes
                       > ZLinkSerialExecutionQueue.RelocationHoldByteLimit
                                      - _messageFollowPendingBytes)
                    return false;
                _messageFollowPending.Enqueue(
                    new PendingMessageFollowRoute(received, encodedBytes));
                _messageFollowPendingBytes = checked(
                    _messageFollowPendingBytes + encodedBytes);
                return true;
            }
        }

        return HandleMessageFollow(received);
    }

    private bool HandleMessageFollow(ZLinkBackendRouteReceived received)
    {
        switch (TryMessageFollow(received))
        {
            case ZLinkSpotMessageFollowResult.Followed:
                return true;
            case ZLinkSpotMessageFollowResult.StaleRejected:
                ZLinkSpotActivationDispatcher
                    .RejectApplicationRouteForStaleMessageFollow(
                        received,
                        ChannelName);
                return true;
            case ZLinkSpotMessageFollowResult.Full:
                ZLinkSpotActivationDispatcher
                    .RejectApplicationRouteForRelocation(
                        received,
                        ChannelName);
                return true;
            case ZLinkSpotMessageFollowResult.NotApplicable:
                return false;
            default:
                throw new InvalidOperationException(
                    "Unknown Spot Message Follow result.");
        }
    }

    private void RelayPendingMessageFollowRoutes()
    {
        PendingMessageFollowRoute[] pending;
        lock (_messageFollowPendingGate)
        {
            _holdIngressForMessageFollow = false;
            pending = _messageFollowPending.ToArray();
            _messageFollowPending.Clear();
            _messageFollowPendingBytes = 0;
        }

        foreach (var route in pending)
            if (!HandleMessageFollow(route.Received))
                ZLinkSpotActivationDispatcher
                    .RejectApplicationRouteForRelocation(
                        route.Received,
                        ChannelName);
    }

    private void ResumePendingMessageFollowRoutes()
    {
        PendingMessageFollowRoute[] pending;
        lock (_messageFollowPendingGate)
        {
            _holdIngressForMessageFollow = false;
            pending = _messageFollowPending.ToArray();
            _messageFollowPending.Clear();
            _messageFollowPendingBytes = 0;
        }

        foreach (var route in pending)
            AdmitNativeRoute(route.Received);
    }

    private void RejectPendingMessageFollowRoutes()
    {
        PendingMessageFollowRoute[] pending;
        lock (_messageFollowPendingGate)
        {
            _holdIngressForMessageFollow = false;
            pending = _messageFollowPending.ToArray();
            _messageFollowPending.Clear();
            _messageFollowPendingBytes = 0;
        }

        foreach (var route in pending)
            ZLinkSpotActivationDispatcher.RejectApplicationRouteForRelocation(
                route.Received,
                ChannelName);
    }

    private void DisposePendingMessageFollowRoutes()
    {
        PendingMessageFollowRoute[] pending;
        lock (_messageFollowPendingGate)
        {
            _holdIngressForMessageFollow = false;
            pending = _messageFollowPending.ToArray();
            _messageFollowPending.Clear();
            _messageFollowPendingBytes = 0;
        }
        foreach (var route in pending)
            route.Received.Dispose();
    }

    internal static bool SubmitSpotMessageFollowRequest(
        ZLinkBackendRouteReceived received,
        ZLinkSpotMessageFollow.AdmissionLease admission,
        Func<RequestCallback, bool> submit)
    {
        ArgumentNullException.ThrowIfNull(received);
        ArgumentNullException.ThrowIfNull(admission);
        ArgumentNullException.ThrowIfNull(submit);
        try
        {
            var accepted = submit((result, parts) =>
            {
                try
                {
                    if (result == RequestResult.Ok
                        && RemainingRequestTimeout(
                            received,
                            DateTimeOffset.UtcNow) != TimeSpan.Zero)
                        _ = received.Reply(parts, SendFlags.None);
                }
                finally
                {
                    ZLinkMessageParts.DisposeAll(parts);
                    received.Dispose();
                    admission.Dispose();
                }
            });
            if (accepted) return true;
            admission.Dispose();
            return false;
        }
        catch
        {
            received.Dispose();
            admission.Dispose();
            throw;
        }
    }

    internal static TimeSpan? RemainingRequestTimeout(
        ZLinkBackendRouteReceived received,
        DateTimeOffset now)
    {
        ArgumentNullException.ThrowIfNull(received);
        if (received.DeadlineUnixMs == 0)
            return null;
        var remainingMilliseconds = checked((long)received.DeadlineUnixMs)
                                    - now.ToUnixTimeMilliseconds();
        return remainingMilliseconds <= 0
            ? TimeSpan.Zero
            : TimeSpan.FromMilliseconds(remainingMilliseconds);
    }

    internal bool TrySealRelocation(out ZLinkSpotRelocationSeal seal)
    {
        var logicalTimers = _timers.FreezeRelocation();
        if (_serial.TrySealRelocation(out var queueSeal))
        {
            seal = new ZLinkSpotRelocationSeal(queueSeal, logicalTimers);
            return true;
        }
        _timers.Resume();
        seal = null!;
        return false;
    }

    internal bool TrySealPerActorShellRelocation(
        out ZLinkSpotRelocationSeal seal)
    {
        if (ExecutionMode != ZLinkUserSpotExecutionMode.PerActor)
        {
            seal = null!;
            return false;
        }
        var logicalTimers = _timers.FreezeRelocation();
        if (_serial.TrySealPerActorShellRelocation(out var queueSeal))
        {
            seal = new ZLinkSpotRelocationSeal(queueSeal, logicalTimers);
            return true;
        }
        _timers.Resume();
        seal = null!;
        return false;
    }

    internal bool TrySealPerActorShellRelocation(
        Func<
            IReadOnlyList<ZLinkAcceptedWorkRecord>,
            IReadOnlyList<ZLinkRelocationLogicalTimer>,
            bool> admit,
        out ZLinkSpotRelocationSeal seal)
    {
        ArgumentNullException.ThrowIfNull(admit);
        if (ExecutionMode != ZLinkUserSpotExecutionMode.PerActor)
        {
            seal = null!;
            return false;
        }
        var logicalTimers = _timers.FreezeRelocation();
        if (_serial.TrySealPerActorShellRelocation(
                reservedAcceptedSequences: 0,
                captured => admit(captured, logicalTimers),
                out var queueSeal,
                out _))
        {
            seal = new ZLinkSpotRelocationSeal(queueSeal, logicalTimers);
            return true;
        }
        _timers.Resume();
        seal = null!;
        return false;
    }

    internal bool TrySealRelocation(
        Func<
            IReadOnlyList<ZLinkAcceptedWorkRecord>,
            IReadOnlyList<ZLinkRelocationLogicalTimer>,
            bool> admit,
        out ZLinkSpotRelocationSeal seal)
    {
        ArgumentNullException.ThrowIfNull(admit);
        var logicalTimers = _timers.FreezeRelocation();
        var pendingTimerCount = logicalTimers.Count(static timer =>
            ZLinkSpotTimerRelocationCodec.Decode(timer).Timer.PendingTick.HasValue);
        if (_serial.TrySealRelocation(
                pendingTimerCount,
                captured => admit(captured, logicalTimers),
                out var queueSeal,
                out var firstPendingSequence))
        {
            var nextPendingSequence = firstPendingSequence;
            logicalTimers = logicalTimers.Select(timer =>
            {
                var snapshot = ZLinkSpotTimerRelocationCodec.Decode(timer);
                return snapshot.Timer.PendingTick.HasValue
                    ? timer with
                    {
                        PendingAcceptedSequence = nextPendingSequence++
                    }
                    : timer;
            }).ToArray();
            seal = new ZLinkSpotRelocationSeal(queueSeal, logicalTimers);
            return true;
        }
        _timers.Resume();
        seal = null!;
        return false;
    }

    internal bool AbortRelocation(ZLinkSpotRelocationSeal seal)
    {
        ArgumentNullException.ThrowIfNull(seal);
        if (!_serial.TryAbortRelocation(seal.QueueSeal))
            return false;
        ResumePendingMessageFollowRoutes();
        _timers.Resume();
        return true;
    }

    internal bool OpenRelocationTargetAdmission(
        ZLinkSpotRelocationSeal seal,
        Action reserveBeforeApplicationAdmission)
    {
        ArgumentNullException.ThrowIfNull(seal);
        ArgumentNullException.ThrowIfNull(reserveBeforeApplicationAdmission);
        if (!_serial.TryOpenRelocationAfterMessageFollow(
                seal.QueueSeal,
                reserveBeforeApplicationAdmission))
            return false;
        ResumePendingMessageFollowRoutes();
        _timers.Resume();
        return true;
    }

    internal bool CommitRelocation(
        ZLinkSpotRelocationSeal seal,
        out IReadOnlyList<ZLinkAcceptedWorkRecord> held,
        bool preserveActorExecution = false)
    {
        ArgumentNullException.ThrowIfNull(seal);
        return _serial.TryCommitRelocation(
            seal.QueueSeal,
            out held,
            preserveActorExecution);
    }

    internal bool FreezeRelocationIngress(
        ZLinkSpotRelocationSeal seal,
        out IReadOnlyList<ZLinkAcceptedWorkRecord> held)
    {
        ArgumentNullException.ThrowIfNull(seal);
        lock (_messageFollowPendingGate)
            _holdIngressForMessageFollow = true;
        if (_serial.TryFreezeRelocationIngress(
                seal.QueueSeal,
                out held))
            return true;
        lock (_messageFollowPendingGate)
            _holdIngressForMessageFollow = false;
        return false;
    }

    internal void RestoreLogicalTimers(
        IReadOnlyList<ZLinkRelocationLogicalTimer> logicalTimers)
    {
        _timers.RestoreRelocation(
            logicalTimers,
            Spot.GetType(),
            StopToken,
            DispatchTimerAsync,
            PublishTimerFailureAsync);
    }

    internal async ValueTask ReplayAcceptedJobsAsync(
        IReadOnlyList<ZLinkRelocationQueuedJob> jobs,
        string sourceMeshName,
        ZLinkSpotRelocationSeal admissionSeal,
        int completedCount,
        Func<
            ZLinkRelocationQueuedJob,
            ZLinkSpotAcceptedJournalRecord,
            byte[][]?,
            CancellationToken,
            ValueTask> replayCompleted,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(admissionSeal);
        ArgumentNullException.ThrowIfNull(replayCompleted);
        var ordered = jobs.OrderBy(static job => job.AcceptedSequence)
            .ToArray();
        if (completedCount < 0 || completedCount > ordered.Length)
            throw new ArgumentOutOfRangeException(nameof(completedCount));
        foreach (var job in ordered.Skip(completedCount))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var journal = DecodeRelocationReplayRecord(job, sourceMeshName);
            var parts = journal.Parts
                .Select(static part => Message.From(part.Span))
                .ToArray();
            byte[][]? capturedReply = null;
            Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? reply = null;
            if (journal.ReplyRouteId != 0
                && journal.SourceNodeRid is { } sourceNodeRid)
            {
                reply = (replyParts, _) =>
                {
                    capturedReply = replyParts
                        .Select(static part => part.ToArray())
                        .ToArray();
                    return SubmitResult.Ok;
                };
            }
            var received = new ZLinkBackendRouteReceived(
                parts,
                journal.SourceNodeRid,
                journal.SpotId,
                journal.RequestSequence,
                reply,
                metadata: journal.Metadata,
                operationId: journal.OperationId,
                targetNodeGeneration: journal.TargetNodeGeneration,
                authorityOwnerGeneration:
                    journal.AuthorityOwnerGeneration,
                ownerLeaseGeneration: journal.OwnerLeaseGeneration,
                messageFollowHopCount: journal.MessageFollowHopCount,
                sourceNodeGeneration: journal.SourceNodeGeneration);
            try
            {
                await _serial.ExecuteSealedRelocationAsync(
                        admissionSeal.QueueSeal,
                        (activation, ct) =>
                            activation._dispatcher.DispatchRouteAsync(
                                received,
                                ct),
                        cancellationToken)
                    .ConfigureAwait(false);
                await replayCompleted(
                        job,
                        journal,
                        capturedReply,
                        cancellationToken)
                    .ConfigureAwait(false);
            }
            finally
            {
                received.Dispose();
            }
        }
    }

    private static ZLinkSpotAcceptedJournalRecord DecodeRelocationReplayRecord(
        ZLinkRelocationQueuedJob job,
        string channelName)
    {
        if (job.CanonicalRequest is not { } request)
            return ZLinkSpotAcceptedJournal.Decode(job.Payload.Span);
        var kind = request.ReplyRouteId == 0
            ? ZLinkMessageKind.Command
            : ZLinkMessageKind.Request;
        using var header = ZLinkEnvelopeCodec.EncodeHeader(
            new ZLinkEnvelopeHeader(
                kind,
                channelName,
                request.ApplicationPayload.PacketName,
                request.ApplicationPayload.ContentType,
                request.ReplyRouteId == 0
                    ? null
                    : request.ReplyRouteId.ToString(
                        System.Globalization.CultureInfo.InvariantCulture),
                null,
                null,
                null,
                null,
                request.SourceSpotId));
        return new ZLinkSpotAcceptedJournalRecord(
            RoutingId.FromHex(request.Source.NodeRid),
            request.Source.NodeGeneration,
            new ZLinkServiceWireCodec.RequestSourceFence(
                request.Source.OwnerId,
                request.Source.OwnerLeaseGeneration,
                RoutingId.FromHex(request.Source.NodeRid),
                request.Source.NodeGeneration),
            request.SourceSpotId,
            null,
            request.ReplyRouteId,
            new MeshOperationId(request.OperationHigh, request.OperationLow),
            request.TargetNodeGeneration,
            request.TargetAuthorityOwnerGeneration,
            request.TargetOwnerLeaseGeneration,
            0,
            request.Metadata,
            [header.ToArray(), request.ApplicationPayload.Payload.ToArray()]);
    }

    private async ValueTask<bool> DispatchTimerAsync(
        ZLinkSpotTimerDescriptor descriptor,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        if (_timers.IsFrozen)
            return false;
        var state = new TimerDispatchState(descriptor, tick);
        if (!_runtime.TryEnterInboundOperation(countAsRequest: false, out var lease))
        {
            // Host admission or the owner fence can close before this Spot
            // reaches its relocation turn. Keep the exact tick pending for
            // the Spot snapshot instead of reporting an application handler
            // failure.
            _timers.FreezeForApplicationAdmissionSeal();
            return false;
        }
        using (lease)
            await _serial.ExecuteTimerAsync(
                descriptor.Name,
                async static (activation, state, innerCt) =>
                {
                    state.Delivered = await activation
                        .InvokeTimerAsync(
                            state.Descriptor,
                            state.Tick,
                            innerCt)
                        .ConfigureAwait(false);
                },
                state,
                cancellationToken)
                .ConfigureAwait(false);
        return state.Delivered;
    }

    internal async ValueTask<ZLinkSpotRelocationApplicationState>
        CaptureRelocationApplicationStateAsync(CancellationToken cancellationToken)
    {
        ZLinkSpotRelocationApplicationState? captured = null;
        await _serial.ExecuteLifecycleAsync(
                async (activation, ct) =>
                {
                    var spotRegistration = activation.ResolveSpotRelocationRegistration();
                    var spotState = await activation.CaptureInstanceAsync(
                            spotRegistration,
                            activation.Spot,
                            ct)
                        .ConfigureAwait(false);
                    var actorStates = await activation.CaptureActorStatesAsync(
                            activation._actors.Snapshot(),
                            includedActorIds: null,
                            ct)
                        .ConfigureAwait(false);
                    captured = new ZLinkSpotRelocationApplicationState(
                        spotState,
                        actorStates);
                },
                cancellationToken)
            .ConfigureAwait(false);
        return captured
               ?? throw new InvalidOperationException(
                   "SPOT relocation capture did not complete.");
    }

    internal async ValueTask<ZLinkSpotRelocationApplicationState>
        CaptureSealedRelocationApplicationStateAsync(
            ZLinkSpotRelocationSeal seal,
            IReadOnlySet<string>? includedActorIds,
            CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(seal);
        ZLinkSpotRelocationApplicationState? captured = null;
        await _serial.ExecuteSealedRelocationAsync(
                seal.QueueSeal,
                async (activation, ct) =>
                {
                    var spotRegistration =
                        activation.ResolveSpotRelocationRegistration();
                    var spotState = await activation.CaptureInstanceAsync(
                            spotRegistration,
                            activation.Spot,
                            ct)
                        .ConfigureAwait(false);
                    var actorStates = await activation.CaptureActorStatesAsync(
                            activation._actors.Snapshot(),
                            includedActorIds,
                            ct)
                        .ConfigureAwait(false);
                    captured = new ZLinkSpotRelocationApplicationState(
                        spotState,
                        actorStates);
                },
                cancellationToken)
            .ConfigureAwait(false);
        return captured
               ?? throw new InvalidOperationException(
                   "SPOT relocation capture did not complete.");
    }

    internal async ValueTask RestoreRelocationApplicationStateAsync(
        ZLinkSpotRelocationApplicationState state,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(state);
        await _serial.ExecuteLifecycleAsync(
                async (activation, ct) =>
                {
                    await activation.RestoreInstanceAsync(
                            activation.ResolveSpotRelocationRegistration(),
                            activation.Spot,
                            state.SpotState,
                            ct)
                        .ConfigureAwait(false);
                    var actors = activation._actors.Snapshot();
                    for (var first = 0;
                         first < actors.Count;
                         first += MaxConcurrentRelocationAdapterCallbacks)
                    {
                        var count = Math.Min(
                            MaxConcurrentRelocationAdapterCallbacks,
                            actors.Count - first);
                        await Task.WhenAll(
                                actors.Skip(first).Take(count).Select(
                                    async actor =>
                                    {
                                        if (!state.ActorStates.TryGetValue(
                                                actor.Context.ActorId,
                                                out var actorState))
                                            throw new InvalidDataException(
                                                $"Relocation state for Actor '{actor.Context.ActorId}' is missing.");
                                        await activation.RestoreInstanceAsync(
                                                activation.ResolveActorRelocationRegistration(actor),
                                                actor,
                                                actorState,
                                                ct)
                                            .ConfigureAwait(false);
                                    }))
                            .ConfigureAwait(false);
                    }
                },
                cancellationToken)
            .ConfigureAwait(false);
    }

    private async ValueTask<IReadOnlyDictionary<string, ReadOnlyMemory<byte>>>
        CaptureActorStatesAsync(
            IReadOnlyList<IZLinkActor> actors,
            IReadOnlySet<string>? includedActorIds,
            CancellationToken cancellationToken)
    {
        var selected = actors
            .Where(actor => includedActorIds is null
                            || includedActorIds.Contains(actor.Context.ActorId))
            .ToArray();
        var captured =
            new Dictionary<string, ReadOnlyMemory<byte>>(StringComparer.Ordinal);
        for (var first = 0;
             first < selected.Length;
             first += MaxConcurrentRelocationAdapterCallbacks)
        {
            var count = Math.Min(
                MaxConcurrentRelocationAdapterCallbacks,
                selected.Length - first);
            var batch = await Task.WhenAll(
                    selected.Skip(first).Take(count).Select(
                        async actor => new KeyValuePair<string, ReadOnlyMemory<byte>>(
                            actor.Context.ActorId,
                            await CaptureInstanceAsync(
                                    ResolveActorRelocationRegistration(actor),
                                    actor,
                                    cancellationToken)
                                .ConfigureAwait(false))))
                .ConfigureAwait(false);
            foreach (var state in batch)
                captured.Add(state.Key, state.Value);
        }
        return captured;
    }

    internal ValueTask RestoreSpotRelocationStateAsync(
        ReadOnlyMemory<byte> state,
        CancellationToken cancellationToken) =>
        _serial.ExecuteLifecycleAsync(
            (activation, ct) => activation.RestoreInstanceAsync(
                activation.ResolveSpotRelocationRegistration(),
                activation.Spot,
                state,
                ct),
            cancellationToken);

    private ZLinkObjectRelocationRegistration ResolveSpotRelocationRegistration()
    {
        var node = _runtime.Registration.SpotNodes[SpotNodeName];
        var matches = node.SpotRelocations.Values
            .Concat(node.InstanceSpotRelocations.Values)
            .Where(registration => registration.InstanceType == Spot.GetType())
            .Distinct()
            .ToArray();
        return matches.Length switch
        {
            1 => matches[0],
            0 => throw new ZLinkConfigurationException(
                $"Relocation policy for SPOT '{Spot.GetType()}' is not registered."),
            _ => throw new ZLinkConfigurationException(
                $"Relocation policy for SPOT '{Spot.GetType()}' is ambiguous.")
        };
    }

    internal ZLinkObjectRelocationRegistration
        ResolveSpotRelocationRegistrationForRetire() =>
        ResolveSpotRelocationRegistration();

    internal string ResolveStableTypeForRetire()
    {
        var node = _runtime.Registration.SpotNodes[SpotNodeName];
        var matches = node.SpotRelocations
            .Concat(node.InstanceSpotRelocations)
            .Where(entry => entry.Value.InstanceType == Spot.GetType())
            .Select(static entry => entry.Key)
            .Distinct(StringComparer.Ordinal)
            .ToArray();
        return matches.Length == 1
            ? matches[0]
            : throw new ZLinkConfigurationException(
                $"Relocation stable type for SPOT '{Spot.GetType()}' is not unique.");
    }

    internal IReadOnlyList<string> SnapshotActorIds() =>
        _actors.Snapshot()
            .Select(static actor => actor.Context.ActorId)
            .OrderBy(static actorId => actorId, StringComparer.Ordinal)
            .ToArray();

    internal IReadOnlyList<ZLinkObjectCapability> ResolveRetireCapabilities(
        bool instanceSpot,
        bool includeActors = true)
    {
        var capabilities = new List<ZLinkObjectCapability>();
        var spot = ResolveSpotRelocationRegistration();
        capabilities.Add(CreateRetireCapability(
            instanceSpot
                ? ZLinkPlacementObjectKind.InstanceSpot
                : ZLinkPlacementObjectKind.UserSpot,
            ResolveStableTypeForRetire(),
            spot));
        if (!includeActors)
            return capabilities;
        foreach (var actor in _actors.Snapshot())
        {
            var actorType = _runtime.GetOrCreateActorState(
                    actor.Context.ActorId)
                .ActorType
                            ?? throw new ZLinkConfigurationException(
                                $"Relocation stable type for Actor '{actor.Context.ActorId}' is not registered.");
            capabilities.Add(CreateRetireCapability(
                ZLinkPlacementObjectKind.Actor,
                actorType,
                ResolveActorRelocationRegistration(actor)));
        }
        return capabilities
            .DistinctBy(static capability => (
                capability.ObjectKind,
                capability.StableType))
            .ToArray();
    }

    private static ZLinkObjectCapability CreateRetireCapability(
        ZLinkPlacementObjectKind kind,
        string stableType,
        ZLinkObjectRelocationRegistration registration) =>
        new(
            kind,
            stableType,
            registration.PolicyKind switch
            {
                0 => ZLinkObjectMaintenancePolicyKind.Disabled,
                1 => ZLinkObjectMaintenancePolicyKind.Recreate,
                2 => ZLinkObjectMaintenancePolicyKind.Snapshot,
                _ => throw new ZLinkConfigurationException(
                    $"Unknown relocation policy kind '{registration.PolicyKind}'.")
            },
            registration.AdapterType is not null,
            0);

    internal ZLinkObjectRelocationRegistration
        ResolveActorRelocationRegistrationForRetire(string actorId)
    {
        if (!_actors.TryGetActor(actorId, out var actor) || actor is null)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NotFound,
                $"Actor '{actorId}' left SPOT '{SpotId}' before relocation sealed.");
        return ResolveActorRelocationRegistration(actor);
    }

    private ZLinkObjectRelocationRegistration ResolveActorRelocationRegistration(
        IZLinkActor actor)
    {
        var node = _runtime.Registration.SpotNodes[SpotNodeName];
        var actorType = _runtime.GetOrCreateActorState(actor.Context.ActorId).ActorType;
        if (actorType is not null
            && node.ActorRelocations.TryGetValue(actorType, out var registered))
            return registered;
        var matches = node.ActorRelocations.Values
            .Where(registration => registration.InstanceType == actor.GetType())
            .Distinct()
            .ToArray();
        return matches.Length switch
        {
            1 => matches[0],
            0 => throw new ZLinkConfigurationException(
                $"Relocation policy for Actor '{actor.GetType()}' is not registered."),
            _ => throw new ZLinkConfigurationException(
                $"Relocation policy for Actor '{actor.GetType()}' is ambiguous.")
        };
    }

    private async ValueTask<byte[]> CaptureInstanceAsync(
        ZLinkObjectRelocationRegistration registration,
        object instance,
        CancellationToken cancellationToken)
    {
        return registration.PolicyKind switch
        {
            0 => throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Relocation is disabled for '{registration.InstanceType}'."),
            1 => [],
            2 when registration.AdapterInvoker is { } invoker =>
                await invoker.CaptureAsync(
                        _scope.ServiceProvider,
                        instance,
                        cancellationToken)
                    .ConfigureAwait(false),
            _ => throw new ZLinkConfigurationException(
                $"Relocation adapter for '{registration.InstanceType}' is not registered.")
        };
    }

    private async ValueTask RestoreInstanceAsync(
        ZLinkObjectRelocationRegistration registration,
        object instance,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        switch (registration.PolicyKind)
        {
            case 1:
                if (!payload.IsEmpty)
                    throw new InvalidDataException(
                        $"Recreate relocation state for '{registration.InstanceType}' must be empty.");
                return;
            case 2 when registration.AdapterInvoker is { } invoker:
                await invoker.RestoreAsync(
                        _scope.ServiceProvider,
                        instance,
                        payload,
                        cancellationToken)
                    .ConfigureAwait(false);
                return;
            case 0:
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    $"Relocation is disabled for '{registration.InstanceType}'.");
            default:
                throw new ZLinkConfigurationException(
                    $"Relocation adapter for '{registration.InstanceType}' is not registered.");
        }
    }

    private ValueTask PublishTimerFailureAsync(
        ZLinkSpotTimerDescriptor descriptor,
        ZLinkTimerTick tick,
        Exception exception,
        bool stopped,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        _runtime.ReportTimerFailure(
            SpotNodeName,
            SpotId,
            false,
            descriptor.Name,
            descriptor.HandlerType,
            tick,
            exception,
            stopped);
        return ValueTask.CompletedTask;
    }

    private static T RegisterWithoutSynchronizationContext<T>(Func<T> action)
    {
        var previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(null);
        try
        {
            return action();
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }

    private sealed class SpotCreateCallState(ZLinkMessage request)
    {
        public ZLinkMessage Request { get; } = request;

        public ZLinkSpotCreateResponse Response { get; set; }
    }

    private sealed class TimerDispatchState(
        ZLinkSpotTimerDescriptor descriptor,
        ZLinkTimerTick tick)
    {
        public ZLinkSpotTimerDescriptor Descriptor { get; } = descriptor;

        public ZLinkTimerTick Tick { get; } = tick;

        public bool Delivered { get; set; }
    }

    private sealed record PendingMessageFollowRoute(
        ZLinkBackendRouteReceived Received,
        long EncodedBytes);
}
