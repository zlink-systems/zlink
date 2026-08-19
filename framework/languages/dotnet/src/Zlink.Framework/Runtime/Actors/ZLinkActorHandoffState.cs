namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorHandoffState(
    string actorId,
    TimeProvider timeProvider,
    Action<string>? diagnostic = null,
    ZLinkBoundedIngressAdmission? sourceIngressAdmission = null,
    ZLinkBoundedIngressAdmission? sourceHoldAdmission = null)
{
    private readonly object _gate = new();
    private readonly List<ZLinkActorHandoffFrame> _frames = [];
    private readonly List<ZLinkActorHandoffFrame> _sourceHoldFrames = [];
    private readonly Dictionary<long, Task>
        _canonicalMaintenanceReplayReservations = [];
    private readonly ZLinkBoundedIngressAdmission _sourceIngressAdmission =
        sourceIngressAdmission ?? new ZLinkBoundedIngressAdmission();
    private readonly ZLinkBoundedIngressAdmission _sourceHoldAdmission =
        sourceHoldAdmission ?? new ZLinkBoundedIngressAdmission();
    private CancellationTokenSource? _messageFollowExpiry;
    private ZLinkActorMessageFollowRoute? _messageFollowRoute;
    private ZLinkBackendActorRef? _staleSourceActor;
    private string? _handoffId;
    private ZLinkRemoteActorJoinRequest? _joinRequest;
    private ZLinkActorSourceHandoffPhase _sourcePhase;
    private ZLinkActorTargetHandoffPhase _targetPhase;
    private TaskCompletionSource? _targetCompletion;
    private TaskCompletionSource? _targetReplayCompletion;
    private long _arrivalIndex;
    private int _importedFrameCount;
    private int _sourceCommittedFrameCount = -1;
    private int _sourceCommittedHoldCount;
    private bool _sourceTrailingImported;
    private bool _sourceCaptureSealed;
    private bool _deferredJoinCapture;
    private bool _deferredJoinAwaitingTarget;
    private bool _abortRestoreAdmissionsReleased;
    private TaskCompletionSource<ZLinkRemoteActorJoinReply>? _preparation;
    private TaskCompletionSource? _sourceCompletion;
    private Task? _canonicalMaintenanceDrain;

    private IDisposable? _pendingShutdownToken;

    /// <summary>
    /// The SafeToShutdown obligation token for the relocation unit currently
    /// sealed on this Actor (spec 30 §11). The owning source runtime sets
    /// this atomically with <see cref="SealCapture(IDisposable)"/>; this class releases it
    /// exactly once, in <see cref="ClearMessageFollowRouteLocked"/>, which
    /// every source exit path — commit-then-S4, abort and reset — already
    /// funnels through. A fresh <see cref="BeginCapture"/> can start before
    /// a previous Message Follow route's window fully elapses (its
    /// background removal then no-ops on the stale route it holds); the
    /// setter disposes whatever token it replaces so that orphaned case
    /// still releases its obligation instead of leaking it forever.
    /// </summary>
    internal IDisposable? PendingShutdownToken
    {
        get => _pendingShutdownToken;
        set
        {
            var previous = Interlocked.Exchange(ref _pendingShutdownToken, value);
            if (!ReferenceEquals(previous, value))
                previous?.Dispose();
        }
    }

    public void BeginCapture()
    {
        lock (_gate)
        {
            if (_sourcePhase is ZLinkActorSourceHandoffPhase.Capturing
                or ZLinkActorSourceHandoffPhase.CutoverPending
                or ZLinkActorSourceHandoffPhase.AbortRestoring
                or ZLinkActorSourceHandoffPhase.MessageFollowCommitted
                || _targetPhase is ZLinkActorTargetHandoffPhase.Importing
                    or ZLinkActorTargetHandoffPhase.Replaying
                    or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                    or ZLinkActorTargetHandoffPhase.Failed
                    or ZLinkActorTargetHandoffPhase.Quarantined)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' already has an active handoff transaction.");

            if (_deferredJoinCapture)
            {
                _deferredJoinCapture = false;
                var preserveCompletedTarget =
                    _targetPhase == ZLinkActorTargetHandoffPhase.Completed;
                if (!preserveCompletedTarget)
                {
                    _handoffId = null;
                    _joinRequest = null;
                    _preparation = null;
                    _targetCompletion = null;
                    _targetReplayCompletion = null;
                    _canonicalMaintenanceDrain = null;
                    _canonicalMaintenanceReplayReservations.Clear();
                    _targetPhase = ZLinkActorTargetHandoffPhase.Idle;
                }
                _sourceTrailingImported = false;
                _sourceCaptureSealed = false;
                _sourceCommittedFrameCount = -1;
                _sourceCommittedHoldCount = 0;
                _abortRestoreAdmissionsReleased = false;
                _sourcePhase = ZLinkActorSourceHandoffPhase.Capturing;
                _sourceCompletion = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                _sourceHoldAdmission.ReleaseAll();
                _sourceHoldFrames.Clear();
                return;
            }

            _handoffId = null;
            _joinRequest = null;
            _preparation = null;
            _targetReplayCompletion = null;
            _canonicalMaintenanceDrain = null;
            _canonicalMaintenanceReplayReservations.Clear();
            _targetPhase = ZLinkActorTargetHandoffPhase.Idle;
            _sourceTrailingImported = false;
            _sourceCaptureSealed = false;
            _sourceCommittedFrameCount = -1;
            _sourceCommittedHoldCount = 0;
            _abortRestoreAdmissionsReleased = false;
            _sourcePhase = ZLinkActorSourceHandoffPhase.Capturing;
            _sourceCompletion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _arrivalIndex = 0;
        }
    }

    public Task? BeginDeferredJoinCapture()
    {
        lock (_gate)
        {
            if (_deferredJoinCapture
                || _deferredJoinAwaitingTarget
                || _sourcePhase != ZLinkActorSourceHandoffPhase.Idle
                || _targetPhase is ZLinkActorTargetHandoffPhase.Failed
                    or ZLinkActorTargetHandoffPhase.Quarantined)
            {
                diagnostic?.Invoke(
                    $"deferred_join_capture_refused actor={actorId} "
                    + $"source_phase={_sourcePhase} target_phase={_targetPhase} "
                    + $"handoff={_handoffId ?? "none"} "
                    + $"deferred={_deferredJoinCapture}");
                throw new InvalidOperationException(
                    $"Actor '{actorId}' already has an active handoff transaction.");
            }

            if (IsTargetHandoffActiveLocked())
            {
                _deferredJoinAwaitingTarget = true;
                diagnostic?.Invoke(
                    $"deferred_join_waiting_for_target actor={actorId} "
                    + $"target_phase={_targetPhase} handoff={_handoffId ?? "none"}");
                return _targetCompletion?.Task
                       ?? throw new InvalidOperationException(
                           $"Actor '{actorId}' target handoff has no completion boundary.");
            }

            BeginDeferredJoinCaptureLocked();
            return null;
        }
    }

    private void BeginDeferredJoinCaptureLocked()
    {
        _sourceIngressAdmission.ReleaseAll();
        _sourceHoldAdmission.ReleaseAll();
        _frames.Clear();
        _sourceHoldFrames.Clear();
        _arrivalIndex = 0;
        _deferredJoinCapture = true;
    }

    private bool IsTargetHandoffActiveLocked() =>
        _targetPhase is ZLinkActorTargetHandoffPhase.Importing
            or ZLinkActorTargetHandoffPhase.AuthorityCommitted
            or ZLinkActorTargetHandoffPhase.NotifyingJoined
            or ZLinkActorTargetHandoffPhase.Prepared
            or ZLinkActorTargetHandoffPhase.Replaying
            or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
        || _targetPhase == ZLinkActorTargetHandoffPhase.Completed;

    public IReadOnlyList<ZLinkActorHandoffFrame> EndDeferredJoinCapture()
    {
        lock (_gate)
        {
            if (_deferredJoinAwaitingTarget)
            {
                _deferredJoinAwaitingTarget = false;
                return [];
            }
            if (!_deferredJoinCapture
                || _sourcePhase != ZLinkActorSourceHandoffPhase.Idle)
                return [];

            var frames = _frames
                .OrderBy(static frame => frame.ArrivalIndex)
                .ToArray();
            _deferredJoinCapture = false;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _arrivalIndex = 0;
            return frames;
        }
    }

    public bool IsSourceMigrationInProgress
    {
        get
        {
            lock (_gate)
                return _sourcePhase is ZLinkActorSourceHandoffPhase.Capturing
                    or ZLinkActorSourceHandoffPhase.CutoverPending
                    or ZLinkActorSourceHandoffPhase.AbortRestoring
                    or ZLinkActorSourceHandoffPhase.MessageFollowCommitted;
        }
    }

    internal bool CapturesSourceIngress
    {
        get
        {
            lock (_gate)
                return _deferredJoinCapture
                       || _sourcePhase == ZLinkActorSourceHandoffPhase.Capturing;
        }
    }

    public bool RetainsSourceTombstone
    {
        get
        {
            lock (_gate) return _staleSourceActor is not null;
        }
    }

    public bool BlocksLocalDispatch
    {
        get
        {
            lock (_gate)
                return _sourcePhase is ZLinkActorSourceHandoffPhase.Capturing
                           or ZLinkActorSourceHandoffPhase.CutoverPending
                           or ZLinkActorSourceHandoffPhase.AbortRestoring
                           or ZLinkActorSourceHandoffPhase.MessageFollowCommitted
                       || _targetPhase is ZLinkActorTargetHandoffPhase.Importing
                           or ZLinkActorTargetHandoffPhase.AuthorityCommitted
                           or ZLinkActorTargetHandoffPhase.NotifyingJoined
                           or ZLinkActorTargetHandoffPhase.Prepared
                           or ZLinkActorTargetHandoffPhase.Replaying
                           or ZLinkActorTargetHandoffPhase.Failed
                           or ZLinkActorTargetHandoffPhase.Quarantined;
        }
    }

    public void CompleteSourceMigration()
    {
        TaskCompletionSource? completion;
        lock (_gate)
        {
            if (_sourcePhase is not (ZLinkActorSourceHandoffPhase.CutoverPending
                or ZLinkActorSourceHandoffPhase.MessageFollowCommitted))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have a source migration to complete.");
            _sourcePhase = ZLinkActorSourceHandoffPhase.Retired;
            completion = _sourceCompletion;
            _sourceCompletion = null;
        }
        completion?.TrySetResult();
    }

    /// <summary>
    /// Clears the terminal state of the previous local source before this
    /// state is reused for a transferred target activation. The target
    /// import, including its replay queue, is already owned by this state and
    /// must remain intact.
    /// </summary>
    internal void PrepareForTransferredActivation()
    {
        lock (_gate)
        {
            if (_deferredJoinCapture)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' still has a deferred source capture.");
            if (_sourcePhase is not (ZLinkActorSourceHandoffPhase.Idle
                or ZLinkActorSourceHandoffPhase.Retired))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot prepare target activation while "
                    + $"source handoff is {_sourcePhase}.");

            _sourcePhase = ZLinkActorSourceHandoffPhase.Idle;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _sourceHoldFrames.Clear();
            _sourceCaptureSealed = false;
            _sourceCommittedFrameCount = -1;
            _sourceCommittedHoldCount = 0;
            _abortRestoreAdmissionsReleased = false;
            _sourceCompletion?.TrySetResult();
            _sourceCompletion = null;
            _staleSourceActor = null;
            ClearMessageFollowRouteLocked();
            diagnostic?.Invoke("source_handoff_state_cleared_for_target_activation");
        }
    }

    public Task WaitForSourceCompletionAsync(CancellationToken cancellationToken)
    {
        Task completion;
        lock (_gate)
        {
            if (_sourcePhase is not (ZLinkActorSourceHandoffPhase.Capturing
                or ZLinkActorSourceHandoffPhase.CutoverPending
                or ZLinkActorSourceHandoffPhase.MessageFollowCommitted))
                return Task.CompletedTask;
            completion = (_sourceCompletion ??= new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously)).Task;
        }
        return completion.WaitAsync(cancellationToken);
    }

    public ZLinkActorHandoffCaptureResult TryCapture(ZLinkSpotActorFrame frame) =>
        TryCapture<object?>(frame, null, null);

    // Generic state overload so per-frame call sites can use a cached static
    // lambda instead of allocating a closure over (runtime, frame) per frame.
    public ZLinkActorHandoffCaptureResult TryCapture<TState>(
        ZLinkSpotActorFrame frame,
        TState prepareState,
        Action<TState, ZLinkSpotActorFrame>? prepareCapture)
    {
        lock (_gate)
        {
            var capturesSourceIngress =
                _deferredJoinCapture
                || _sourcePhase == ZLinkActorSourceHandoffPhase.Capturing;
            var capturesSourceHold = capturesSourceIngress
                                     && _sourceCaptureSealed;
            var capturesTargetIngress =
                _targetPhase is ZLinkActorTargetHandoffPhase.Importing
                    or ZLinkActorTargetHandoffPhase.AuthorityCommitted
                    or ZLinkActorTargetHandoffPhase.NotifyingJoined
                    or ZLinkActorTargetHandoffPhase.Prepared
                    or ZLinkActorTargetHandoffPhase.Replaying;
            //  Several NotSealed returns share one result; print the values
            //  they test so a frame that is not backlogged names its reason.
            diagnostic?.Invoke(
                $"capture_entry actor={actorId} src_ingress={capturesSourceIngress} "
                + $"tgt_ingress={capturesTargetIngress} direct={frame.RouteContext.IsDirectRoute} "
                + $"flags={frame.Flags} bound_route={frame.RouteContext.IsBoundSessionRoute} "
                + $"arrival={_arrivalIndex} kind={frame.Header.Kind} request_id={frame.RequestId}");
            if (!capturesSourceIngress
                && !capturesTargetIngress)
                return ZLinkActorHandoffCaptureResult.NotSealed;

            if (!frame.RouteContext.IsDirectRoute)
            {
                // A source-ingress request keeps its live reply route and must
                // drain before the session route seal. A target-ingress
                // request is already at the committed authority and can be
                // retained in the target replay queue when its exact binding
                // fence is valid. One-way work can cross either boundary
                // because its accepted sequence is frozen.
                if ((frame.Flags & 1U) != 0 && !capturesTargetIngress)
                    return ZLinkActorHandoffCaptureResult.NotSealed;
                if (!frame.RouteContext.IsBoundSessionRoute
                    || frame.RequestSource is not { } boundSource
                    || frame.SourceNodeGeneration == 0
                    || frame.SourceNodeRid != boundSource.NodeRid
                    || frame.SourceNodeGeneration != boundSource.NodeGeneration
                    || !ZLinkActorBoundSessionHandoffMetadata.TryDecode(
                        frame.ApplicationMetadata.Span,
                        out var boundSession)
                    || boundSession.ActorId != actorId
                    || boundSession.ActorGeneration != frame.Actor.Generation
                    || boundSession.SessionRid != frame.SourceSessionRid)
                    throw new ZLinkActorHandoffRejectedException(
                        $"Actor '{actorId}' received a bound-session frame without "
                        + "an exact owner, binding, and accepted-sequence fence.");
            }

            else if (frame.RequestSource is not { } source
                || frame.SourceNodeGeneration == 0
                || frame.SourceNodeRid != source.NodeRid
                || frame.SourceNodeGeneration != source.NodeGeneration)
                throw new ZLinkActorHandoffRejectedException(
                    $"Actor '{actorId}' cannot accept a direct request without "
                    + "an exact ingress request-source fence.");

            prepareCapture?.Invoke(prepareState, frame);
            var captured = ZLinkActorHandoffFrames.Capture(frame, _arrivalIndex);
            var encodedBytes = capturesSourceIngress || capturesTargetIngress
                ? ZLinkActorHandoffFrames.CanonicalEncodedLength(
                    captured,
                    frame.Actor)
                : 0;
            if (capturesSourceIngress || capturesTargetIngress)
                captured = captured with
                {
                    CanonicalEncodedLength = encodedBytes
                };
            if (capturesSourceIngress
                && !capturesSourceHold
                && !_sourceIngressAdmission.TryAcquire(encodedBytes))
                return ZLinkActorHandoffCaptureResult.Full;
            if (capturesSourceHold
                && !_sourceHoldAdmission.TryAcquire(encodedBytes))
                return ZLinkActorHandoffCaptureResult.Full;
            try
            {
                if (capturesSourceHold)
                    _sourceHoldFrames.Add(captured);
                else
                    _frames.Add(captured);
                _arrivalIndex++;
            }
            catch
            {
                if (capturesSourceIngress && !capturesSourceHold)
                    _sourceIngressAdmission.Release(encodedBytes);
                if (capturesSourceHold)
                    _sourceHoldAdmission.Release(encodedBytes);
                throw;
            }
            diagnostic?.Invoke(
                $"handoff_backlog actor={actorId} arrival={_arrivalIndex - 1} kind={frame.Header.Kind} request_id={frame.RequestId} flags={frame.Flags}");
            return ZLinkActorHandoffCaptureResult.Captured;
        }
    }

    public bool Import(
        ZLinkRemoteActorJoinRequest request,
        out Task<ZLinkRemoteActorJoinReply> preparation)
    {
        var handoffId = request.HandoffId;
        if (string.IsNullOrWhiteSpace(handoffId))
            throw new InvalidOperationException("Actor handoff id must not be empty.");

        lock (_gate)
        {
            if (_sourcePhase is ZLinkActorSourceHandoffPhase.Capturing
                or ZLinkActorSourceHandoffPhase.CutoverPending
                or ZLinkActorSourceHandoffPhase.MessageFollowCommitted)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot import while its source handoff is active.");
            if (string.Equals(_handoffId, handoffId, StringComparison.Ordinal))
            {
                if (_joinRequest is null
                    || !ZLinkActorHandoffRequestIdentity.Matches(_joinRequest, request))
                    throw new InvalidOperationException(
                        $"Actor '{actorId}' handoff '{handoffId}' was retried with different commit data.");
                preparation = _preparation?.Task
                              ?? throw new InvalidOperationException(
                                  $"Actor '{actorId}' handoff '{handoffId}' has no preparation result.");
                return false;
            }
            if (_targetPhase is ZLinkActorTargetHandoffPhase.Importing
                or ZLinkActorTargetHandoffPhase.AuthorityCommitted
                or ZLinkActorTargetHandoffPhase.NotifyingJoined
                or ZLinkActorTargetHandoffPhase.Prepared
                or ZLinkActorTargetHandoffPhase.Replaying
                or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                or ZLinkActorTargetHandoffPhase.Failed
                or ZLinkActorTargetHandoffPhase.Quarantined)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' already has an active handoff '{_handoffId}'.");

            _handoffId = handoffId;
            _joinRequest = request;
            _preparation = new TaskCompletionSource<ZLinkRemoteActorJoinReply>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _targetCompletion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _targetReplayCompletion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            preparation = _preparation.Task;
            _targetPhase = ZLinkActorTargetHandoffPhase.Importing;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _arrivalIndex = 0;
            foreach (var frame in request.HandoffFrames.OrderBy(static frame => frame.ArrivalIndex))
            {
                _frames.Add(frame with
                {
                    ArrivalIndex = _arrivalIndex++,
                    CanonicalEncodedLength = 0
                });
                diagnostic?.Invoke(
                    $"backlog_enqueued actor={actorId} arrival={_arrivalIndex - 1} request_id={frame.RequestId} flags={frame.Flags}");
            }
            _importedFrameCount = _frames.Count;
            _sourceTrailingImported = false;
            return true;
        }
    }

    internal void BeginCanonicalMaintenanceImport(
        string handoffId,
        IReadOnlyList<ZLinkActorHandoffFrame> frames)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(handoffId);
        ArgumentNullException.ThrowIfNull(frames);
        lock (_gate)
        {
            var adoptingPreparedRemoteJoin =
                string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                && _targetPhase == ZLinkActorTargetHandoffPhase.Prepared;
            if (string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                && _targetPhase is ZLinkActorTargetHandoffPhase.Importing
                    or ZLinkActorTargetHandoffPhase.AuthorityCommitted
                    or ZLinkActorTargetHandoffPhase.Replaying
                    or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                    or ZLinkActorTargetHandoffPhase.Failed
                    or ZLinkActorTargetHandoffPhase.Completed)
                return;
            if (!adoptingPreparedRemoteJoin
                && (_sourcePhase is ZLinkActorSourceHandoffPhase.Capturing
                    or ZLinkActorSourceHandoffPhase.CutoverPending
                    or ZLinkActorSourceHandoffPhase.MessageFollowCommitted
                || _targetPhase is ZLinkActorTargetHandoffPhase.Importing
                    or ZLinkActorTargetHandoffPhase.AuthorityCommitted
                    or ZLinkActorTargetHandoffPhase.NotifyingJoined
                    or ZLinkActorTargetHandoffPhase.Prepared
                    or ZLinkActorTargetHandoffPhase.Replaying
                    or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                    or ZLinkActorTargetHandoffPhase.Failed
                    or ZLinkActorTargetHandoffPhase.Quarantined))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' already has an active handoff transaction.");
            _handoffId = handoffId;
            _joinRequest = null;
            _preparation = null;
            _targetCompletion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _targetReplayCompletion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            _canonicalMaintenanceDrain = null;
            _canonicalMaintenanceReplayReservations.Clear();
            _targetPhase = ZLinkActorTargetHandoffPhase.Importing;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            long previousSequence = 0;
            try
            {
                foreach (var frame in frames.OrderBy(
                             static frame => frame.ArrivalIndex))
                {
                    if (frame.ArrivalIndex <= previousSequence
                        || frame.CanonicalEncodedLength <= 0)
                        throw new ZLinkRelocationDataLostException(
                            $"Actor '{actorId}' canonical accepted sequence or size is invalid.");
                    _frames.Add(frame);
                    previousSequence = frame.ArrivalIndex;
                }
            }
            catch
            {
                _frames.Clear();
                _canonicalMaintenanceReplayReservations.Clear();
                _handoffId = null;
                _targetPhase = ZLinkActorTargetHandoffPhase.RolledBack;
                throw;
            }
            _arrivalIndex = checked(previousSequence + 1);
            _importedFrameCount = _frames.Count;
            _sourceTrailingImported = true;
        }
    }

    internal void AppendCanonicalMaintenanceImport(
        string handoffId,
        IReadOnlyList<ZLinkActorHandoffFrame> frames)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(handoffId);
        ArgumentNullException.ThrowIfNull(frames);
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                || _targetPhase != ZLinkActorTargetHandoffPhase.Importing)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have the matching canonical import.");
            var ordered = frames.OrderBy(static frame => frame.ArrivalIndex)
                .ToArray();
            var expectedSequence = _arrivalIndex;
            foreach (var frame in ordered)
            {
                if (frame.ArrivalIndex != expectedSequence
                    || frame.CanonicalEncodedLength <= 0)
                    throw new ZLinkRelocationDataLostException(
                        $"Actor '{actorId}' canonical delta sequence or size is invalid.");
                expectedSequence++;
            }
            foreach (var frame in ordered)
            {
                _frames.Add(frame);
                _arrivalIndex++;
            }
            _importedFrameCount = _frames.Count;
        }
    }

    internal void AppendPreparedImport(
        string handoffId,
        IReadOnlyList<ZLinkActorHandoffFrame> frames)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(handoffId);
        ArgumentNullException.ThrowIfNull(frames);
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                || _joinRequest is null
                || _targetPhase != ZLinkActorTargetHandoffPhase.Importing)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have the matching prepared import.");
            foreach (var frame in frames.OrderBy(static frame => frame.ArrivalIndex))
            {
                _frames.Add(frame with
                {
                    ArrivalIndex = _arrivalIndex++,
                    CanonicalEncodedLength = 0
                });
                diagnostic?.Invoke(
                    $"backlog_enqueued actor={actorId} arrival={_arrivalIndex - 1} request_id={frame.RequestId} flags={frame.Flags}");
            }
            _importedFrameCount = _frames.Count;
        }
    }

    public void CompleteJoinedNotification(
        string handoffId,
        ZLinkRemoteActorJoinReply reply)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                || _targetPhase != ZLinkActorTargetHandoffPhase.NotifyingJoined)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' handoff joined notification is not active.");
            _targetPhase = ZLinkActorTargetHandoffPhase.Prepared;
            _preparation!.TrySetResult(reply);
        }
    }

    public Task<ZLinkRemoteActorJoinReply> WaitForPreparationAsync(
        string handoffId,
        CancellationToken cancellationToken)
    {
        Task<ZLinkRemoteActorJoinReply> preparation;
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                || _preparation is null)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have an active handoff preparation.");
            preparation = _preparation.Task;
        }
        return preparation.WaitAsync(cancellationToken);
    }

    public void MarkAuthorityCommitted(
        string handoffId,
        ulong sourceObjectGeneration,
        ulong targetObjectGeneration)
    {
        if (sourceObjectGeneration == 0
            || targetObjectGeneration != sourceObjectGeneration)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{actorId}' target changed ObjectGeneration during handoff.");

        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                || _targetPhase != ZLinkActorTargetHandoffPhase.Importing)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot commit authority for an inactive handoff.");
            _targetPhase = ZLinkActorTargetHandoffPhase.AuthorityCommitted;
        }
    }

    public bool IsAuthorityCommitted(string handoffId)
    {
        lock (_gate)
            return string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                   && _targetPhase is ZLinkActorTargetHandoffPhase.AuthorityCommitted
                       or ZLinkActorTargetHandoffPhase.NotifyingJoined
                       or ZLinkActorTargetHandoffPhase.Prepared
                       or ZLinkActorTargetHandoffPhase.Replaying
                       or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                       or ZLinkActorTargetHandoffPhase.Failed
                       or ZLinkActorTargetHandoffPhase.Completed;
    }

    public bool TryBeginJoinedNotification(string handoffId)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot notify an inactive handoff.");
            if (_targetPhase != ZLinkActorTargetHandoffPhase.AuthorityCommitted)
                return false;
            _targetPhase = ZLinkActorTargetHandoffPhase.NotifyingJoined;
            return true;
        }
    }

    public bool IsJoinedNotificationCompleted(string handoffId)
    {
        lock (_gate)
            return string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                   && _targetPhase is ZLinkActorTargetHandoffPhase.Prepared
                       or ZLinkActorTargetHandoffPhase.Replaying
                       or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                       or ZLinkActorTargetHandoffPhase.Completed;
    }

    public void RetryJoinedNotification(string handoffId)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                || _targetPhase != ZLinkActorTargetHandoffPhase.NotifyingJoined)
                return;
            _targetPhase = ZLinkActorTargetHandoffPhase.AuthorityCommitted;
        }
    }

    public bool FailJoinedNotification(
        string handoffId,
        ZLinkRemoteActorJoinReply reply)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot fail an inactive handoff.");
            if (_targetPhase == ZLinkActorTargetHandoffPhase.Failed) return false;
            if (_targetPhase is not (ZLinkActorTargetHandoffPhase.AuthorityCommitted
                or ZLinkActorTargetHandoffPhase.NotifyingJoined))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' handoff joined notification cannot become terminally failed.");
            _targetPhase = ZLinkActorTargetHandoffPhase.Failed;
            _preparation!.TrySetResult(reply);
            _targetCompletion?.TrySetException(
                new InvalidOperationException(
                    $"Actor '{actorId}' target handoff '{handoffId}' failed."));
            _targetReplayCompletion?.TrySetException(
                new InvalidOperationException(
                    $"Actor '{actorId}' target handoff '{handoffId}' failed."));
            _deferredJoinAwaitingTarget = false;
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _importedFrameCount = 0;
            _sourceTrailingImported = false;
            _canonicalMaintenanceReplayReservations.Clear();
            return true;
        }
    }

    public void RejectPreparation(string handoffId, ZLinkRemoteActorJoinReply reply)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)) return;
            if (_targetPhase is not (ZLinkActorTargetHandoffPhase.Importing
                or ZLinkActorTargetHandoffPhase.AuthorityCommitted
                or ZLinkActorTargetHandoffPhase.NotifyingJoined
                or ZLinkActorTargetHandoffPhase.Prepared
                or ZLinkActorTargetHandoffPhase.Replaying
                or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                or ZLinkActorTargetHandoffPhase.Quarantined))
                return;
            _preparation?.TrySetResult(reply);
        }
    }

    public bool IsKnown(string handoffId)
    {
        lock (_gate) return string.Equals(_handoffId, handoffId, StringComparison.Ordinal);
    }

    public bool IsQuarantined(string handoffId)
    {
        lock (_gate)
            return _targetPhase == ZLinkActorTargetHandoffPhase.Quarantined
                   && string.Equals(_handoffId, handoffId, StringComparison.Ordinal);
    }

    public void Quarantine(string handoffId)
    {
        lock (_gate)
        {
            if (string.Equals(_handoffId, handoffId, StringComparison.Ordinal))
                _targetPhase = ZLinkActorTargetHandoffPhase.Quarantined;
        }
    }

    public void AbortImport(string handoffId)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)) return;
            _targetCompletion?.TrySetException(
                new InvalidOperationException(
                    $"Actor '{actorId}' target handoff '{handoffId}' was aborted."));
            _targetReplayCompletion?.TrySetException(
                new InvalidOperationException(
                    $"Actor '{actorId}' target handoff '{handoffId}' was aborted."));
            _deferredJoinAwaitingTarget = false;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _importedFrameCount = 0;
            _sourceTrailingImported = false;
            _handoffId = null;
            _joinRequest = null;
            _targetPhase = ZLinkActorTargetHandoffPhase.RolledBack;
            _preparation = null;
            _targetCompletion = null;
            _targetReplayCompletion = null;
            _canonicalMaintenanceDrain = null;
            _canonicalMaintenanceReplayReservations.Clear();
        }
    }

    public void Complete(string handoffId)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot complete an inactive handoff.");
            if (TryCompleteTargetHandoffLocked(handoffId))
                return;
            throw new InvalidOperationException(
                $"Actor '{actorId}' cannot complete before target replay drains.");
        }
    }

    internal bool TryCompleteTransferredActorReplay(string handoffId)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal))
                return false;
            return TryCompleteTargetHandoffLocked(handoffId);
        }
    }

    private bool TryCompleteTargetHandoffLocked(string handoffId)
    {
        if (_targetPhase == ZLinkActorTargetHandoffPhase.Completed)
        {
            _canonicalMaintenanceDrain = null;
            _canonicalMaintenanceReplayReservations.Clear();
            _targetReplayCompletion?.TrySetResult();
            ReleaseTargetCompletionLocked(handoffId);
            return true;
        }
        if (_targetPhase != ZLinkActorTargetHandoffPhase.Replaying
            || _frames.Count != 0)
            return false;

        // The phase change and the empty-frame check share this lock. A late
        // ingress frame is therefore either captured before this boundary and
        // replayed by the caller, or admitted directly after the target
        // handoff has completed.
        _targetPhase = ZLinkActorTargetHandoffPhase.Completed;
        _sourceTrailingImported = false;
        _canonicalMaintenanceReplayReservations.Clear();
        _targetReplayCompletion?.TrySetResult();
        ReleaseTargetCompletionLocked(handoffId);
        return true;
    }

    private void ReleaseTargetCompletionLocked(string handoffId)
    {
        if (_deferredJoinAwaitingTarget)
        {
            _deferredJoinAwaitingTarget = false;
            BeginDeferredJoinCaptureLocked();
            diagnostic?.Invoke(
                $"deferred_join_capture_started_after_target actor={actorId} "
                + $"handoff={handoffId}");
        }
        _targetCompletion?.TrySetResult();
    }

    public IReadOnlyList<ZLinkActorHandoffFrame> SnapshotFrames()
    {
        lock (_gate)
        {
            if (_sourcePhase != ZLinkActorSourceHandoffPhase.Capturing)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' source handoff capture is not active.");
            return _frames.ToArray();
        }
    }

    internal void SealCapture()
    {
        lock (_gate)
        {
            if (!CanSealCaptureLocked())
                throw new InvalidOperationException(
                    $"Actor '{actorId}' source handoff capture cannot be sealed.");
            _sourceCaptureSealed = true;
        }
    }

    /// <summary>
    /// Seals capture and attaches the relocation unit's SafeToShutdown
    /// obligation token (spec 30 §11) in the same locked transition, so no
    /// window exists between "sealed" and "counted" where a status read
    /// could observe SafeToShutdown=true for an obligation the seal just
    /// created, and no abort/reset racing the attach can leak the token —
    /// a seal that throws disposes it immediately instead of orphaning it.
    /// </summary>
    internal void SealCapture(IDisposable shutdownToken)
    {
        ArgumentNullException.ThrowIfNull(shutdownToken);
        lock (_gate)
        {
            if (!CanSealCaptureLocked())
            {
                shutdownToken.Dispose();
                throw new InvalidOperationException(
                    $"Actor '{actorId}' source handoff capture cannot be sealed.");
            }
            _sourceCaptureSealed = true;
            PendingShutdownToken = shutdownToken;
        }
    }

    //  Shared by both SealCapture overloads: a capture may be sealed exactly
    //  once, and only while still in progress. Caller must hold _gate.
    private bool CanSealCaptureLocked() =>
        _sourcePhase == ZLinkActorSourceHandoffPhase.Capturing
        && !_sourceCaptureSealed;

    internal ZLinkActorHandoffCommitBoundary FreezeCaptureCommitBoundary()
    {
        lock (_gate)
        {
            if (_sourcePhase != ZLinkActorSourceHandoffPhase.Capturing
                || !_sourceCaptureSealed
                || _sourceCommittedFrameCount >= 0)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' source commit boundary cannot be frozen.");
            var frames = _frames
                .Concat(_sourceHoldFrames)
                .OrderBy(static frame => frame.ArrivalIndex)
                .ToArray();
            _sourceCommittedFrameCount = frames.Length;
            _sourceCommittedHoldCount = _sourceHoldFrames.Count;
            return new ZLinkActorHandoffCommitBoundary(
                frames,
                checked((ulong)frames.Length),
                _sourceHoldAdmission.RemainingRecordCapacity,
                _sourceHoldAdmission.RemainingByteCapacity);
        }
    }

    public IReadOnlyList<ZLinkActorHandoffFrame> CutoverCaptureToMessageFollow(
        int committedFrameCount,
        ZLinkBackendActorRef sourceActor,
        ZLinkBackendActorRef targetActor,
        string targetMeshName,
        ulong sourceNodeGeneration,
        ulong targetNodeGeneration,
        ulong sourceAuthorityOwnerGeneration,
        ulong targetAuthorityOwnerGeneration,
        ulong sourceOwnerLeaseGeneration,
        ulong targetOwnerLeaseGeneration)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(targetMeshName);
        if (sourceNodeGeneration == 0
            || targetNodeGeneration == 0
            || sourceAuthorityOwnerGeneration == 0
            || sourceAuthorityOwnerGeneration > long.MaxValue
            || targetAuthorityOwnerGeneration
               is 0 or > long.MaxValue
            || targetAuthorityOwnerGeneration <= sourceAuthorityOwnerGeneration
            || sourceOwnerLeaseGeneration == 0
            || targetOwnerLeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(
                nameof(targetAuthorityOwnerGeneration),
                "Actor Message Follow requires an exact committed source-to-target authority fence.");
        lock (_gate)
        {
            if (_sourcePhase != ZLinkActorSourceHandoffPhase.Capturing)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have an active source handoff capture.");
            if (committedFrameCount < 0
                || (!_sourceCaptureSealed
                    && committedFrameCount > _frames.Count)
                || (_sourceCaptureSealed
                    && (_sourceCommittedFrameCount < 0
                        || committedFrameCount
                        != _sourceCommittedFrameCount)))
                throw new ArgumentOutOfRangeException(nameof(committedFrameCount));

            _messageFollowExpiry?.Cancel();
            _messageFollowExpiry = null;
            _messageFollowRoute = new ZLinkActorMessageFollowRoute(
                sourceActor,
                targetActor,
                targetMeshName,
                sourceNodeGeneration,
                targetNodeGeneration,
                sourceAuthorityOwnerGeneration,
                targetAuthorityOwnerGeneration,
                sourceOwnerLeaseGeneration,
                targetOwnerLeaseGeneration,
                new ZLinkActorMessageFollowLease(timeProvider));
            _staleSourceActor = sourceActor;
            _sourcePhase = ZLinkActorSourceHandoffPhase.CutoverPending;
            _sourceCaptureSealed = true;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            diagnostic?.Invoke(
                $"message_follow_registered source_rid={sourceActor.NodeRid} "
                + $"target_rid={targetActor.NodeRid} entries=1");
            return _frames
                .Skip(Math.Min(committedFrameCount, _frames.Count))
                .Concat(_sourceHoldFrames.Skip(_sourceCommittedHoldCount))
                .OrderBy(static frame => frame.ArrivalIndex)
                .ToArray();
        }
    }

    /// <param name="duration">
    /// The Message Follow route lifetime — this source's clock measures S4
    /// (route removable) as this long after this call (spec 30 §11).
    /// </param>
    /// <param name="retransmissionWindow">
    /// The cutover retransmission window (spec 28 §4.4/30 §11). SafeToShutdown
    /// requires both S4 and this window to have elapsed, so the obligation
    /// release waits <c>max(duration, retransmissionWindow)</c> from this
    /// call — the route_convergence metric below is unaffected and still
    /// measures S1→S4 alone (spec 25 §5).
    /// </param>
    public void CommitMessageFollow(
        TimeSpan duration,
        TimeSpan retransmissionWindow = default)
    {
        CancellationTokenSource expiry;
        long committedAt;
        lock (_gate)
        {
            if (_sourcePhase != ZLinkActorSourceHandoffPhase.CutoverPending
                || _messageFollowRoute is not { } messageFollowRoute)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have a pending Message Follow commit.");

            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _sourcePhase = ZLinkActorSourceHandoffPhase.MessageFollowCommitted;
            _sourceCaptureSealed = true;
            expiry = new CancellationTokenSource();
            _messageFollowExpiry = expiry;
            //  S1 (spec 25 §5): the cutover submit terminal this class
            //  observes. Taken from timeProvider so it is source-local
            //  and deterministic under a fake clock in tests.
            committedAt = timeProvider.GetTimestamp();
            messageFollowRoute.Lease.Commit(duration);
        }

        _ = RemoveMessageFollowRouteAfterDelayAsync(
            duration, retransmissionWindow, committedAt, expiry);
    }

    public IReadOnlyList<ZLinkActorHandoffFrame> PrepareImportedReplay(
        IReadOnlyList<ZLinkActorHandoffFrame> sourceTrailingFrames)
    {
        lock (_gate)
        {
            if (_targetPhase == ZLinkActorTargetHandoffPhase.Completed
                && sourceTrailingFrames.Count == 0)
                return [];
            if (_targetPhase is not (ZLinkActorTargetHandoffPhase.Importing
                or ZLinkActorTargetHandoffPhase.Prepared
                or ZLinkActorTargetHandoffPhase.Replaying))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have a target handoff to replay.");
            _targetPhase = ZLinkActorTargetHandoffPhase.Replaying;
            if (!_sourceTrailingImported)
            {
                var trailing = sourceTrailingFrames
                    .OrderBy(static frame => frame.ArrivalIndex)
                    .Select(static frame => frame with
                    {
                        CanonicalEncodedLength = 0
                    })
                    .ToArray();
                var trailingStart = _importedFrameCount;
                _frames.InsertRange(_importedFrameCount, trailing);
                for (var index = 0; index < _frames.Count; index++)
                    _frames[index] = _frames[index] with { ArrivalIndex = index };
                for (var index = trailingStart; index < _frames.Count; index++)
                {
                    var frame = _frames[index];
                    diagnostic?.Invoke(
                        $"backlog_enqueued actor={actorId} arrival={frame.ArrivalIndex} request_id={frame.RequestId} flags={frame.Flags}");
                }
                _arrivalIndex = _frames.Count;
                _sourceTrailingImported = true;
                _importedFrameCount = 0;
            }

            return _frames.ToArray();
        }
    }

    internal IReadOnlyList<ZLinkActorHandoffFrame>
        PrepareCanonicalMaintenanceReplay(string handoffId)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId, StringComparison.Ordinal)
                || _targetPhase is not (
                    ZLinkActorTargetHandoffPhase.AuthorityCommitted
                    or ZLinkActorTargetHandoffPhase.Replaying))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' has no committed canonical maintenance import.");
            _targetPhase = ZLinkActorTargetHandoffPhase.Replaying;
            return _frames.ToArray();
        }
    }

    internal bool IsCanonicalMaintenanceReplayComplete(string handoffId)
    {
        lock (_gate)
            return string.Equals(_handoffId, handoffId,
                StringComparison.Ordinal)
                   && _targetPhase == ZLinkActorTargetHandoffPhase.Completed;
    }

    internal bool IsCanonicalMaintenanceHandoff(string handoffId)
    {
        lock (_gate)
        {
            // Canonical maintenance imports do not have a remote join request.
            // That identity keeps recovery from starting a second replay of the
            // same durable accepted journal.
            return string.Equals(_handoffId, handoffId,
                       StringComparison.Ordinal)
                   && _joinRequest is null
                   && _sourceTrailingImported
                   && _targetPhase is
                       ZLinkActorTargetHandoffPhase.Importing
                       or ZLinkActorTargetHandoffPhase.AuthorityCommitted
                       or ZLinkActorTargetHandoffPhase.NotifyingJoined
                       or ZLinkActorTargetHandoffPhase.Prepared
                       or ZLinkActorTargetHandoffPhase.Replaying
                       or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                       or ZLinkActorTargetHandoffPhase.Completed;
        }
    }

    internal Task WaitForTargetReplayCompletionAsync(
        CancellationToken cancellationToken)
    {
        Task completion;
        lock (_gate)
        {
            if (_targetPhase == ZLinkActorTargetHandoffPhase.Completed)
                return Task.CompletedTask;
            completion = _targetReplayCompletion?.Task
                         ?? throw new InvalidOperationException(
                             $"Actor '{actorId}' target handoff has no replay completion boundary.");
        }
        return completion.WaitAsync(cancellationToken);
    }

    internal bool TryOpenCanonicalMaintenanceAdmission(
        string handoffId,
        long queuedThroughArrivalIndex)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId,
                    StringComparison.Ordinal)
                || _targetPhase != ZLinkActorTargetHandoffPhase.Replaying)
                return false;
            if (_frames.Any(frame =>
                    frame.ArrivalIndex > queuedThroughArrivalIndex))
                return false;
            _targetPhase =
                ZLinkActorTargetHandoffPhase.AdmissionOpenDraining;
            return true;
        }
    }

    internal void ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
            string handoffId,
            long queuedThroughArrivalIndex,
            Action<ZLinkActorHandoffFrame> reserveReplay)
    {
        ReserveCanonicalMaintenanceTrailing(
            handoffId,
            queuedThroughArrivalIndex,
            reserveReplay,
            openAdmission: true);
    }

    internal IReadOnlyList<Task>
        ReserveCanonicalMaintenanceTrailingAndOpenAdmission(
            string handoffId,
            long queuedThroughArrivalIndex,
            Func<ZLinkActorHandoffFrame, Task> reserveReplay)
    {
        return ReserveCanonicalMaintenanceTrailing(
            handoffId,
            queuedThroughArrivalIndex,
            reserveReplay,
            openAdmission: true);
    }

    internal void ReserveCanonicalMaintenanceTrailing(
        string handoffId,
        long queuedThroughArrivalIndex,
        Action<ZLinkActorHandoffFrame> reserveReplay)
    {
        ReserveCanonicalMaintenanceTrailing(
            handoffId,
            queuedThroughArrivalIndex,
            reserveReplay,
            openAdmission: false);
    }

    internal IReadOnlyList<Task> ReserveCanonicalMaintenanceTrailing(
        string handoffId,
        long queuedThroughArrivalIndex,
        Func<ZLinkActorHandoffFrame, Task> reserveReplay)
    {
        return ReserveCanonicalMaintenanceTrailing(
            handoffId,
            queuedThroughArrivalIndex,
            reserveReplay,
            openAdmission: false);
    }

    private void ReserveCanonicalMaintenanceTrailing(
            string handoffId,
            long queuedThroughArrivalIndex,
            Action<ZLinkActorHandoffFrame> reserveReplay,
            bool openAdmission)
    {
        _ = ReserveCanonicalMaintenanceTrailing(
            handoffId,
            queuedThroughArrivalIndex,
            frame =>
            {
                reserveReplay(frame);
                return Task.CompletedTask;
            },
            openAdmission);
    }

    private IReadOnlyList<Task> ReserveCanonicalMaintenanceTrailing(
            string handoffId,
            long queuedThroughArrivalIndex,
            Func<ZLinkActorHandoffFrame, Task> reserveReplay,
            bool openAdmission)
    {
        ArgumentNullException.ThrowIfNull(reserveReplay);
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId,
                    StringComparison.Ordinal)
                || _targetPhase != ZLinkActorTargetHandoffPhase.Replaying)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot open canonical maintenance admission.");
            var trailing = _frames
                .Where(frame =>
                    frame.ArrivalIndex > queuedThroughArrivalIndex)
                // Frames relayed by the previous owner were accepted before
                // the authority cutover. They must reserve execution before
                // direct ingress that observed the new owner, even when the
                // network delivers the direct frame first.
                .OrderBy(static frame =>
                    frame.RouteContext.MessageFollowHopCount == 0 ? 1 : 0)
                .ThenBy(static frame => frame.ArrivalIndex)
                .ToArray();
            // ACK removes the frame before DispatchReplayAsync necessarily
            // finishes its post-handler reconciliation. Keep every reserved
            // task in the drain until the whole replay completes so a retry
            // cannot mistake an ACKed but still-running dispatch for durable
            // completion.
            var reservations =
                _canonicalMaintenanceReplayReservations
                    .OrderBy(static reservation => reservation.Key)
                    .Select(static reservation => reservation.Value)
                    .ToList();
            foreach (var frame in trailing)
            {
                if (_canonicalMaintenanceReplayReservations.TryGetValue(
                        frame.ArrivalIndex,
                        out _))
                    continue;

                var reservation = reserveReplay(frame)
                                  ?? throw new InvalidOperationException(
                                      $"Actor '{actorId}' replay reservation returned no completion task.");
                _canonicalMaintenanceReplayReservations.Add(
                    frame.ArrivalIndex,
                    reservation);
                reservations.Add(reservation);
            }
            if (openAdmission)
            {
                // New target ingress can bypass capture only after every
                // preserved frame has reserved its mailbox position.
                _targetPhase =
                    ZLinkActorTargetHandoffPhase.AdmissionOpenDraining;
            }
            return reservations;
        }
    }

    internal void RegisterCanonicalMaintenanceDrain(
        string handoffId,
        Task drain)
    {
        ArgumentNullException.ThrowIfNull(drain);
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId,
                    StringComparison.Ordinal)
                || _targetPhase
                   != ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                || _canonicalMaintenanceDrain is not null)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot register the canonical drain.");
            _canonicalMaintenanceDrain = drain;
        }
    }

    internal Task? GetCanonicalMaintenanceDrain(string handoffId)
    {
        lock (_gate)
            return string.Equals(_handoffId, handoffId,
                StringComparison.Ordinal)
                ? _canonicalMaintenanceDrain
                : null;
    }

    internal bool TryCompleteCanonicalMaintenanceReplay(string handoffId)
    {
        lock (_gate)
        {
            if (!string.Equals(_handoffId, handoffId,
                    StringComparison.Ordinal)
                || _targetPhase
                   != ZLinkActorTargetHandoffPhase.AdmissionOpenDraining
                || _frames.Count != 0)
                return false;
            _targetPhase = ZLinkActorTargetHandoffPhase.Completed;
            _sourceTrailingImported = false;
            _canonicalMaintenanceReplayReservations.Clear();
            _targetReplayCompletion?.TrySetResult();
            ReleaseTargetCompletionLocked(handoffId);
            return true;
        }
    }

    public IReadOnlyList<ZLinkActorHandoffFrame> SnapshotFinalReplay()
    {
        lock (_gate)
        {
            if (_targetPhase == ZLinkActorTargetHandoffPhase.Completed)
                return [];
            if (_targetPhase is not (
                    ZLinkActorTargetHandoffPhase.Replaying
                    or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' target handoff replay is not active.");
            return _frames.ToArray();
        }
    }

    public void AcknowledgeReplayedFrame()
    {
        lock (_gate)
        {
            if (_targetPhase is not (
                    ZLinkActorTargetHandoffPhase.Replaying
                    or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot acknowledge replay outside target replay.");
            if (_frames.Count == 0)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have a handoff frame to acknowledge.");
            _frames.RemoveAt(0);
        }
    }

    internal void AcknowledgeReplayedFrame(long arrivalIndex)
    {
        lock (_gate)
        {
            if (_targetPhase is not (
                    ZLinkActorTargetHandoffPhase.Replaying
                    or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot acknowledge replay outside target replay.");
            var index = _frames.FindIndex(frame =>
                frame.ArrivalIndex == arrivalIndex);
            if (index < 0)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have replay frame '{arrivalIndex}' to acknowledge.");
            _frames.RemoveAt(index);
        }
    }

    internal void AcknowledgeCanonicalReplayThrough(ulong acceptedSequence)
    {
        lock (_gate)
        {
            if (_targetPhase is not (
                    ZLinkActorTargetHandoffPhase.Replaying
                    or ZLinkActorTargetHandoffPhase.AdmissionOpenDraining))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' cannot acknowledge canonical replay outside target replay.");
            while (_frames.Count != 0
                   && checked((ulong)_frames[0].ArrivalIndex) <= acceptedSequence)
            {
                _frames.RemoveAt(0);
            }
        }
    }

    public IReadOnlyList<ZLinkActorHandoffFrame> AbortCapture()
    {
        var frames = BeginAbortCaptureRestore();
        foreach (var frame in frames)
            AcknowledgeAbortRestoreEnqueued(frame.ArrivalIndex);
        CompleteAbortCaptureRestore();
        return frames;
    }

    internal IReadOnlyList<ZLinkActorHandoffFrame> BeginAbortCaptureRestore()
    {
        lock (_gate)
        {
            if (_sourcePhase is not (ZLinkActorSourceHandoffPhase.Capturing
                or ZLinkActorSourceHandoffPhase.CutoverPending
                or ZLinkActorSourceHandoffPhase.AbortRestoring))
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have an abortable source handoff.");
            if (_sourcePhase != ZLinkActorSourceHandoffPhase.AbortRestoring)
                _abortRestoreAdmissionsReleased =
                    _sourcePhase == ZLinkActorSourceHandoffPhase.CutoverPending;
            _sourcePhase = ZLinkActorSourceHandoffPhase.AbortRestoring;
            return _frames
                .Concat(_sourceHoldFrames)
                .OrderBy(static frame => frame.ArrivalIndex)
                .ToArray();
        }
    }

    internal void AcknowledgeAbortRestoreEnqueued(long arrivalIndex)
    {
        lock (_gate)
        {
            if (_sourcePhase != ZLinkActorSourceHandoffPhase.AbortRestoring)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have an active abort restore.");
            var sourceIndex = _frames.Count == 0
                ? -1
                : _frames.FindIndex(frame => frame.ArrivalIndex == arrivalIndex);
            var holdIndex = _sourceHoldFrames.Count == 0
                ? -1
                : _sourceHoldFrames.FindIndex(
                    frame => frame.ArrivalIndex == arrivalIndex);
            var nextArrival = _frames
                .Concat(_sourceHoldFrames)
                .Select(static frame => frame.ArrivalIndex)
                .DefaultIfEmpty(long.MinValue)
                .Min();
            if (nextArrival != arrivalIndex
                || sourceIndex < 0 && holdIndex < 0)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' abort restore acknowledgement is out of order.");

            if (sourceIndex >= 0)
            {
                var frame = _frames[sourceIndex];
                _frames.RemoveAt(sourceIndex);
                if (!_abortRestoreAdmissionsReleased)
                    _sourceIngressAdmission.Release(
                        frame.CanonicalEncodedLength);
            }
            else
            {
                var frame = _sourceHoldFrames[holdIndex];
                _sourceHoldFrames.RemoveAt(holdIndex);
                if (!_abortRestoreAdmissionsReleased)
                    _sourceHoldAdmission.Release(
                        frame.CanonicalEncodedLength);
            }
        }
    }

    internal void CompleteAbortCaptureRestore()
    {
        TaskCompletionSource? completion;
        lock (_gate)
        {
            if (_sourcePhase != ZLinkActorSourceHandoffPhase.AbortRestoring)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' does not have an abort restore to complete.");
            if (_frames.Count != 0 || _sourceHoldFrames.Count != 0)
                throw new InvalidOperationException(
                    $"Actor '{actorId}' abort restore still has queued frames.");
            _sourcePhase = ZLinkActorSourceHandoffPhase.Idle;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _importedFrameCount = 0;
            _sourceTrailingImported = false;
            _sourceCaptureSealed = false;
            _sourceCommittedFrameCount = -1;
            _sourceCommittedHoldCount = 0;
            _abortRestoreAdmissionsReleased = false;
            _staleSourceActor = null;
            completion = _sourceCompletion;
            _sourceCompletion = null;
            ClearMessageFollowRouteLocked();
            completion?.TrySetResult();
        }
    }

    public ZLinkActorFrameRoute ResolveFrameRoute(
        ZLinkBackendActorRef? currentActor,
        ZLinkBackendActorRef frameActor,
        out ZLinkBackendActorRef targetActor)
    {
        lock (_gate)
        {
            return ResolveFrameRouteLocked(currentActor, frameActor, out targetActor);
        }
    }

    public ZLinkActorFrameRoute RouteFrame(
        ZLinkBackendActorRef? currentActor,
        ZLinkBackendActorRef frameActor,
        out ZLinkActorMessageFollowRoute? messageFollowRoute)
    {
        lock (_gate)
        {
            var targetActor = default(ZLinkBackendActorRef);
            var route = ResolveFrameRouteLocked(currentActor, frameActor, out targetActor);
            messageFollowRoute = route == ZLinkActorFrameRoute.MessageFollow
                ? _messageFollowRoute
                : null;
            return route;
        }
    }

    public void Reset()
    {
        lock (_gate)
        {
            _sourcePhase = ZLinkActorSourceHandoffPhase.Idle;
            _targetPhase = ZLinkActorTargetHandoffPhase.Idle;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _importedFrameCount = 0;
            _sourceTrailingImported = false;
            _sourceCaptureSealed = false;
            _sourceCommittedFrameCount = -1;
            _sourceCommittedHoldCount = 0;
            _handoffId = null;
            _joinRequest = null;
            _preparation = null;
            _targetCompletion?.TrySetException(
                new InvalidOperationException(
                    $"Actor '{actorId}' target handoff was reset."));
            _targetCompletion = null;
            _targetReplayCompletion?.TrySetException(
                new InvalidOperationException(
                    $"Actor '{actorId}' target handoff was reset."));
            _targetReplayCompletion = null;
            _deferredJoinAwaitingTarget = false;
            _canonicalMaintenanceDrain = null;
            _canonicalMaintenanceReplayReservations.Clear();
            _sourceCompletion?.TrySetResult();
            _sourceCompletion = null;
            _staleSourceActor = null;
            ClearMessageFollowRouteLocked();
        }
    }

    public void AbortRuntimeGeneration(Exception failure)
    {
        ArgumentNullException.ThrowIfNull(failure);
        TaskCompletionSource<ZLinkRemoteActorJoinReply>? preparation;
        lock (_gate)
        {
            preparation = _preparation;
            _sourcePhase = ZLinkActorSourceHandoffPhase.Idle;
            _targetPhase = ZLinkActorTargetHandoffPhase.Idle;
            _sourceIngressAdmission.ReleaseAll();
            _sourceHoldAdmission.ReleaseAll();
            _frames.Clear();
            _sourceHoldFrames.Clear();
            _importedFrameCount = 0;
            _sourceTrailingImported = false;
            _sourceCaptureSealed = false;
            _sourceCommittedFrameCount = -1;
            _sourceCommittedHoldCount = 0;
            _handoffId = null;
            _joinRequest = null;
            _preparation = null;
            _targetCompletion?.TrySetException(failure);
            _targetCompletion = null;
            _targetReplayCompletion?.TrySetException(failure);
            _targetReplayCompletion = null;
            _deferredJoinAwaitingTarget = false;
            _canonicalMaintenanceDrain = null;
            _canonicalMaintenanceReplayReservations.Clear();
            _sourceCompletion?.TrySetException(failure);
            _sourceCompletion = null;
            _staleSourceActor = null;
            ClearMessageFollowRouteLocked();
        }

        preparation?.TrySetException(failure);
    }

    private ZLinkActorFrameRoute ResolveFrameRouteLocked(
        ZLinkBackendActorRef? currentActor,
        ZLinkBackendActorRef frameActor,
        out ZLinkBackendActorRef targetActor)
    {
        targetActor = currentActor ?? frameActor;
        if (_targetPhase is ZLinkActorTargetHandoffPhase.Failed
            or ZLinkActorTargetHandoffPhase.Quarantined)
            return ZLinkActorFrameRoute.Stale;
        if (_messageFollowRoute is { } messageFollowRoute)
        {
            if (!messageFollowRoute.Lease.IsActive)
            {
                //  Spec 30 §11: S4 route removal and the retransmission
                //  window are independent source-local deadlines. The
                //  lease going inactive only means S4 (the route is no
                //  longer usable for follow-routing) — it does not mean
                //  the retransmission window has also elapsed. Stop
                //  routing through it here, but leave the SafeToShutdown
                //  obligation to RemoveMessageFollowRouteAfterDelayAsync,
                //  which releases it only after both deadlines pass.
                _messageFollowRoute = null;
                messageFollowRoute.Lease.Cancel();
            }
            else if (messageFollowRoute.SourceActor.NodeRid == frameActor.NodeRid
                     && messageFollowRoute.SourceActor.Generation == frameActor.Generation)
            {
                targetActor = messageFollowRoute.TargetActor;
                return ZLinkActorFrameRoute.MessageFollow;
            }
        }

        if (_staleSourceActor is { } stale
            && stale.NodeRid == frameActor.NodeRid
            && stale.Generation == frameActor.Generation)
            return _sourcePhase == ZLinkActorSourceHandoffPhase.MessageFollowCommitted
                ? ZLinkActorFrameRoute.MessageFollowExpired
                : ZLinkActorFrameRoute.Stale;

        if (targetActor.NodeRid == frameActor.NodeRid
            && targetActor.Generation == frameActor.Generation)
            return ZLinkActorFrameRoute.Current;

        return ZLinkActorFrameRoute.Stale;
    }

    private async Task RemoveMessageFollowRouteAfterDelayAsync(
        TimeSpan duration,
        TimeSpan retransmissionWindow,
        long committedAt,
        CancellationTokenSource expiry)
    {
        try
        {
            //  S4 (spec 25 §5/30 §11): the Message Follow route becomes
            //  removable this long after S1. route_convergence (S1→S4) is
            //  recorded as its own event right here — it must not wait for
            //  the retransmission-window tail below, or a deployment that
            //  configures a longer window than follow duration would
            //  inflate the metric past S4.
            await Task.Delay(duration, timeProvider, expiry.Token).ConfigureAwait(false);
            ZLinkRuntimeMetrics.RecordRelocationRouteConvergence(
                timeProvider.GetElapsedTime(committedAt), "actor");
            //  SafeToShutdown additionally requires the cutover
            //  retransmission window to have ended (spec 30 §11). Both
            //  waits share the same S1 clock, so this only adds the
            //  remainder when the window outlives the follow duration.
            var remainder = retransmissionWindow - duration;
            if (remainder > TimeSpan.Zero)
                await Task.Delay(remainder, timeProvider, expiry.Token)
                    .ConfigureAwait(false);
            lock (_gate)
            {
                if (!ReferenceEquals(_messageFollowExpiry, expiry)) return;

                ClearMessageFollowRouteLocked();
                diagnostic?.Invoke("message_follow_route_removed entries=0");
            }
        }
        catch (OperationCanceledException) when (expiry.IsCancellationRequested)
        {
            //  Shutdown called before S4 published (spec 30 §11 allows
            //  this): the caller that canceled `expiry` already released
            //  the SafeToShutdown obligation via ClearMessageFollowRouteLocked.
            //  The measurement window never elapsed, so no metric sample.
        }
        finally
        {
            expiry.Dispose();
        }
    }

    private void ClearMessageFollowRouteLocked()
    {
        _messageFollowRoute?.Lease.Cancel();
        _messageFollowRoute = null;
        var expiry = _messageFollowExpiry;
        _messageFollowExpiry = null;
        expiry?.Cancel();
        //  Spec 30 §11: this is the single point every source exit path —
        //  normal S4+window completion, abort, and reset — funnels
        //  through, so the SafeToShutdown obligation acquired at seal
        //  (BeginPendingRelocationUnit) always releases exactly once, no
        //  matter how the unit ends. The setter disposes the token it
        //  replaces.
        PendingShutdownToken = null;
    }
}

internal readonly record struct ZLinkActorMessageFollowRoute(
    ZLinkBackendActorRef SourceActor,
    ZLinkBackendActorRef TargetActor,
    string TargetMeshName,
    ulong SourceNodeGeneration,
    ulong TargetNodeGeneration,
    ulong SourceAuthorityOwnerGeneration,
    ulong TargetAuthorityOwnerGeneration,
    ulong SourceOwnerLeaseGeneration,
    ulong TargetOwnerLeaseGeneration,
    ZLinkActorMessageFollowLease Lease);

internal sealed class ZLinkActorMessageFollowLease(TimeProvider timeProvider)
{
    private readonly object _gate = new();
    private readonly ZLinkMessageFollowSuppressionRegistry _suppression = new(capacity: 1);
    private ZLinkActorMessageFollowLeasePhase _phase;
    private long _committedAt;
    private TimeSpan _duration;

    public bool IsActive
    {
        get
        {
            lock (_gate)
            {
                return _phase switch
                {
                    ZLinkActorMessageFollowLeasePhase.Pending => true,
                    ZLinkActorMessageFollowLeasePhase.Committed =>
                        timeProvider.GetElapsedTime(_committedAt) < _duration,
                    _ => false
                };
            }
        }
    }

    public bool IsCommitted
    {
        get
        {
            lock (_gate)
                return _phase == ZLinkActorMessageFollowLeasePhase.Committed
                       && timeProvider.GetElapsedTime(_committedAt) < _duration;
        }
    }

    public void Commit(TimeSpan duration)
    {
        lock (_gate)
        {
            if (_phase != ZLinkActorMessageFollowLeasePhase.Pending)
                throw new InvalidOperationException("Actor Message Follow lease is not pending.");
            _committedAt = timeProvider.GetTimestamp();
            _duration = duration;
            _phase = ZLinkActorMessageFollowLeasePhase.Committed;
        }
    }

    public void Cancel()
    {
        lock (_gate)
        {
            _phase = ZLinkActorMessageFollowLeasePhase.Cancelled;
            _suppression.ExpireAll();
        }
    }

    internal bool TryBeginMessageFollowNotice(ZLinkMessageFollowFence fence)
    {
        lock (_gate)
        {
            return _phase == ZLinkActorMessageFollowLeasePhase.Committed
                   && timeProvider.GetElapsedTime(_committedAt) < _duration
                   && _suppression.TryBegin(fence);
        }
    }

    internal void MarkMessageFollowNoticeSent(ZLinkMessageFollowFence fence) =>
        _suppression.MarkSent(fence);

    internal void AbortMessageFollowNotice(ZLinkMessageFollowFence fence) =>
        _suppression.Abort(fence);
}

internal enum ZLinkActorMessageFollowLeasePhase
{
    Pending,
    Committed,
    Cancelled
}

internal enum ZLinkActorSourceHandoffPhase
{
    Idle,
    Capturing,
    CutoverPending,
    AbortRestoring,
    MessageFollowCommitted,
    Retired
}

internal enum ZLinkActorTargetHandoffPhase
{
    Idle,
    Importing,
    AuthorityCommitted,
    NotifyingJoined,
    Prepared,
    Replaying,
    AdmissionOpenDraining,
    Failed,
    Completed,
    Quarantined,
    RolledBack
}
