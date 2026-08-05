namespace Zlink.Framework.Runtime.Actors;

internal readonly record struct ZLinkActorHandoffDrainSnapshot(
    long Epoch,
    bool IsSafe);

internal readonly record struct ZLinkActorHandoffAdmissionDecision(
    ZLinkRemoteActorAdmissionReply Reply,
    ZLinkRelocationPermitPool.ZLinkRelocationPermitLease Reservation,
    ZLinkRelocationCapacityFence? CapacityFence = null);

internal sealed class ZLinkActorHandoffAdmissions(
    TimeProvider? timeProvider = null,
    Action<string>? diagnostic = null,
    Func<ZLinkRelocationCapacityFence, CancellationToken,
        ValueTask<ZLinkRelocationCapacityAbortResult>>?
        abortCapacityReservation = null)
{
    private const int TerminalCapacity = 1024;
    private static readonly TimeSpan CleanupAttemptTimeout =
        TimeSpan.FromSeconds(2);
    private static readonly TimeSpan CleanupTransitionTimeout =
        TimeSpan.FromSeconds(5);
    private static readonly TimeSpan CleanupRecoveryDelay =
        TimeSpan.FromMilliseconds(250);
    private readonly object _gate = new();
    private readonly Dictionary<string, PendingAdmission> _pending = new(StringComparer.Ordinal);
    private readonly Dictionary<string, AdmissionExecution> _admitting = new(StringComparer.Ordinal);
    private readonly Dictionary<string, TerminalOutcome> _terminal = new(StringComparer.Ordinal);
    private readonly Queue<string> _terminalOrder = new();
    private readonly TimeProvider _timeProvider = timeProvider ?? TimeProvider.System;
    private CancellationTokenSource _generationStop = new();
    private TaskCompletionSource _drainSafe = CompletedSignal();
    private bool _drainUnsafe;
    private long _drainEpoch;

    internal ZLinkActorHandoffDrainSnapshot SnapshotDrain()
    {
        lock (_gate)
            return new(
                _drainEpoch,
                !_drainUnsafe && _admitting.Count == 0 && _pending.Count == 0);
    }

    public Task WaitUntilDrainSafeAsync(CancellationToken cancellationToken)
    {
        Task wait;
        lock (_gate) wait = _drainSafe.Task;
        return wait.WaitAsync(cancellationToken);
    }

    public async ValueTask<ZLinkRemoteActorAdmissionReply> AdmitAsync(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        Func<CancellationToken, ValueTask<ZLinkRemoteActorAdmissionReply>> admit,
        CancellationToken cancellationToken)
    {
        return await AdmitReservedAsync(
                request,
                targetSpotId,
                async token => new ZLinkActorHandoffAdmissionDecision(
                    await admit(token).ConfigureAwait(false),
                    default),
                cancellationToken)
            .ConfigureAwait(false);
    }

    public async ValueTask<ZLinkRemoteActorAdmissionReply> AdmitReservedAsync(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        Func<CancellationToken, ValueTask<ZLinkActorHandoffAdmissionDecision>> admit,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(admit);
        AdmissionExecution execution;
        var ownsExecution = false;
        lock (_gate)
        {
            if (_pending.TryGetValue(request.HandoffId, out var pending))
            {
                if (pending.Deadline <= _timeProvider.GetUtcNow())
                {
                    if (pending.HasDurableCapacityReservation)
                        throw new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.DeadlineExceeded,
                            $"Actor '{request.ActorId}' handoff admission '{request.HandoffId}' is awaiting durable expiry cleanup.");
                    RemovePendingWithoutCapacityLocked(
                        request.HandoffId,
                        pending);
                }
                else
                {
                    if (!pending.Matches(request, targetSpotId))
                        throw new InvalidOperationException(
                            $"Handoff admission '{request.HandoffId}' was retried with different request data.");
                    return pending.Reply;
                }
            }

            if (_admitting.TryGetValue(request.HandoffId, out execution!))
            {
                if (!execution.Matches(request, targetSpotId))
                    throw new InvalidOperationException(
                        $"Handoff admission '{request.HandoffId}' is already assigned to another request.");
            }
            else
            {
                execution = new AdmissionExecution(request, targetSpotId);
                _admitting.Add(request.HandoffId, execution);
                MarkDrainUnsafeLocked();
                ownsExecution = true;
            }
        }

        if (!ownsExecution)
            return await execution.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

        try
        {
            EnsureDeadlineAndCancellation(request, cancellationToken);
            var decision = await admit(cancellationToken).ConfigureAwait(false);
            await RegisterReservedAsync(
                    request,
                    targetSpotId,
                    decision,
                    cancellationToken)
                .ConfigureAwait(false);
            execution.Complete(decision.Reply);
            return decision.Reply;
        }
        catch (Exception exception)
        {
            execution.Fail(exception);
            throw;
        }
        finally
        {
            lock (_gate)
            {
                if (_admitting.TryGetValue(request.HandoffId, out var current)
                    && ReferenceEquals(current, execution))
                    _admitting.Remove(request.HandoffId);
                TryCompleteDrainSafeLocked();
            }
        }
    }

    public bool TryGetReply(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        out ZLinkRemoteActorAdmissionReply reply)
    {
        lock (_gate)
        {
            if (_pending.TryGetValue(request.HandoffId, out var pending))
            {
                if (pending.Deadline <= _timeProvider.GetUtcNow())
                {
                    if (!pending.HasDurableCapacityReservation)
                        RemovePendingWithoutCapacityLocked(
                            request.HandoffId,
                            pending);
                    reply = null!;
                    return false;
                }
                else if (pending.Matches(request, targetSpotId))
                {
                    reply = pending.Reply;
                    return true;
                }
            }
        }

        reply = null!;
        return false;
    }

    public void Register(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        ZLinkRemoteActorAdmissionReply reply)
    {
        RegisterReserved(
            request,
            targetSpotId,
            new ZLinkActorHandoffAdmissionDecision(reply, default));
    }

    public void RegisterRecoveredReservation(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        DateTimeOffset deadline,
        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease reservation)
    {
        var admission = new ZLinkRemoteActorAdmissionRequest(
            request.ActorId,
            request.ActorType,
            request.SourceSpotId,
            request.SourceNodeRid,
            request.RequestContentType,
            request.Request,
            request.HandoffId,
            deadline.ToUnixTimeMilliseconds(),
            request.ActorGeneration,
            request.ActorAuthorityOwnerGeneration,
            request.ReservedPayloadBytes,
            request.TargetSpotGeneration,
            request.TargetSpotAuthorityOwnerGeneration);
        var reply = new ZLinkRemoteActorAdmissionReply(
            true,
            ZLinkEnvelopeCodec.DefaultContentType,
            [],
            deadline.ToUnixTimeMilliseconds(),
            request.ReservationToken,
            request.ReservedPayloadBytes,
            request.TargetNodeRid,
            request.TargetNodeGeneration,
            request.TargetSpotGeneration,
            request.TargetAuthorityOwnerGeneration,
            request.TargetSpotAuthorityOwnerGeneration);
        RegisterReserved(
            admission,
            targetSpotId,
            new ZLinkActorHandoffAdmissionDecision(reply, reservation));
    }

    private void RegisterReserved(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        ZLinkActorHandoffAdmissionDecision decision)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(request.HandoffId))
                throw new InvalidOperationException("Remote actor admission requires a handoff id.");

            var deadline = DateTimeOffset.FromUnixTimeMilliseconds(request.DeadlineUnixTimeMilliseconds);
            if (deadline <= _timeProvider.GetUtcNow())
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    $"Actor '{request.ActorId}' handoff admission deadline has expired.");

            var pending = new PendingAdmission(
                request,
                targetSpotId,
                deadline,
                decision.Reply,
                decision.Reservation,
                decision.CapacityFence,
                abortCapacityReservation);
            lock (_gate)
            {
                if (_pending.TryGetValue(request.HandoffId, out var existing))
                {
                    if (existing.Deadline <= _timeProvider.GetUtcNow())
                    {
                        if (existing.HasDurableCapacityReservation)
                            throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.DeadlineExceeded,
                                $"Actor '{request.ActorId}' handoff admission '{request.HandoffId}' is awaiting durable expiry cleanup.");
                        RemovePendingWithoutCapacityLocked(
                            request.HandoffId,
                            existing);
                    }
                    else
                    {
                        if (!existing.Matches(request, targetSpotId))
                            throw new InvalidOperationException(
                                $"Handoff admission '{request.HandoffId}' is already assigned to another actor.");
                        decision.Reservation.Dispose();
                        return;
                    }
                }

                _pending.Add(request.HandoffId, pending);
                if (decision.Reply.Accepted) MarkDrainUnsafeLocked();
            }

            CancellationToken generationToken;
            lock (_gate) generationToken = _generationStop.Token;
            pending.SetExpirationTask(
                ExpireAsync(
                    request.HandoffId,
                    pending,
                    generationToken));
        }
        catch
        {
            decision.Reservation.Dispose();
            throw;
        }
    }

    private async ValueTask RegisterReservedAsync(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        ZLinkActorHandoffAdmissionDecision decision,
        CancellationToken cancellationToken)
    {
        if (decision.CapacityFence is null)
        {
            RegisterReserved(request, targetSpotId, decision);
            return;
        }

        if (string.IsNullOrWhiteSpace(request.HandoffId))
            throw new InvalidOperationException(
                "Remote actor admission requires a handoff id.");
        var deadline = DateTimeOffset.FromUnixTimeMilliseconds(
            request.DeadlineUnixTimeMilliseconds);
        var pending = new PendingAdmission(
            request,
            targetSpotId,
            deadline,
            decision.Reply,
            decision.Reservation,
            decision.CapacityFence,
            abortCapacityReservation);
        var registered = false;
        try
        {
            lock (_gate)
            {
                if (_pending.TryGetValue(
                        request.HandoffId,
                        out var existing))
                {
                    if (!existing.Matches(request, targetSpotId))
                        throw new InvalidOperationException(
                            $"Handoff admission '{request.HandoffId}' is already assigned to another actor.");
                    decision.Reservation.Dispose();
                    return;
                }
                _pending.Add(request.HandoffId, pending);
                registered = true;
                MarkDrainUnsafeLocked();
            }

            CancellationToken generationToken;
            lock (_gate) generationToken = _generationStop.Token;
            pending.SetExpirationTask(
                ExpireAsync(
                    request.HandoffId,
                    pending,
                    generationToken));

            if (deadline <= _timeProvider.GetUtcNow())
            {
                using var cleanupDeadline =
                    new CancellationTokenSource(
                        CleanupTransitionTimeout);
                try
                {
                    await AbortAsync(
                            request.HandoffId,
                            cleanupDeadline.Token)
                        .ConfigureAwait(false);
                }
                catch
                {
                    // The registered expiration task remains the retry owner.
                }
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    $"Actor '{request.ActorId}' handoff admission deadline has expired.");
            }
        }
        catch
        {
            if (!registered)
            {
                try
                {
                    using var cleanupDeadline =
                        new CancellationTokenSource(
                            CleanupTransitionTimeout);
                    await pending.AbortCapacityAsync(
                            cleanupDeadline.Token)
                        .ConfigureAwait(false);
                }
                finally
                {
                    pending.ReleasePermit();
                }
            }
            throw;
        }
    }

    private void EnsureDeadlineAndCancellation(
        ZLinkRemoteActorAdmissionRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (DateTimeOffset.FromUnixTimeMilliseconds(
                request.DeadlineUnixTimeMilliseconds)
            <= _timeProvider.GetUtcNow())
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DeadlineExceeded,
                $"Actor '{request.ActorId}' handoff admission deadline has expired.");
    }

    public async ValueTask OwnAndAbortCapacityAsync(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        ZLinkRelocationCapacityFence capacityFence,
        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease reservation,
        CancellationToken cancellationToken)
    {
        var pending = new PendingAdmission(
            request,
            targetSpotId,
            DateTimeOffset.FromUnixTimeMilliseconds(
                request.DeadlineUnixTimeMilliseconds),
            new ZLinkRemoteActorAdmissionReply(
                false,
                string.Empty,
                [],
                request.DeadlineUnixTimeMilliseconds),
            reservation,
            capacityFence,
            abortCapacityReservation);
        lock (_gate)
        {
            if (_pending.TryGetValue(request.HandoffId, out var existing))
            {
                reservation.Dispose();
                if (!existing.Matches(request, targetSpotId))
                    throw new InvalidOperationException(
                        $"Handoff admission '{request.HandoffId}' is already assigned to another actor.");
            }
            else
            {
                _pending.Add(request.HandoffId, pending);
                MarkDrainUnsafeLocked();
                var generationToken = _generationStop.Token;
                pending.SetExpirationTask(
                    ExpireAsync(
                        request.HandoffId,
                        pending,
                        generationToken));
            }
        }

        using var cleanupDeadline =
            CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        cleanupDeadline.CancelAfter(CleanupTransitionTimeout);
        await AbortAsync(request.HandoffId, cleanupDeadline.Token)
            .ConfigureAwait(false);
    }

    public ZLinkRelocationCapacityFence? BeginCommit(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId) =>
        BeginCommitCore(request, targetSpotId, null);

    public ZLinkRelocationCapacityFence? BeginCommit(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        long actualPayloadBytes) =>
        BeginCommitCore(request, targetSpotId, actualPayloadBytes);

    private ZLinkRelocationCapacityFence? BeginCommitCore(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        long? actualPayloadBytes)
    {
        lock (_gate)
        {
            if (!_pending.TryGetValue(request.HandoffId, out var pending)
                || !pending.Matches(request, targetSpotId))
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' does not have a matching pending handoff admission '{request.HandoffId}'.");
            if (pending.Deadline <= _timeProvider.GetUtcNow())
            {
                if (!pending.HasDurableCapacityReservation)
                    RemovePendingWithoutCapacityLocked(
                        request.HandoffId,
                        pending);
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    $"Actor '{request.ActorId}' handoff admission '{request.HandoffId}' has expired.");
            }

            if (!pending.Reply.Accepted)
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' handoff admission '{request.HandoffId}' was rejected.");

            if (actualPayloadBytes is { } actual)
                pending.ValidateCommit(request, targetSpotId, actual);
            pending.Committing = true;
            return pending.CapacityFence;
        }
    }

    public void Complete(string handoffId)
    {
        PendingAdmission? removed = null;
        lock (_gate)
        {
            if (_pending.Remove(handoffId, out removed))
                removed.MarkCapacityCommitted();
            TryCompleteDrainSafeLocked();
        }
        removed?.ReleasePermit();
    }

    public async ValueTask AbortAsync(
        string handoffId,
        CancellationToken cancellationToken = default)
    {
        PendingAdmission? pending;
        lock (_gate)
            _pending.TryGetValue(handoffId, out pending);
        if (pending is null) return;
        await pending.AbortCapacityAsync(cancellationToken)
            .ConfigureAwait(false);
        lock (_gate)
        {
            if (_pending.TryGetValue(handoffId, out var current)
                && ReferenceEquals(current, pending))
                _pending.Remove(handoffId);
            TryCompleteDrainSafeLocked();
        }
        pending.ReleasePermit();
    }

    public async ValueTask AbortReservationAsync(
        ZLinkRemoteActorAdmissionAbortRequest request,
        string targetSpotId,
        CancellationToken cancellationToken = default)
    {
        PendingAdmission? pending;
        lock (_gate)
        {
            if (!_pending.TryGetValue(request.HandoffId, out pending))
                return;
            if (!pending.MatchesAbort(request, targetSpotId))
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' admission abort does not match reservation '{request.HandoffId}'.");
        }
        await pending.AbortCapacityAsync(cancellationToken)
            .ConfigureAwait(false);
        lock (_gate)
        {
            if (_pending.TryGetValue(request.HandoffId, out var current)
                && ReferenceEquals(current, pending))
                _pending.Remove(request.HandoffId);
            TryCompleteDrainSafeLocked();
        }
        pending.ReleasePermit();
    }

    public bool TryGetJoinOutcome(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        out ZLinkRemoteActorJoinReply reply)
    {
        lock (_gate)
        {
            if (_terminal.TryGetValue(request.HandoffId, out var terminal)
                && terminal.Matches(request, targetSpotId))
            {
                reply = terminal.Reply;
                return true;
            }
        }

        reply = null!;
        return false;
    }

    public void RecordJoinOutcome(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        ZLinkRemoteActorJoinReply reply,
        TimeSpan? preparedCompletionTimeout = null)
    {
        lock (_gate)
        {
            if (_terminal.TryGetValue(request.HandoffId, out var existing))
            {
                if (!existing.Matches(request, targetSpotId))
                    throw new InvalidOperationException(
                        $"Handoff outcome '{request.HandoffId}' is already assigned to another transaction.");
                return;
            }

            _terminal.Add(
                request.HandoffId,
                new TerminalOutcome(
                    request,
                    targetSpotId,
                    reply,
                    reply.Accepted && preparedCompletionTimeout is { } timeout
                        ? _timeProvider.GetUtcNow() + timeout
                        : null));
            _terminalOrder.Enqueue(request.HandoffId);
            TrimTerminalOutcomesLocked();
        }
    }

    public void RejectPreparedJoinOutcome(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        ZLinkRemoteActorJoinReply rejectedReply)
    {
        if (rejectedReply.Accepted)
            throw new ArgumentException("A compensated handoff outcome must be rejected.", nameof(rejectedReply));

        lock (_gate)
        {
            if (!_terminal.TryGetValue(request.HandoffId, out var terminal))
            {
                _terminal.Add(
                    request.HandoffId,
                    new TerminalOutcome(request, targetSpotId, rejectedReply, null));
                _terminalOrder.Enqueue(request.HandoffId);
                TrimTerminalOutcomesLocked();
                return;
            }

            if (!terminal.Matches(request, targetSpotId))
                throw new InvalidOperationException(
                    $"Handoff outcome '{request.HandoffId}' is already assigned to another transaction.");
            if (terminal.Phase != ZLinkActorCommitPhase.Prepared)
                return;

            terminal.Reply = rejectedReply;
            terminal.Phase = ZLinkActorCommitPhase.Rejected;
            TrimTerminalOutcomesLocked();
        }
    }

    public bool TryBeginCompletion(
        ZLinkRemoteActorHandoffCompletionRequest request,
        string targetSpotId)
    {
        lock (_gate)
        {
            var terminal = ValidateCompletionLocked(request, targetSpotId);
            switch (terminal.Phase)
            {
                case ZLinkActorCommitPhase.Prepared:
                    if (terminal.Completion is null)
                        terminal.Completion = request;
                    else if (!terminal.Matches(request, targetSpotId))
                        throw new InvalidOperationException(
                            $"Actor '{request.ActorId}' handoff completion '{request.HandoffId}' was retried with different frame data.");
                    terminal.Phase = ZLinkActorCommitPhase.Completing;
                    return true;
                case ZLinkActorCommitPhase.Completed:
                    if (!terminal.Matches(request, targetSpotId))
                        throw new InvalidOperationException(
                            $"Actor '{request.ActorId}' handoff completion '{request.HandoffId}' was retried with different frame data.");
                    return false;
                case ZLinkActorCommitPhase.Completing:
                    throw new InvalidOperationException(
                        $"Actor '{request.ActorId}' handoff completion is already in progress.");
                default:
                    throw new ZLinkActorHandoffRejectedException(
                        $"Actor '{request.ActorId}' handoff completion is no longer accepted.");
            }
        }
    }

    public void CancelCompletion(
        ZLinkRemoteActorHandoffCompletionRequest request,
        string targetSpotId)
    {
        lock (_gate)
        {
            var terminal = ValidateCompletionLocked(request, targetSpotId);
            if (terminal.Phase == ZLinkActorCommitPhase.Completing)
                terminal.Phase = ZLinkActorCommitPhase.Prepared;
        }
    }

    public bool TryExpirePreparedCommit(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        ZLinkRemoteActorJoinReply rejectedReply)
    {
        lock (_gate)
        {
            if (!_terminal.TryGetValue(request.HandoffId, out var terminal)
                || !terminal.Matches(request, targetSpotId)
                || terminal.Phase != ZLinkActorCommitPhase.Prepared
                || terminal.PreparedCompletionDeadline is not { } deadline
                || deadline > _timeProvider.GetUtcNow())
                return false;

            terminal.Reply = rejectedReply;
            terminal.Phase = ZLinkActorCommitPhase.Expired;
            TrimTerminalOutcomesLocked();
            return true;
        }
    }

    public bool IsPreparedCommitPending(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId)
    {
        lock (_gate)
        {
            return _terminal.TryGetValue(request.HandoffId, out var terminal)
                   && terminal.Matches(request, targetSpotId)
                   && terminal.Reply.Accepted
                   && terminal.Phase is ZLinkActorCommitPhase.Prepared
                       or ZLinkActorCommitPhase.Completing;
        }
    }

    public void RecordCompletion(
        ZLinkRemoteActorHandoffCompletionRequest request,
        string targetSpotId)
    {
        lock (_gate)
        {
            if (!_terminal.TryGetValue(request.HandoffId, out var terminal)
                || !string.Equals(terminal.Request.ActorId, request.ActorId, StringComparison.Ordinal))
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' does not have a terminal handoff '{request.HandoffId}'.");
            if (!terminal.Matches(request, targetSpotId, requireRecordedCompletion: false)
                || terminal.Completion is not null && !terminal.Matches(request, targetSpotId))
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' handoff completion '{request.HandoffId}' conflicts with its terminal result.");
            if (terminal.Phase != ZLinkActorCommitPhase.Completing)
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' handoff completion is not active.");
            terminal.Phase = ZLinkActorCommitPhase.Completed;
            TrimTerminalOutcomesLocked();
        }
    }

    private void TrimTerminalOutcomesLocked()
    {
        var candidates = _terminalOrder.Count;
        while (_terminal.Count > TerminalCapacity && candidates-- > 0)
        {
            var handoffId = _terminalOrder.Dequeue();
            if (!_terminal.TryGetValue(handoffId, out var terminal)) continue;
            if (terminal.Phase is ZLinkActorCommitPhase.Completed or ZLinkActorCommitPhase.Expired
                || !terminal.Reply.Accepted)
                _terminal.Remove(handoffId);
            else
                _terminalOrder.Enqueue(handoffId);
        }
    }

    private TerminalOutcome ValidateCompletionLocked(
        ZLinkRemoteActorHandoffCompletionRequest request,
        string targetSpotId)
    {
        if (!_terminal.TryGetValue(request.HandoffId, out var terminal)
            || !terminal.Reply.Accepted
            || !terminal.Matches(request, targetSpotId, requireRecordedCompletion: false))
            // Terminal for the source's completion reconciliation: this
            // target no longer honors the handoff (expired or replaced), so
            // retrying the completion can never succeed.
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                $"Actor '{request.ActorId}' does not have a matching accepted handoff '{request.HandoffId}'.");
        return terminal;
    }

    public async ValueTask ResetGenerationAsync(
        CancellationToken cancellationToken = default)
    {
        CancellationTokenSource stopped;
        AdmissionExecution[] admitting;
        PendingAdmission[] pending;
        lock (_gate)
        {
            stopped = _generationStop;
            _generationStop = new CancellationTokenSource();
            admitting = _admitting.Values.ToArray();
            _admitting.Clear();
            pending = _pending.Values.ToArray();
            _terminal.Clear();
            _terminalOrder.Clear();
        }

        stopped.Cancel();
        stopped.Dispose();
        var expirationFailures =
            new Dictionary<PendingAdmission, Exception>();
        foreach (var admission in pending)
        {
            try
            {
                await admission.WaitForExpirationOwnerAsync()
                    .WaitAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                // Generation cancellation hands cleanup to this shutdown path.
            }
            catch (Exception exception)
            {
                expirationFailures[admission] = exception;
            }
        }
        var cleanupFailures = new List<Exception>();
        foreach (var admission in pending)
        {
            try
            {
                await admission.AbortCapacityAsync(cancellationToken)
                    .ConfigureAwait(false);
                admission.ReleasePermit();
                lock (_gate)
                {
                    if (_pending.TryGetValue(
                            admission.HandoffId,
                            out var current)
                        && ReferenceEquals(current, admission))
                        _pending.Remove(admission.HandoffId);
                }
            }
            catch (Exception cleanupFailure)
            {
                cleanupFailures.Add(
                    expirationFailures.TryGetValue(
                        admission,
                        out var expirationFailure)
                        ? new AggregateException(
                            expirationFailure,
                            cleanupFailure)
                        : cleanupFailure);
                CancellationToken successorToken;
                lock (_gate) successorToken = _generationStop.Token;
                admission.SetExpirationTask(
                    ExpireAsync(
                        admission.HandoffId,
                        admission,
                        successorToken));
            }
        }
        lock (_gate) TryCompleteDrainSafeLocked();
        var failure = new InvalidOperationException(
            "Actor handoff admission belongs to a stopped framework runtime generation.");
        foreach (var execution in admitting) execution.Fail(failure);
        if (cleanupFailures.Count > 0)
            throw new AggregateException(
                "Actor handoff cleanup did not reach a terminal Store state before the shutdown deadline.",
                cleanupFailures);
    }

    private async Task ExpireAsync(
        string handoffId,
        PendingAdmission pending,
        CancellationToken cancellationToken)
    {
        var shouldExpire = false;
        try
        {
            var delay = pending.Deadline - _timeProvider.GetUtcNow();
            if (delay > TimeSpan.Zero)
                await Task.Delay(delay, _timeProvider, cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return;
        }

        lock (_gate)
        {
            if (_pending.TryGetValue(handoffId, out var current)
                && ReferenceEquals(current, pending)
                && !current.Committing)
            {
                shouldExpire = true;
            }
        }
        if (shouldExpire)
        {
            while (true)
            {
                try
                {
                    await pending.AbortCapacityAsync(cancellationToken)
                        .ConfigureAwait(false);
                    break;
                }
                catch (OperationCanceledException)
                    when (cancellationToken.IsCancellationRequested)
                {
                    return;
                }
                catch
                {
                    await Task.Delay(
                            CleanupRecoveryDelay,
                            _timeProvider,
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }
            lock (_gate)
            {
                if (_pending.TryGetValue(handoffId, out var current)
                    && ReferenceEquals(current, pending)
                    && !current.Committing)
                    _pending.Remove(handoffId);
                TryCompleteDrainSafeLocked();
            }
            pending.ReleasePermit();
            diagnostic?.Invoke(
                $"pending_admission_expired actor={pending.ActorId} handoff_id={handoffId}");
        }
    }

    private void MarkDrainUnsafeLocked()
    {
        _drainUnsafe = true;
        _drainEpoch++;
        if (_drainSafe.Task.IsCompleted)
            _drainSafe = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private void TryCompleteDrainSafeLocked()
    {
        if (_admitting.Count == 0
            && _pending.Count == 0
            && _drainUnsafe)
        {
            _drainUnsafe = false;
            _drainEpoch++;
            _drainSafe.TrySetResult();
        }
    }

    private void RemovePendingWithoutCapacityLocked(
        string handoffId,
        PendingAdmission pending)
    {
        if (pending.HasDurableCapacityReservation)
            throw new InvalidOperationException(
                "Durable capacity cleanup requires an asynchronous terminal transition.");
        if (_pending.TryGetValue(handoffId, out var current)
            && ReferenceEquals(current, pending))
        {
            _pending.Remove(handoffId);
            pending.ReleasePermit();
            TryCompleteDrainSafeLocked();
        }
    }

    private static TaskCompletionSource CompletedSignal()
    {
        var signal = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        signal.SetResult();
        return signal;
    }

    private sealed class PendingAdmission(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        DateTimeOffset deadline,
        ZLinkRemoteActorAdmissionReply reply,
        ZLinkRelocationPermitPool.ZLinkRelocationPermitLease reservation,
        ZLinkRelocationCapacityFence? capacityFence,
        Func<ZLinkRelocationCapacityFence, CancellationToken,
            ValueTask<ZLinkRelocationCapacityAbortResult>>?
            abortCapacityReservation)
    {
        public string ActorId { get; } = request.ActorId;
        public string HandoffId { get; } = request.HandoffId;

        public DateTimeOffset Deadline { get; } = deadline;

        public ZLinkRemoteActorAdmissionReply Reply { get; } = reply;

        public bool Committing { get; set; }
        public ZLinkRelocationCapacityFence? CapacityFence { get; } =
            capacityFence;
        public bool HasDurableCapacityReservation =>
            CapacityFence is not null;

        public void ValidateCommit(
            ZLinkRemoteActorJoinRequest candidate,
            string candidateTargetSpotId,
            long actualPayloadBytes)
        {
            if (!Matches(candidate, candidateTargetSpotId)
                || request.ActorGeneration != candidate.ActorGeneration
                || request.ActorAuthorityOwnerGeneration
                   != candidate.ActorAuthorityOwnerGeneration
                || request.PredictedPayloadBytes
                   != candidate.ReservedPayloadBytes
                || string.IsNullOrEmpty(Reply.ReservationToken)
                || !string.Equals(
                    Reply.ReservationToken,
                    candidate.ReservationToken,
                    StringComparison.Ordinal)
                || Reply.ReservedPayloadBytes != candidate.ReservedPayloadBytes
                || Reply.TargetNodeGeneration != candidate.TargetNodeGeneration
                || Reply.TargetSpotGeneration != candidate.TargetSpotGeneration
                || Reply.TargetAuthorityOwnerGeneration
                   != candidate.TargetAuthorityOwnerGeneration
                || Reply.TargetSpotAuthorityOwnerGeneration
                   != candidate.TargetSpotAuthorityOwnerGeneration
                || Reply.TargetNodeRid is null
                || candidate.TargetNodeRid is null
                || !Reply.TargetNodeRid.AsSpan().SequenceEqual(
                    candidate.TargetNodeRid))
                throw new InvalidOperationException(
                    $"Actor '{candidate.ActorId}' handoff commit does not match its target reservation.");
            if (!reservation.TryShrinkPayload(actualPayloadBytes))
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.Rejected,
                    $"Actor '{candidate.ActorId}' relocation payload exceeded its target reservation.");
        }

        private int _permitReleased;
        private int _capacityCommitted;
        private Task _expirationTask = Task.CompletedTask;

        public void SetExpirationTask(Task expirationTask) =>
            _expirationTask = expirationTask;

        public Task WaitForExpirationOwnerAsync() => _expirationTask;

        public void MarkCapacityCommitted() =>
            Volatile.Write(ref _capacityCommitted, 1);

        public void ReleasePermit()
        {
            if (Interlocked.Exchange(ref _permitReleased, 1) == 0)
                reservation.Dispose();
        }

        public async ValueTask AbortCapacityAsync(
            CancellationToken cancellationToken)
        {
            if (Volatile.Read(ref _capacityCommitted) != 0
                || CapacityFence is not { } fence
                || abortCapacityReservation is null)
                return;
            Exception? lastFailure = null;
            for (var attempt = 0; attempt < 3; attempt++)
            {
                cancellationToken.ThrowIfCancellationRequested();
                try
                {
                    using var attemptDeadline =
                        CancellationTokenSource.CreateLinkedTokenSource(
                            cancellationToken);
                    attemptDeadline.CancelAfter(CleanupAttemptTimeout);
                    var result = await abortCapacityReservation(
                            fence,
                            attemptDeadline.Token)
                        .AsTask()
                        .WaitAsync(attemptDeadline.Token)
                        .ConfigureAwait(false);
                    if (result is ZLinkRelocationCapacityAbortResult.Aborted
                        or ZLinkRelocationCapacityAbortResult.AlreadyAborted
                        or ZLinkRelocationCapacityAbortResult.AlreadyCommitted)
                        return;
                    lastFailure = new InvalidOperationException(
                        $"Capacity reservation '{fence.Value}' is stale and cannot be released yet.");
                }
                catch (Exception exception)
                    when (exception is not OperationCanceledException)
                {
                    lastFailure = exception;
                }
                if (attempt < 2)
                    await Task.Delay(
                            TimeSpan.FromMilliseconds(25 * (attempt + 1)),
                            cancellationToken)
                        .ConfigureAwait(false);
            }
            throw new InvalidOperationException(
                $"Capacity reservation '{fence.Value}' cleanup did not reach a terminal Store state.",
                lastFailure);
        }

        public bool MatchesAbort(
            ZLinkRemoteActorAdmissionAbortRequest candidate,
            string candidateTargetSpotId) =>
            string.Equals(request.ActorId, candidate.ActorId, StringComparison.Ordinal)
            && string.Equals(request.HandoffId, candidate.HandoffId, StringComparison.Ordinal)
            && string.Equals(
                Reply.ReservationToken,
                candidate.ReservationToken,
                StringComparison.Ordinal)
            && targetSpotId == candidateTargetSpotId;

        public bool Matches(ZLinkRemoteActorAdmissionRequest candidate, string candidateTargetSpotId)
            => string.Equals(request.ActorId, candidate.ActorId, StringComparison.Ordinal)
               && string.Equals(request.ActorType, candidate.ActorType, StringComparison.Ordinal)
               && string.Equals(request.HandoffId, candidate.HandoffId, StringComparison.Ordinal)
               && request.DeadlineUnixTimeMilliseconds == candidate.DeadlineUnixTimeMilliseconds
               && request.SourceNodeRid.AsSpan().SequenceEqual(candidate.SourceNodeRid)
               && string.Equals(request.SourceSpotId, candidate.SourceSpotId, StringComparison.Ordinal)
                && string.Equals(request.RequestContentType, candidate.RequestContentType, StringComparison.Ordinal)
                && request.Request.AsSpan().SequenceEqual(candidate.Request)
                && targetSpotId == candidateTargetSpotId;

        public bool Matches(ZLinkRemoteActorJoinRequest candidate, string candidateTargetSpotId)
            => string.Equals(request.ActorId, candidate.ActorId, StringComparison.Ordinal)
               && string.Equals(request.ActorType, candidate.ActorType, StringComparison.Ordinal)
               && string.Equals(request.HandoffId, candidate.HandoffId, StringComparison.Ordinal)
               && request.SourceNodeRid.AsSpan().SequenceEqual(candidate.SourceNodeRid)
               && string.Equals(request.SourceSpotId, candidate.SourceSpotId, StringComparison.Ordinal)
               && string.Equals(request.RequestContentType, candidate.RequestContentType, StringComparison.Ordinal)
               && request.Request.AsSpan().SequenceEqual(candidate.Request)
               && targetSpotId == candidateTargetSpotId;
    }

    private sealed class AdmissionExecution(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId)
    {
        private readonly TaskCompletionSource<ZLinkRemoteActorAdmissionReply> _result = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        public Task<ZLinkRemoteActorAdmissionReply> Task => _result.Task;

        public bool Matches(ZLinkRemoteActorAdmissionRequest candidate, string candidateTargetSpotId)
            => targetSpotId == candidateTargetSpotId
               && string.Equals(request.ActorId, candidate.ActorId, StringComparison.Ordinal)
               && string.Equals(request.ActorType, candidate.ActorType, StringComparison.Ordinal)
               && string.Equals(request.HandoffId, candidate.HandoffId, StringComparison.Ordinal)
               && request.DeadlineUnixTimeMilliseconds == candidate.DeadlineUnixTimeMilliseconds
               && request.SourceNodeRid.AsSpan().SequenceEqual(candidate.SourceNodeRid)
               && string.Equals(request.SourceSpotId, candidate.SourceSpotId, StringComparison.Ordinal)
               && string.Equals(request.RequestContentType, candidate.RequestContentType, StringComparison.Ordinal)
               && request.Request.AsSpan().SequenceEqual(candidate.Request)
               && request.ActorGeneration == candidate.ActorGeneration
               && request.ActorAuthorityOwnerGeneration
                  == candidate.ActorAuthorityOwnerGeneration
               && request.PredictedPayloadBytes == candidate.PredictedPayloadBytes
               && request.TargetSpotGeneration == candidate.TargetSpotGeneration
               && request.TargetSpotAuthorityOwnerGeneration
                  == candidate.TargetSpotAuthorityOwnerGeneration;

        public void Complete(ZLinkRemoteActorAdmissionReply reply) => _result.TrySetResult(reply);

        public void Fail(Exception exception) => _result.TrySetException(exception);
    }

    private sealed class TerminalOutcome(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        ZLinkRemoteActorJoinReply reply,
        DateTimeOffset? preparedCompletionDeadline)
    {
        public ZLinkRemoteActorJoinRequest Request { get; } = request;

        public ZLinkRemoteActorJoinReply Reply { get; set; } = reply;

        public DateTimeOffset? PreparedCompletionDeadline { get; } = preparedCompletionDeadline;

        public ZLinkActorCommitPhase Phase { get; set; } = reply.Accepted
            ? ZLinkActorCommitPhase.Prepared
            : ZLinkActorCommitPhase.Rejected;

        public ZLinkRemoteActorHandoffCompletionRequest? Completion { get; set; }

        public bool Matches(ZLinkRemoteActorJoinRequest candidate, string candidateTargetSpotId)
            => targetSpotId == candidateTargetSpotId
               && ZLinkActorHandoffRequestIdentity.Matches(Request, candidate);

        public bool Matches(
            ZLinkRemoteActorHandoffCompletionRequest candidate,
            string candidateTargetSpotId,
            bool requireRecordedCompletion = true)
            => string.Equals(Request.ActorId, candidate.ActorId, StringComparison.Ordinal)
               && string.Equals(Request.HandoffId, candidate.HandoffId, StringComparison.Ordinal)
               && string.Equals(Request.SourceSpotId, candidate.SourceSpotId, StringComparison.Ordinal)
               && Request.SourceNodeRid.AsSpan().SequenceEqual(candidate.SourceNodeRid)
               && targetSpotId == candidateTargetSpotId
               && string.Equals(candidate.TargetSpotId, candidateTargetSpotId, StringComparison.Ordinal)
               && BoundSessionRouteMatches(Request, candidate)
               && (!requireRecordedCompletion
                   || Completion is not null
                   && Completion.OperationIdHigh == candidate.OperationIdHigh
                   && Completion.OperationIdLow == candidate.OperationIdLow
                   && string.Equals(
                       Completion.ReplyContentType,
                       candidate.ReplyContentType,
                       StringComparison.Ordinal)
                   && (Completion.Reply ?? []).AsSpan().SequenceEqual(candidate.Reply ?? []))
               && (!requireRecordedCompletion
                   || Completion is not null
                   && BoundSessionRouteMatches(Completion, candidate)
                   && ZLinkActorHandoffRequestIdentity.FramesEqual(Completion.Frames, candidate.Frames));

        private static bool BoundSessionRouteMatches(
            ZLinkRemoteActorJoinRequest left,
            ZLinkRemoteActorHandoffCompletionRequest right)
        {
            var leftHasRoute = left.BoundSessionNodeRid is { Length: > 0 }
                               || left.BoundSessionRid is { Length: > 0 }
                               || !string.IsNullOrEmpty(left.BoundSessionBindingToken);
            var rightHasRoute = right.BoundSessionNodeRid is { Length: > 0 }
                                || right.BoundSessionRid is { Length: > 0 }
                                || !string.IsNullOrEmpty(right.BoundSessionBindingToken);
            if (!leftHasRoute && !rightHasRoute) return true;
            if (leftHasRoute != rightHasRoute) return false;

            return (left.BoundSessionNodeRid ?? []).AsSpan()
                .SequenceEqual(right.BoundSessionNodeRid ?? [])
            && (left.BoundSessionRid ?? []).AsSpan()
                .SequenceEqual(right.BoundSessionRid ?? [])
            && string.Equals(
                left.BoundSessionBindingToken,
                right.BoundSessionBindingToken,
                StringComparison.Ordinal)
            && left.BoundSessionBindingGeneration == right.BoundSessionBindingGeneration
            && left.BoundSessionObjectGeneration == right.BoundSessionObjectGeneration
            && left.BoundSessionAuthorityOwnerGeneration
                == right.BoundSessionAuthorityOwnerGeneration
            && string.Equals(
                left.BoundSessionMeshName,
                right.BoundSessionMeshName,
                StringComparison.Ordinal)
            && left.BoundSessionTargetNodeGeneration
                == right.BoundSessionTargetNodeGeneration
            && left.BoundSessionOwnerLeaseGeneration
                == right.BoundSessionOwnerLeaseGeneration
            && left.BoundSessionOwnerNodeGeneration
                == right.BoundSessionOwnerNodeGeneration
            && left.BoundSessionAcceptedHighWater
                == right.BoundSessionAcceptedHighWater;
        }

        private static bool BoundSessionRouteMatches(
            ZLinkRemoteActorHandoffCompletionRequest left,
            ZLinkRemoteActorHandoffCompletionRequest right) =>
            (left.BoundSessionNodeRid ?? []).AsSpan()
                .SequenceEqual(right.BoundSessionNodeRid ?? [])
            && (left.BoundSessionRid ?? []).AsSpan()
                .SequenceEqual(right.BoundSessionRid ?? [])
            && string.Equals(
                left.BoundSessionBindingToken,
                right.BoundSessionBindingToken,
                StringComparison.Ordinal)
            && left.BoundSessionBindingGeneration == right.BoundSessionBindingGeneration
            && left.BoundSessionObjectGeneration == right.BoundSessionObjectGeneration
            && left.BoundSessionAuthorityOwnerGeneration
                == right.BoundSessionAuthorityOwnerGeneration
            && string.Equals(
                left.BoundSessionMeshName,
                right.BoundSessionMeshName,
                StringComparison.Ordinal)
            && left.BoundSessionTargetNodeGeneration
                == right.BoundSessionTargetNodeGeneration
            && left.BoundSessionOwnerLeaseGeneration
                == right.BoundSessionOwnerLeaseGeneration
            && left.BoundSessionOwnerNodeGeneration
                == right.BoundSessionOwnerNodeGeneration
            && left.BoundSessionAcceptedHighWater
                == right.BoundSessionAcceptedHighWater;
    }
}

internal enum ZLinkActorCommitPhase
{
    Prepared,
    Completing,
    Completed,
    Rejected,
    Expired
}

internal static class ZLinkActorHandoffRequestIdentity
{
    public static bool Matches(
        ZLinkRemoteActorJoinRequest left,
        ZLinkRemoteActorJoinRequest right)
    {
        return string.Equals(left.ActorId, right.ActorId, StringComparison.Ordinal)
               && string.Equals(left.ActorType, right.ActorType, StringComparison.Ordinal)
               && string.Equals(left.HandoffId, right.HandoffId, StringComparison.Ordinal)
               && BytesEqual(left.BoundSessionNodeRid, right.BoundSessionNodeRid)
               && BytesEqual(left.BoundSessionRid, right.BoundSessionRid)
               && string.Equals(
                   left.BoundSessionBindingToken,
                   right.BoundSessionBindingToken,
                   StringComparison.Ordinal)
               && left.BoundSessionBindingGeneration
               == right.BoundSessionBindingGeneration
               && left.BoundSessionObjectGeneration
               == right.BoundSessionObjectGeneration
               && left.BoundSessionAuthorityOwnerGeneration
               == right.BoundSessionAuthorityOwnerGeneration
               && string.Equals(
                   left.BoundSessionMeshName,
                   right.BoundSessionMeshName,
                   StringComparison.Ordinal)
               && left.BoundSessionTargetNodeGeneration
               == right.BoundSessionTargetNodeGeneration
               && left.BoundSessionOwnerLeaseGeneration
               == right.BoundSessionOwnerLeaseGeneration
               && left.BoundSessionOwnerNodeGeneration
               == right.BoundSessionOwnerNodeGeneration
               && left.BoundSessionAcceptedHighWater
               == right.BoundSessionAcceptedHighWater
               && string.Equals(left.RelocationContentType, right.RelocationContentType, StringComparison.Ordinal)
               && string.Equals(
                   left.RelocationReference,
                   right.RelocationReference,
                   StringComparison.Ordinal)
               && left.RelocationChecksumCrc32c
               == right.RelocationChecksumCrc32c
               && left.RelocationAggregateId == right.RelocationAggregateId
               && left.RelocationAggregateGeneration
               == right.RelocationAggregateGeneration
               && left.RelocationInventoryDigest.AsSpan().SequenceEqual(
                   right.RelocationInventoryDigest)
               && string.Equals(left.RequestContentType, right.RequestContentType, StringComparison.Ordinal)
               && left.Request.AsSpan().SequenceEqual(right.Request)
               && string.Equals(left.SourceSpotId, right.SourceSpotId, StringComparison.Ordinal)
               && left.SourceNodeRid.AsSpan().SequenceEqual(right.SourceNodeRid)
               && left.ActorGeneration == right.ActorGeneration
               && left.ActorAuthorityOwnerGeneration
               == right.ActorAuthorityOwnerGeneration
               && string.Equals(
                   left.ReservationToken,
                   right.ReservationToken,
                   StringComparison.Ordinal)
               && left.ReservedPayloadBytes == right.ReservedPayloadBytes
               && BytesEqual(left.TargetNodeRid, right.TargetNodeRid)
               && left.TargetNodeGeneration == right.TargetNodeGeneration
               && left.TargetSpotGeneration == right.TargetSpotGeneration
               && left.TargetAuthorityOwnerGeneration
               == right.TargetAuthorityOwnerGeneration
               && left.TargetSpotAuthorityOwnerGeneration
               == right.TargetSpotAuthorityOwnerGeneration
               && FramesEqual(left.HandoffFrames, right.HandoffFrames);
    }

    public static bool FramesEqual(
        IReadOnlyList<ZLinkActorHandoffFrame> left,
        IReadOnlyList<ZLinkActorHandoffFrame> right)
    {
        if (left.Count != right.Count) return false;
        for (var index = 0; index < left.Count; index++)
        {
            var a = left[index];
            var b = right[index];
            if (a.ArrivalIndex != b.ArrivalIndex
                || a.RequestId != b.RequestId
                || a.Flags != b.Flags
                || a.RouteContext != b.RouteContext
                || !a.ReplyActorNodeRid.AsSpan().SequenceEqual(b.ReplyActorNodeRid)
                || a.ReplyActorGeneration != b.ReplyActorGeneration
                || !a.SourceNodeRid.AsSpan().SequenceEqual(b.SourceNodeRid)
                || !a.SourceSessionRid.AsSpan().SequenceEqual(b.SourceSessionRid)
                || !a.Header.AsSpan().SequenceEqual(b.Header)
                || !a.Body.AsSpan().SequenceEqual(b.Body))
                return false;
        }

        return true;
    }

    private static bool BytesEqual(byte[]? left, byte[]? right)
        => left is null ? right is null : right is not null && left.AsSpan().SequenceEqual(right);
}
