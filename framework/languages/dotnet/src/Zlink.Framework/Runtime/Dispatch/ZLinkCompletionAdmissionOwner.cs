namespace Zlink.Framework.Runtime.Dispatch;

// Owns the host-wide request completion bounds. Request payload bytes stay in
// the application budget while a responder waits for a handler permit.
internal sealed class ZLinkCompletionAdmissionOwner : IDisposable
{
    private readonly object _gate = new();
    private readonly int _requestLimit;
    private readonly int _sendLimit;
    private readonly ulong _byteLimit;
    private TaskCompletionSource<bool> _changed = NewSignal();
    private int _requests;
    private int _sends;
    private ulong _requestBytes;
    private ulong _replyBytes;
    private long _generation = 1;
    private bool _stopped;

    internal ZLinkCompletionAdmissionOwner(
        int pendingRequestLimit,
        int completionSendLimit,
        ulong completionReserveByteLimit)
    {
        if (pendingRequestLimit <= 0)
            throw new ArgumentOutOfRangeException(nameof(pendingRequestLimit));
        if (completionSendLimit <= 0)
            throw new ArgumentOutOfRangeException(nameof(completionSendLimit));
        _requestLimit = pendingRequestLimit;
        _sendLimit = completionSendLimit;
        _byteLimit = completionReserveByteLimit;
    }

    internal async ValueTask<RequesterLease> AcquireRequesterAsync(
        ulong reservedReplyBytes,
        CancellationToken cancellationToken = default)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Task wait;
            lock (_gate)
            {
                ThrowIfStopped();
                if (_requests < _requestLimit && CanReserve(reservedReplyBytes))
                {
                    _requests++;
                    _requestBytes = checked(_requestBytes + reservedReplyBytes);
                    return new RequesterLease(
                        this, _generation, reservedReplyBytes);
                }
                wait = _changed.Task;
            }
            await wait.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
    }

    // This lease is acquired before the application handler starts.
    internal async ValueTask<ResponderLease> AcquireResponderAsync(
        CancellationToken cancellationToken = default)
    {
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Task wait;
            lock (_gate)
            {
                ThrowIfStopped();
                if (_sends < _sendLimit)
                {
                    _sends++;
                    return new ResponderLease(this, _generation);
                }
                wait = _changed.Task;
            }
            await wait.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
    }

    internal ZLinkCompletionAdmissionSnapshot Snapshot()
    {
        lock (_gate)
            return new(
                _requests, _requestLimit, _sends, _sendLimit,
                _requestBytes, _replyBytes, _byteLimit);
    }

    public void Dispose()
    {
        TaskCompletionSource<bool> changed;
        lock (_gate)
        {
            if (_stopped) return;
            _stopped = true;
            _generation++;
            _requests = 0;
            _sends = 0;
            _requestBytes = 0;
            _replyBytes = 0;
            changed = ReplaceSignal();
        }
        changed.TrySetResult(true);
    }

    private async ValueTask ReserveReplyAsync(
        ResponderLease lease,
        ulong replyBytes,
        CancellationToken cancellationToken)
    {
        lock (_gate)
        {
            Validate(lease);
            if (lease.Preparing || lease.Prepared)
                throw new InvalidOperationException(
                    "Reply bytes can be reserved only once.");
            lease.Preparing = true;
        }

        try
        {
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                Task wait;
                lock (_gate)
                {
                    Validate(lease);
                    if (CanReserve(replyBytes))
                    {
                        _replyBytes = checked(_replyBytes + replyBytes);
                        lease.ReplyBytes = replyBytes;
                        lease.Preparing = false;
                        lease.Prepared = true;
                        return;
                    }
                    wait = _changed.Task;
                }
                await wait.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
        }
        catch
        {
            TaskCompletionSource<bool>? changed = null;
            lock (_gate)
            {
                if (!_stopped && lease.Generation == _generation
                    && !lease.Terminal && !lease.Prepared)
                {
                    lease.Preparing = false;
                    changed = ReplaceSignal();
                }
            }
            changed?.TrySetResult(true);
            throw;
        }
    }

    private void TransferToCore(ResponderLease lease)
    {
        TaskCompletionSource<bool> changed;
        lock (_gate)
        {
            Validate(lease);
            if (!lease.Prepared)
                throw new InvalidOperationException(
                    "Reserve reply bytes before transferring ownership to Core.");
            lease.Terminal = true;
            _sends--;
            _replyBytes -= lease.ReplyBytes;
            changed = ReplaceSignal();
        }
        changed.TrySetResult(true);
    }

    private void Release(RequesterLease lease)
    {
        TaskCompletionSource<bool>? changed = null;
        lock (_gate)
        {
            if (lease.Terminal) return;
            lease.Terminal = true;
            if (_stopped || lease.Generation != _generation) return;
            _requests--;
            _requestBytes -= lease.ReservedBytes;
            changed = ReplaceSignal();
        }
        changed.TrySetResult(true);
    }

    private void Release(ResponderLease lease)
    {
        TaskCompletionSource<bool>? changed = null;
        lock (_gate)
        {
            if (lease.Terminal) return;
            lease.Terminal = true;
            if (_stopped || lease.Generation != _generation) return;
            _sends--;
            if (lease.Prepared) _replyBytes -= lease.ReplyBytes;
            changed = ReplaceSignal();
        }
        changed.TrySetResult(true);
    }

    private bool CanReserve(ulong bytes)
    {
        if (_byteLimit == 0) return true;
        var used = checked(_requestBytes + _replyBytes);
        if (used == 0) return true; // one oversized completion can progress
        return used < _byteLimit && bytes <= _byteLimit - used;
    }

    private void Validate(ResponderLease lease)
    {
        ThrowIfStopped();
        if (lease.Generation != _generation || lease.Terminal)
            throw new ObjectDisposedException(nameof(ResponderLease));
    }

    private void ThrowIfStopped()
    {
        if (_stopped)
            throw new ObjectDisposedException(nameof(ZLinkCompletionAdmissionOwner));
    }

    private TaskCompletionSource<bool> ReplaceSignal()
    {
        var previous = _changed;
        _changed = NewSignal();
        return previous;
    }

    private static TaskCompletionSource<bool> NewSignal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    internal sealed class RequesterLease : IDisposable
    {
        private readonly ZLinkCompletionAdmissionOwner _owner;
        internal RequesterLease(
            ZLinkCompletionAdmissionOwner owner, long generation, ulong bytes)
        {
            _owner = owner;
            Generation = generation;
            ReservedBytes = bytes;
        }
        internal long Generation { get; }
        internal ulong ReservedBytes { get; }
        internal bool Terminal { get; set; }
        internal void Complete() => _owner.Release(this);
        public void Dispose() => Complete();
    }

    internal sealed class ResponderLease : IDisposable
    {
        private readonly ZLinkCompletionAdmissionOwner _owner;
        internal ResponderLease(ZLinkCompletionAdmissionOwner owner, long generation)
        {
            _owner = owner;
            Generation = generation;
        }
        internal long Generation { get; }
        internal bool Preparing { get; set; }
        internal bool Prepared { get; set; }
        internal bool Terminal { get; set; }
        internal ulong ReplyBytes { get; set; }
        internal ValueTask ReserveReplyAsync(
            ulong bytes, CancellationToken cancellationToken = default) =>
            _owner.ReserveReplyAsync(this, bytes, cancellationToken);
        // Call only after Core accepts the reply. Backpressure retains the lease.
        internal void TransferToCore() => _owner.TransferToCore(this);
        public void Dispose() => _owner.Release(this);
    }
}

internal readonly record struct ZLinkCompletionAdmissionSnapshot(
    int PendingRequests,
    int PendingRequestLimit,
    int PendingCompletionSends,
    int CompletionSendLimit,
    ulong RequesterReserveBytes,
    ulong ResponderReserveBytes,
    ulong CompletionReserveByteLimit);
