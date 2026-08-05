namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkSpotMessageFollowResult
{
    NotApplicable,
    Followed,
    StaleRejected,
    Full
}

internal sealed class ZLinkSpotMessageFollow(
    RoutingId targetNodeRid,
    ulong objectGeneration,
    ulong sourceNodeGeneration,
    ulong targetNodeGeneration,
    ulong sourceAuthorityOwnerGeneration,
    ulong targetAuthorityOwnerGeneration,
    ZLinkLocationOwnerToken sourceOwner,
    ZLinkLocationOwnerToken targetOwner,
    DateTimeOffset expiresAt,
    ZLinkBoundedIngressAdmission? admission = null)
{
    private readonly ZLinkBoundedIngressAdmission _admission =
        admission ?? new ZLinkBoundedIngressAdmission();
    private int _messageFollowNoticeClaimed;

    internal RoutingId TargetNodeRid { get; } = targetNodeRid;
    internal ulong ObjectGeneration { get; } = objectGeneration;
    internal ulong SourceNodeGeneration { get; } = sourceNodeGeneration;
    internal ulong TargetNodeGeneration { get; } = targetNodeGeneration;
    internal ulong SourceAuthorityOwnerGeneration { get; } =
        sourceAuthorityOwnerGeneration;
    internal ulong TargetAuthorityOwnerGeneration { get; } =
        targetAuthorityOwnerGeneration;
    internal ZLinkLocationOwnerToken SourceOwner { get; } = sourceOwner;
    internal ZLinkLocationOwnerToken TargetOwner { get; } = targetOwner;
    internal DateTimeOffset ExpiresAt { get; } = expiresAt;

    internal bool ShouldRemoveAfterRejectedFrame(DateTimeOffset now) =>
        ExpiresAt <= now;

    internal bool MatchesSourceRoute(
        ZLinkBackendRouteReceived received,
        ulong currentObjectGeneration,
        ZLinkLocationOwnerToken? currentSourceOwner,
        DateTimeOffset now) =>
        ExpiresAt > now
        && received.MessageFollowHopCount < 8
        && received.OperationId.High != 0
        && received.OperationId.Low != 0
        && ObjectGeneration != 0
        && ObjectGeneration == currentObjectGeneration
        && received.TargetNodeGeneration == SourceNodeGeneration
        && received.AuthorityOwnerGeneration == SourceAuthorityOwnerGeneration
        && SourceOwner.LeaseGeneration > 0
        && TargetOwner.LeaseGeneration > 0
        && received.OwnerLeaseGeneration
           == checked((ulong)SourceOwner.LeaseGeneration)
        && SourceAuthorityOwnerGeneration
           is > 0 and <= long.MaxValue
        && TargetAuthorityOwnerGeneration
           is > 0 and <= long.MaxValue
        && TargetAuthorityOwnerGeneration
           > SourceAuthorityOwnerGeneration
        && currentSourceOwner is { } owner
        && owner == SourceOwner;

    internal bool TryAcquire(long bytes, out AdmissionLease? lease)
    {
        if (!_admission.TryAcquire(bytes))
        {
            lease = null;
            return false;
        }

        lease = new AdmissionLease(_admission, bytes);
        return true;
    }

    internal (int Records, long Bytes) AdmissionSnapshot() =>
        _admission.Snapshot();

    internal bool TryClaimMessageFollowNotice() =>
        Interlocked.CompareExchange(
            ref _messageFollowNoticeClaimed,
            1,
            0) == 0;

    internal void ReleaseMessageFollowNoticeClaim() =>
        Volatile.Write(ref _messageFollowNoticeClaimed, 0);

    internal async ValueTask WaitForExpiryAndDrainAsync(
        CancellationToken cancellationToken)
    {
        var remaining = ExpiresAt - DateTimeOffset.UtcNow;
        if (remaining > TimeSpan.Zero)
            await Task.Delay(remaining, cancellationToken).ConfigureAwait(false);
        await _admission.CloseAndWaitForEmptyAsync(cancellationToken)
            .ConfigureAwait(false);
    }

    internal sealed class AdmissionLease(
        ZLinkBoundedIngressAdmission admission,
        long bytes) : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                admission.Release(bytes);
        }
    }
}
