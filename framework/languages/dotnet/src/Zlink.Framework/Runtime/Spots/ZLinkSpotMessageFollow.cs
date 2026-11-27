using System.Diagnostics;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Spots;

internal enum ZLinkSpotMessageFollowResult
{
    NotApplicable,
    Followed,
    StaleRejected,
    Full,
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
    TimeSpan expiresAt,
    ZLinkBoundedIngressAdmission? admission = null
)
{
    private readonly ZLinkBoundedIngressAdmission _admission =
        admission ?? new ZLinkBoundedIngressAdmission();
    private readonly ZLinkMessageFollowSuppressionRegistry _suppression = new(capacity: 1);

    internal RoutingId TargetNodeRid { get; } = targetNodeRid;
    internal ulong ObjectGeneration { get; } = objectGeneration;
    internal ulong SourceNodeGeneration { get; } = sourceNodeGeneration;
    internal ulong TargetNodeGeneration { get; } = targetNodeGeneration;
    internal ulong SourceAuthorityOwnerGeneration { get; } = sourceAuthorityOwnerGeneration;
    internal ulong TargetAuthorityOwnerGeneration { get; } = targetAuthorityOwnerGeneration;
    internal ZLinkLocationOwnerToken SourceOwner { get; } = sourceOwner;
    internal ZLinkLocationOwnerToken TargetOwner { get; } = targetOwner;
    internal TimeSpan ExpiresAt { get; } = expiresAt;

    internal bool ShouldRemoveAfterRejectedFrame(TimeSpan now) => ExpiresAt <= now;

    internal bool MatchesSourceRoute(
        ulong objectGeneration,
        ulong nodeGeneration,
        ulong authorityOwnerGeneration,
        ulong ownerLeaseGeneration,
        byte hopCount,
        TimeSpan now
    ) =>
        ExpiresAt > now
        && hopCount < ZLinkServiceWireCodec.MessageFollowMaximumHopCount
        && ObjectGeneration != 0
        && ObjectGeneration == objectGeneration
        && nodeGeneration == SourceNodeGeneration
        && authorityOwnerGeneration == SourceAuthorityOwnerGeneration
        && SourceOwner.LeaseGeneration > 0
        && TargetOwner.LeaseGeneration > 0
        && ownerLeaseGeneration == checked((ulong)SourceOwner.LeaseGeneration)
        && SourceAuthorityOwnerGeneration is > 0 and <= long.MaxValue
        && TargetAuthorityOwnerGeneration is > 0 and <= long.MaxValue
        && TargetAuthorityOwnerGeneration > SourceAuthorityOwnerGeneration;

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

    internal (int Records, long Bytes) AdmissionSnapshot() => _admission.Snapshot();

    internal bool TryBeginMessageFollowNotice(ZLinkMessageFollowFence fence) =>
        _suppression.TryBegin(fence);

    internal void MarkMessageFollowNoticeSent(ZLinkMessageFollowFence fence) =>
        _suppression.MarkSent(fence);

    internal void AbortMessageFollowNotice(ZLinkMessageFollowFence fence) =>
        _suppression.Abort(fence);

    internal async ValueTask WaitForExpiryAndDrainAsync(CancellationToken cancellationToken)
    {
        var remaining = ExpiresAt - Stopwatch.GetElapsedTime(0);
        if (remaining > TimeSpan.Zero)
            await Task.Delay(remaining, cancellationToken).ConfigureAwait(false);
        await _admission.CloseAndWaitForEmptyAsync(cancellationToken).ConfigureAwait(false);
        _suppression.ExpireAll();
    }

    internal sealed class AdmissionLease(ZLinkBoundedIngressAdmission admission, long bytes)
        : IDisposable
    {
        private int _disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _disposed, 1) == 0)
                admission.Release(bytes);
        }
    }
}
