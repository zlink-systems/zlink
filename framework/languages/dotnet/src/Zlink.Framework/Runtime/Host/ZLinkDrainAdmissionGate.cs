using Zlink.Framework.Runtime.Execution;

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

internal readonly record struct ZLinkRelocationRollbackLeaseAcquisition(
    bool Acquired,
    ZLinkRelocationRollbackLease? Lease);

internal readonly record struct ZLinkActorAdmissionResult(
    bool Accepted,
    ZLinkDrainAdmissionGate.ActorAdmissionLease Lease);

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
    private readonly ZLinkStateLane _lane = new();
    private int _acceptedActorAdmissions;
    private long _actorAdmissionEpoch;
    private TaskCompletionSource? _actorAdmissionsDrained;
    private int _draining;
    private int _sealed;
    private ZLinkDrainOwner _owner;
    private ZLinkRelocationRollbackLease? _rollbackLease;

    public bool IsDraining => RunState(() => _draining != 0);

    public bool BeginDrain() => BeginDrain(ZLinkDrainOwner.Relocation);

    internal bool BeginDrain(ZLinkDrainOwner owner)
    {
        if (owner == ZLinkDrainOwner.None)
            throw new ArgumentOutOfRangeException(nameof(owner));
        var result = RunState(() =>
        {
            if (_draining != 0)
            {
                if (owner == ZLinkDrainOwner.Shutdown
                    && (_owner is ZLinkDrainOwner.Relocation
                        or ZLinkDrainOwner.RelocationRollback))
                {
                    var rollbackLease = _rollbackLease;
                    _rollbackLease = null;
                    _owner = ZLinkDrainOwner.Shutdown;
                    return (Started: true, RollbackLease: rollbackLease);
                }
                else
                    return (Started: false, RollbackLease: (ZLinkRelocationRollbackLease?)null);
            }
            else
            {
                _draining = 1;
                _owner = owner;
                return (Started: true, RollbackLease: (ZLinkRelocationRollbackLease?)null);
            }
        });
        result.RollbackLease?.Invalidate();
        return result.Started;
    }

    internal void ClaimShutdown()
    {
        var rollbackLease = RunState(() =>
        {
            _draining = 1;
            var current = _rollbackLease;
            _rollbackLease = null;
            _owner = ZLinkDrainOwner.Shutdown;
            _sealed = 1;
            return current;
        });
        rollbackLease?.Invalidate();
    }

    public bool IsSealed => RunState(() => _sealed != 0);

    public void Seal() => RunState(() => _sealed = 1);

    public void Reset()
    {
        RunState(() =>
        {
            if (_acceptedActorAdmissions != 0)
                throw new InvalidOperationException(
                    "The drain admission gate cannot reset while actor admissions are active.");
            _actorAdmissionsDrained = null;
            _sealed = 0;
            _draining = 0;
            _rollbackLease = null;
            _owner = ZLinkDrainOwner.None;
        });
    }

    internal bool TryBeginRelocationFence(
        Func<ZLinkActorAdmissionSnapshot, bool> commit)
    {
        ArgumentNullException.ThrowIfNull(commit);
        var prepared = RunState(() =>
        {
            if (_draining != 0) return (Started: false, Snapshot: default(ZLinkActorAdmissionSnapshot));
            _draining = 1;
            _owner = ZLinkDrainOwner.Relocation;
            return (Started: true, Snapshot: new ZLinkActorAdmissionSnapshot(
                _actorAdmissionEpoch,
                _acceptedActorAdmissions));
        });
        if (!prepared.Started) return false;
        var committed = false;
        try
        {
            committed = commit(prepared.Snapshot);
            return committed;
        }
        finally
        {
            //  The callback runs outside the state lane so a callback can inspect this gate
            //  without re-entering it. The failed fence restores the same state as the old
            //  lock-protected finally block.
            if (!committed)
                RunState(() =>
                {
                    _draining = 0;
                    _owner = ZLinkDrainOwner.None;
                });
        }
    }

    internal bool TryReopenRelocationFence(Func<bool> reopen)
    {
        ArgumentNullException.ThrowIfNull(reopen);
        if (!RunState(() => _owner == ZLinkDrainOwner.Relocation
                            && _acceptedActorAdmissions == 0))
            return false;
        if (!reopen()) return false;
        return RunState(() =>
        {
            if (_owner != ZLinkDrainOwner.Relocation
                || _acceptedActorAdmissions != 0)
                return false;

            _actorAdmissionsDrained = null;
            _sealed = 0;
            _draining = 0;
            _owner = ZLinkDrainOwner.None;
            return true;
        });
    }

    internal ZLinkRelocationRollbackLeaseAcquisition TryAcquireRelocationRollbackLease(
        Func<bool> acquire)
    {
        ArgumentNullException.ThrowIfNull(acquire);
        if (!RunState(() => _owner == ZLinkDrainOwner.Relocation
                            && _acceptedActorAdmissions == 0))
            return new ZLinkRelocationRollbackLeaseAcquisition(false, null);
        if (!acquire()) return new ZLinkRelocationRollbackLeaseAcquisition(false, null);
        return RunState(() =>
        {
            if (_owner != ZLinkDrainOwner.Relocation
                || _acceptedActorAdmissions != 0)
                return new ZLinkRelocationRollbackLeaseAcquisition(false, null);

            var lease = new ZLinkRelocationRollbackLease();
            _rollbackLease = lease;
            _owner = ZLinkDrainOwner.RelocationRollback;
            return new ZLinkRelocationRollbackLeaseAcquisition(true, lease);
        });
    }

    internal bool TryAcquireRelocationRollbackLease(
        Func<bool> acquire,
        out ZLinkRelocationRollbackLease? lease)
    {
        var result = TryAcquireRelocationRollbackLease(acquire);
        lease = result.Lease;
        return result.Acquired;
    }

    internal bool TryCompleteRelocationRollbackLease(
        ZLinkRelocationRollbackLease lease,
        Func<bool> complete)
    {
        ArgumentNullException.ThrowIfNull(lease);
        ArgumentNullException.ThrowIfNull(complete);
        if (!RunState(() => _owner == ZLinkDrainOwner.RelocationRollback
                            && ReferenceEquals(_rollbackLease, lease)
                            && lease.IsCurrent
                            && _acceptedActorAdmissions == 0))
            return false;
        if (!complete()) return false;
        var completed = RunState(() =>
        {
            if (_owner != ZLinkDrainOwner.RelocationRollback
                || !ReferenceEquals(_rollbackLease, lease)
                || !lease.IsCurrent
                || _acceptedActorAdmissions != 0)
                return false;

            _rollbackLease = null;
            _actorAdmissionsDrained = null;
            _sealed = 0;
            _draining = 0;
            _owner = ZLinkDrainOwner.None;
            return true;
        });
        if (completed) lease.Dispose();
        return completed;
    }

    internal ZLinkActorAdmissionResult TryEnterActorAdmission()
    {
        return RunState(() =>
        {
            if (_draining != 0)
                return new ZLinkActorAdmissionResult(false, new ActorAdmissionLease(null));
            _acceptedActorAdmissions++;
            _actorAdmissionEpoch++;
            return new ZLinkActorAdmissionResult(true, new ActorAdmissionLease(this));
        });
    }

    public bool TryEnterActorAdmission(out ActorAdmissionLease lease)
    {
        var result = TryEnterActorAdmission();
        lease = result.Lease;
        return result.Accepted;
    }

    internal ZLinkActorAdmissionSnapshot SnapshotActorAdmissions() =>
        RunState(SnapshotActorAdmissionsOnLane);

    public Task WaitForAcceptedActorAdmissionsAsync(CancellationToken cancellationToken) =>
        RunState(() =>
        {
            if (_acceptedActorAdmissions == 0) return Task.CompletedTask;
            var pending = (_actorAdmissionsDrained ??= new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously)).Task;
            return pending.WaitAsync(cancellationToken);
        });

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
        var drained = RunState(() =>
        {
            if (--_acceptedActorAdmissions < 0)
                throw new InvalidOperationException("Actor admission lease count became negative.");
            _actorAdmissionEpoch++;
            if (_acceptedActorAdmissions == 0)
            {
                var current = _actorAdmissionsDrained;
                _actorAdmissionsDrained = null;
                return current;
            }
            return null;
        });
        drained?.TrySetResult();
    }

    private ZLinkActorAdmissionSnapshot SnapshotActorAdmissionsOnLane() =>
        new(_actorAdmissionEpoch, _acceptedActorAdmissions);

    private T RunState<T>(Func<T> operation) =>
        AwaitStateLane(_lane.RunAsync(operation));

    private void RunState(Action operation) =>
        AwaitStateLane(_lane.RunAsync(operation));

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    public sealed class ActorAdmissionLease(ZLinkDrainAdmissionGate? owner) : IDisposable
    {
        private ZLinkDrainAdmissionGate? _owner = owner;

        public void Dispose() => Interlocked.Exchange(ref _owner, null)?.ExitActorAdmission();
    }
}
