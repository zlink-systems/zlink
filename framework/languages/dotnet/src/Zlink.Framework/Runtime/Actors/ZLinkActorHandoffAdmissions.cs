namespace Zlink.Framework.Runtime.Actors;

internal readonly record struct ZLinkActorHandoffDrainSnapshot(
    long Epoch,
    bool IsSafe);

internal sealed class ZLinkActorHandoffAdmissions(
    TimeProvider? timeProvider = null,
    Action<string>? diagnostic = null)
{
    private readonly object _gate = new();
    private readonly Dictionary<string, PendingAdmission> _pending = new(StringComparer.Ordinal);
    private readonly Dictionary<string, AdmissionExecution> _admitting = new(StringComparer.Ordinal);
    private readonly Dictionary<string, TerminalOutcome> _terminal = new(StringComparer.Ordinal);
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
        ArgumentNullException.ThrowIfNull(admit);
        AdmissionExecution execution;
        var ownsExecution = false;
        lock (_gate)
        {
            if (_pending.TryGetValue(request.HandoffId, out var pending))
            {
                if (pending.Deadline <= _timeProvider.GetUtcNow())
                {
                    RemovePendingLocked(
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
            var reply = await admit(cancellationToken).ConfigureAwait(false);
            await RegisterReservedAsync(
                    request,
                    targetSpotId,
                    reply,
                    cancellationToken)
                .ConfigureAwait(false);
            execution.Complete(reply);
            return reply;
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
                    RemovePendingLocked(request.HandoffId, pending);
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
            reply);
    }

    public void RegisterRecoveredReservation(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        DateTimeOffset deadline)
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
            reply);
    }

    private void RegisterReserved(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        ZLinkRemoteActorAdmissionReply reply)
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
            reply);
        lock (_gate)
        {
            if (_pending.TryGetValue(request.HandoffId, out var existing))
            {
                if (existing.Deadline <= _timeProvider.GetUtcNow())
                {
                    RemovePendingLocked(
                        request.HandoffId,
                        existing);
                }
                else
                {
                    if (!existing.Matches(request, targetSpotId))
                        throw new InvalidOperationException(
                            $"Handoff admission '{request.HandoffId}' is already assigned to another actor.");
                    return;
                }
            }

            _pending.Add(request.HandoffId, pending);
            if (reply.Accepted) MarkDrainUnsafeLocked();
        }

        CancellationToken generationToken;
        lock (_gate) generationToken = _generationStop.Token;
        pending.SetExpirationTask(
            ExpireAsync(
                request.HandoffId,
                pending,
                generationToken));
    }

    private ValueTask RegisterReservedAsync(
        ZLinkRemoteActorAdmissionRequest request,
        string targetSpotId,
        ZLinkRemoteActorAdmissionReply reply,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        RegisterReserved(request, targetSpotId, reply);
        return ValueTask.CompletedTask;
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

    public void BeginCommit(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId)
    {
        lock (_gate)
        {
            if (!_pending.TryGetValue(request.HandoffId, out var pending)
                || !pending.Matches(request, targetSpotId))
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' does not have a matching pending handoff admission '{request.HandoffId}'.");
            if (pending.Deadline <= _timeProvider.GetUtcNow())
            {
                RemovePendingLocked(request.HandoffId, pending);
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DeadlineExceeded,
                    $"Actor '{request.ActorId}' handoff admission '{request.HandoffId}' has expired.");
            }

            if (!pending.Reply.Accepted)
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' handoff admission '{request.HandoffId}' was rejected.");

            pending.ValidateCommit(request, targetSpotId);
            pending.Committing = true;
        }
    }

    public void RollbackCommit(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId)
    {
        lock (_gate)
        {
            if (!_pending.TryGetValue(request.HandoffId, out var pending))
                return;
            pending.ValidateCommit(request, targetSpotId);
            if (pending.Deadline <= _timeProvider.GetUtcNow())
            {
                RemovePendingLocked(request.HandoffId, pending);
                return;
            }
            pending.Committing = false;
        }
    }

    public void Complete(string handoffId)
    {
        lock (_gate)
        {
            _pending.Remove(handoffId);
            TryCompleteDrainSafeLocked();
        }
    }

    public ValueTask AbortAsync(
        string handoffId,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            _pending.Remove(handoffId);
            TryCompleteDrainSafeLocked();
        }
        return ValueTask.CompletedTask;
    }

    public ValueTask AbortReservationAsync(
        ZLinkRemoteActorAdmissionAbortRequest request,
        string targetSpotId,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        lock (_gate)
        {
            if (!_pending.TryGetValue(request.HandoffId, out var pending))
                return ValueTask.CompletedTask;
            if (!pending.MatchesAbort(request, targetSpotId))
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' admission abort does not match reservation '{request.HandoffId}'.");
            _pending.Remove(request.HandoffId);
            TryCompleteDrainSafeLocked();
        }
        return ValueTask.CompletedTask;
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
                return;
            }

            if (!terminal.Matches(request, targetSpotId))
                throw new InvalidOperationException(
                    $"Handoff outcome '{request.HandoffId}' is already assigned to another transaction.");
            if (terminal.Phase != ZLinkActorCommitPhase.Prepared)
                return;

            terminal.Reply = rejectedReply;
            terminal.Phase = ZLinkActorCommitPhase.Rejected;
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

    public Task WaitForTerminalCompletionAsync(
        ZLinkRemoteActorJoinRequest request,
        string targetSpotId,
        CancellationToken cancellationToken)
    {
        Task completion;
        lock (_gate)
        {
            if (!_terminal.TryGetValue(request.HandoffId, out var terminal)
                || !terminal.Matches(request, targetSpotId))
                throw new InvalidOperationException(
                    $"Actor '{request.ActorId}' does not have a matching terminal handoff.");
            completion = terminal.TerminalCompletion;
        }
        return completion.WaitAsync(cancellationToken);
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
            terminal.CompleteTerminal();
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
        }

        stopped.Cancel();
        stopped.Dispose();
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
        }
        foreach (var admission in pending)
        {
            lock (_gate)
            {
                if (_pending.TryGetValue(
                        admission.HandoffId,
                        out var current)
                    && ReferenceEquals(current, admission))
                    _pending.Remove(admission.HandoffId);
            }
        }
        lock (_gate) TryCompleteDrainSafeLocked();
        var failure = new InvalidOperationException(
            "Actor handoff admission belongs to a stopped framework runtime generation.");
        foreach (var execution in admitting) execution.Fail(failure);
    }

    private async Task ExpireAsync(
        string handoffId,
        PendingAdmission pending,
        CancellationToken cancellationToken)
    {
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

        PendingAdmission? expired = null;
        lock (_gate)
        {
            if (_pending.TryGetValue(handoffId, out var current)
                && ReferenceEquals(current, pending)
                && !current.Committing)
            {
                _pending.Remove(handoffId, out expired);
                TryCompleteDrainSafeLocked();
            }
        }
        if (expired is not null)
        {
            diagnostic?.Invoke(
                $"pending_admission_expired actor={expired.ActorId} handoff_id={handoffId}");
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

    private void RemovePendingLocked(
        string handoffId,
        PendingAdmission pending)
    {
        if (_pending.TryGetValue(handoffId, out var current)
            && ReferenceEquals(current, pending))
        {
            _pending.Remove(handoffId);
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
        ZLinkRemoteActorAdmissionReply reply)
    {
        public string ActorId { get; } = request.ActorId;
        public string HandoffId { get; } = request.HandoffId;

        public DateTimeOffset Deadline { get; } = deadline;

        public ZLinkRemoteActorAdmissionReply Reply { get; } = reply;

        public bool Committing { get; set; }

        public void ValidateCommit(
            ZLinkRemoteActorJoinRequest candidate,
            string candidateTargetSpotId)
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
        }

        private Task _expirationTask = Task.CompletedTask;

        public void SetExpirationTask(Task expirationTask) =>
            _expirationTask = expirationTask;

        public Task WaitForExpirationOwnerAsync() => _expirationTask;

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

        private ZLinkActorHandoffCommitIdentity CommitIdentity { get; } =
            ZLinkActorHandoffCommitIdentity.Capture(request);

        public ZLinkRemoteActorJoinReply Reply { get; set; } = reply;

        public DateTimeOffset? PreparedCompletionDeadline { get; } = preparedCompletionDeadline;

        public ZLinkActorCommitPhase Phase { get; set; } = reply.Accepted
            ? ZLinkActorCommitPhase.Prepared
            : ZLinkActorCommitPhase.Rejected;

        public ZLinkRemoteActorHandoffCompletionRequest? Completion { get; set; }

        private TaskCompletionSource TerminalCompletionSource { get; } = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        public Task TerminalCompletion => TerminalCompletionSource.Task;

        public void CompleteTerminal() => TerminalCompletionSource.TrySetResult();

        public bool Matches(ZLinkRemoteActorJoinRequest candidate, string candidateTargetSpotId)
            => targetSpotId == candidateTargetSpotId
               && CommitIdentity.Matches(candidate);

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
                == right.BoundSessionAcceptedHighWater
            && string.Equals(
                left.BoundSessionSessionOwnerId,
                right.BoundSessionSessionOwnerId,
                StringComparison.Ordinal)
            && left.BoundSessionSessionOwnerLeaseGeneration
                == right.BoundSessionSessionOwnerLeaseGeneration;
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
                == right.BoundSessionAcceptedHighWater
            && string.Equals(
                left.BoundSessionSessionOwnerId,
                right.BoundSessionSessionOwnerId,
                StringComparison.Ordinal)
            && left.BoundSessionSessionOwnerLeaseGeneration
                == right.BoundSessionSessionOwnerLeaseGeneration;
    }
}

internal sealed class ZLinkActorHandoffCommitIdentity
{
    private readonly ZLinkRemoteActorJoinRequest _request;

    private ZLinkActorHandoffCommitIdentity(ZLinkRemoteActorJoinRequest request)
    {
        _request = request with
        {
            BoundSessionNodeRid = request.BoundSessionNodeRid?.ToArray(),
            BoundSessionRid = request.BoundSessionRid?.ToArray(),
            RelocationInventoryDigest = request.RelocationInventoryDigest.ToArray(),
            Request = request.Request.ToArray(),
            HandoffFrames = [],
            SourceNodeRid = request.SourceNodeRid.ToArray(),
            TargetNodeRid = request.TargetNodeRid?.ToArray(),
            RelocationCoordinatorNodeRid =
                request.RelocationCoordinatorNodeRid?.ToArray()
        };
    }

    public static ZLinkActorHandoffCommitIdentity Capture(
        ZLinkRemoteActorJoinRequest request) => new(request);

    public bool Matches(ZLinkRemoteActorJoinRequest candidate) =>
        ZLinkActorHandoffRequestIdentity.CommitMatches(_request, candidate);
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
        return CommitMatches(left, right)
               && FramesEqual(left.HandoffFrames, right.HandoffFrames);
    }

    public static bool CommitMatches(
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
               && string.Equals(
                   left.BoundSessionSessionOwnerId,
                   right.BoundSessionSessionOwnerId,
                   StringComparison.Ordinal)
               && left.BoundSessionSessionOwnerLeaseGeneration
               == right.BoundSessionSessionOwnerLeaseGeneration
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
               && string.Equals(
                   left.RelocationCoordinatorOwnerId,
                   right.RelocationCoordinatorOwnerId,
                   StringComparison.Ordinal)
               && left.RelocationCoordinatorLeaseGeneration
               == right.RelocationCoordinatorLeaseGeneration
               && BytesEqual(
                   left.RelocationCoordinatorNodeRid,
                   right.RelocationCoordinatorNodeRid)
               && left.RelocationCoordinatorNodeGeneration
               == right.RelocationCoordinatorNodeGeneration
               && string.Equals(
                   left.RelocationCoordinatorExpectedAuthorityStoreVersion,
                   right.RelocationCoordinatorExpectedAuthorityStoreVersion,
                   StringComparison.Ordinal);
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
