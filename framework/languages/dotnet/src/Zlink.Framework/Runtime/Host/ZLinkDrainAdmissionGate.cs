namespace Zlink.Framework.Runtime.Host;

internal enum ZLinkDrainOwner
{
    None = 0,
    Relocation = 1,
    Shutdown = 2,
    RelocationRollback = 3
}

internal readonly record struct ZLinkActorAdmissionSnapshot(
    long Epoch,
    int ActiveCount);

internal sealed class ZLinkRelocationRollbackLease : IDisposable
{
    private readonly CancellationTokenSource _cancellation = new();
    private int _valid = 1;

    internal CancellationToken CancellationToken => _cancellation.Token;

    internal bool IsCurrent => Volatile.Read(ref _valid) != 0;

    internal void Invalidate()
    {
        if (Interlocked.Exchange(ref _valid, 0) != 0)
        {
            try
            {
                _cancellation.Cancel();
            }
            catch
            {
                //  Ownership is already invalidated. A cancellation callback
                // failure must not re-enter the admission transition.
            }
        }
    }

    public void Dispose() => Invalidate();
}

internal sealed class ZLinkDrainAdmissionGate
{
    private readonly object _gate = new();
    private int _acceptedActorAdmissions;
    private long _actorAdmissionEpoch;
    private TaskCompletionSource? _actorAdmissionsDrained;
    private int _draining;
    private int _sealed;
    private ZLinkDrainOwner _owner;
    private ZLinkRelocationRollbackLease? _rollbackLease;

    public bool IsDraining => Volatile.Read(ref _draining) != 0;

    public bool BeginDrain() => BeginDrain(ZLinkDrainOwner.Relocation);

    internal bool BeginDrain(ZLinkDrainOwner owner)
    {
        if (owner == ZLinkDrainOwner.None)
            throw new ArgumentOutOfRangeException(nameof(owner));
        ZLinkRelocationRollbackLease? rollbackLease = null;
        lock (_gate)
        {
            if (_draining != 0)
            {
                if (owner == ZLinkDrainOwner.Shutdown
                    && (_owner is ZLinkDrainOwner.Relocation
                        or ZLinkDrainOwner.RelocationRollback))
                {
                    rollbackLease = _rollbackLease;
                    _rollbackLease = null;
                    _owner = ZLinkDrainOwner.Shutdown;
                }
                else
                    return false;
            }
            else
            {
                Volatile.Write(ref _draining, 1);
                _owner = owner;
                return true;
            }
        }
        rollbackLease?.Invalidate();
        return true;
    }

    internal void ClaimShutdown()
    {
        ZLinkRelocationRollbackLease? rollbackLease;
        lock (_gate)
        {
            Volatile.Write(ref _draining, 1);
            rollbackLease = _rollbackLease;
            _rollbackLease = null;
            _owner = ZLinkDrainOwner.Shutdown;
            Volatile.Write(ref _sealed, 1);
        }
        rollbackLease?.Invalidate();
    }

    public bool IsSealed => Volatile.Read(ref _sealed) != 0;

    public void Seal() => Volatile.Write(ref _sealed, 1);

    public void Reset()
    {
        lock (_gate)
        {
            if (_acceptedActorAdmissions != 0)
                throw new InvalidOperationException(
                    "The drain admission gate cannot reset while actor admissions are active.");
            _actorAdmissionsDrained = null;
            Volatile.Write(ref _sealed, 0);
            Volatile.Write(ref _draining, 0);
            _rollbackLease = null;
            _owner = ZLinkDrainOwner.None;
        }
    }

    internal bool TryBeginRelocationFence(
        Func<ZLinkActorAdmissionSnapshot, bool> commit)
    {
        ArgumentNullException.ThrowIfNull(commit);
        lock (_gate)
        {
            if (_draining != 0) return false;
            Volatile.Write(ref _draining, 1);
            _owner = ZLinkDrainOwner.Relocation;
            var snapshot = new ZLinkActorAdmissionSnapshot(
                _actorAdmissionEpoch,
                _acceptedActorAdmissions);
            var committed = false;
            try
            {
                if (commit(snapshot))
                {
                    committed = true;
                    return true;
                }
                return false;
            }
            finally
            {
                if (!committed)
                {
                    Volatile.Write(ref _draining, 0);
                    _owner = ZLinkDrainOwner.None;
                }
            }
        }
    }

    internal bool TryReopenRelocationFence(Func<bool> reopen)
    {
        ArgumentNullException.ThrowIfNull(reopen);
        lock (_gate)
        {
            if (_owner != ZLinkDrainOwner.Relocation
                || _acceptedActorAdmissions != 0)
                return false;
            if (!reopen()) return false;

            _actorAdmissionsDrained = null;
            Volatile.Write(ref _sealed, 0);
            Volatile.Write(ref _draining, 0);
            _owner = ZLinkDrainOwner.None;
            return true;
        }
    }

    internal bool TryAcquireRelocationRollbackLease(
        Func<bool> acquire,
        out ZLinkRelocationRollbackLease? lease)
    {
        ArgumentNullException.ThrowIfNull(acquire);
        lock (_gate)
        {
            lease = null;
            if (_owner != ZLinkDrainOwner.Relocation
                || _acceptedActorAdmissions != 0
                || !acquire())
                return false;
            lease = new ZLinkRelocationRollbackLease();
            _rollbackLease = lease;
            _owner = ZLinkDrainOwner.RelocationRollback;
            return true;
        }
    }

    internal bool TryCompleteRelocationRollbackLease(
        ZLinkRelocationRollbackLease lease,
        Func<bool> complete)
    {
        ArgumentNullException.ThrowIfNull(lease);
        ArgumentNullException.ThrowIfNull(complete);
        lock (_gate)
        {
            if (_owner != ZLinkDrainOwner.RelocationRollback
                || !ReferenceEquals(_rollbackLease, lease)
                || !lease.IsCurrent
                || _acceptedActorAdmissions != 0
                || !complete())
                return false;

            _rollbackLease = null;
            _actorAdmissionsDrained = null;
            Volatile.Write(ref _sealed, 0);
            Volatile.Write(ref _draining, 0);
            _owner = ZLinkDrainOwner.None;
        }
        lease.Dispose();
        return true;
    }

    public bool TryEnterActorAdmission(out ActorAdmissionLease lease)
    {
        lock (_gate)
        {
            if (_draining != 0)
            {
                lease = new ActorAdmissionLease(null);
                return false;
            }
            _acceptedActorAdmissions++;
            _actorAdmissionEpoch++;
            lease = new ActorAdmissionLease(this);
            return true;
        }
    }

    internal ZLinkActorAdmissionSnapshot SnapshotActorAdmissions()
    {
        lock (_gate)
            return SnapshotActorAdmissionsUnderLock();
    }

    public Task WaitForAcceptedActorAdmissionsAsync(CancellationToken cancellationToken)
    {
        lock (_gate)
        {
            if (_acceptedActorAdmissions == 0) return Task.CompletedTask;
            var pending = (_actorAdmissionsDrained ??= new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously)).Task;
            return pending.WaitAsync(cancellationToken);
        }
    }

    public void RequireSpotAdmission()
    {
        if (IsDraining)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "The framework runtime is draining and does not accept new SPOT assignments.",
                ZLinkRetryAdvice.DoNotRetry);
    }

    public void RequireActorAdmission()
    {
        if (IsDraining)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.Rejected,
                "The framework runtime is draining and does not accept new actor assignments.",
                ZLinkRetryAdvice.DoNotRetry);
    }

    private void ExitActorAdmission()
    {
        TaskCompletionSource? drained = null;
        lock (_gate)
        {
            if (--_acceptedActorAdmissions < 0)
                throw new InvalidOperationException("Actor admission lease count became negative.");
            _actorAdmissionEpoch++;
            if (_acceptedActorAdmissions == 0)
            {
                drained = _actorAdmissionsDrained;
                _actorAdmissionsDrained = null;
            }
        }
        drained?.TrySetResult();
    }

    private ZLinkActorAdmissionSnapshot SnapshotActorAdmissionsUnderLock() =>
        new(_actorAdmissionEpoch, _acceptedActorAdmissions);

    public sealed class ActorAdmissionLease(ZLinkDrainAdmissionGate? owner) : IDisposable
    {
        private ZLinkDrainAdmissionGate? _owner = owner;

        public void Dispose() => Interlocked.Exchange(ref _owner, null)?.ExitActorAdmission();
    }
}
