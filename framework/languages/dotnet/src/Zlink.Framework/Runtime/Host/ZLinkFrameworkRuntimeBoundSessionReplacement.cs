using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal bool TryHandleBoundSessionReplacedNotification(
        RoutingId receivingNodeRid,
        RoutingId sourceNodeRid,
        ZLinkServiceWireCodec.BoundSessionReplacedRecord record)
    {
        var retired = record.RetiredSession;
        var authority = record.ActorAuthority;
        if (retired.SessionOwnerNodeRid != receivingNodeRid
            || sourceNodeRid != authority.TargetNodeRid)
            return false;

        var nodeRuntime = _state?.SpotNodes.Values.SingleOrDefault(
            node => node.Node.RoutingId == receivingNodeRid);
        if (nodeRuntime is null
            || nodeRuntime.Node.MeshStatus().LifecycleGeneration
               != retired.SessionOwnerNodeGeneration
            || !MatchesLocalSessionOwnerFence(nodeRuntime, retired))
            return false;

        // The Actor authority fence is deliberately not consulted to find an
        // Actor here. It only authenticates the transport source above; the
        // retired session identity is the sole lookup key on this node.
        if (!_actorBoundSessionCoordinator.TryGetExactRetiredSessionBinding(
                authority.ActorId,
                retired.SessionOwnerNodeRid,
                retired.SessionRid,
                retired.SessionOwnerNodeGeneration,
                retired.SessionOwnerId,
                retired.SessionOwnerLeaseGeneration,
                retired.RetiredBindingGeneration,
                out var binding)
            || binding.Context.SessionRuntime is not { } session)
            return false;

        return session.TryEnqueueActorBindingReplaced(
            authority.ActorId,
            retired.SessionOwnerNodeRid,
            retired.SessionOwnerNodeGeneration,
            retired.SessionOwnerId,
            retired.SessionOwnerLeaseGeneration,
            retired.SessionRid,
            retired.RetiredBindingGeneration,
            binding.BindingToken);
    }

    private static bool MatchesLocalSessionOwnerFence(
        ZLinkSpotNodeRuntime nodeRuntime,
        ZLinkServiceWireCodec.BoundSessionReplacedRetiredSession retired)
    {
        try
        {
            var local = nodeRuntime.LocalRequestSource;
            return string.Equals(
                       local.OwnerId,
                       retired.SessionOwnerId,
                       StringComparison.Ordinal)
                   && local.LeaseGeneration
                      == retired.SessionOwnerLeaseGeneration;
        }
        catch (InvalidOperationException)
        {
            return string.Equals(
                       retired.SessionOwnerId,
                       retired.SessionOwnerNodeRid.ToHex(),
                       StringComparison.Ordinal)
                   && retired.SessionOwnerLeaseGeneration
                      == retired.SessionOwnerNodeGeneration;
        }
    }

    private void ScheduleBoundSessionReplacedNotification(
        IZLinkBackendSpotNode sourceNode,
        RoutingId targetNodeRid,
        ZLinkServiceWireCodec.BoundSessionReplacedRecord record)
    {
        if (sourceNode is not IZLinkBackendBoundSessionReplacementNotifications sender)
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"bound_session_replaced_sender_missing target={targetNodeRid}");
            return;
        }

        var deadline = DateTime.UtcNow + Registration.DefaultRequestTimeout;
        if (!TryRunDetached(
            $"bound-session-replaced:{record.ActorAuthority.ActorId}:{record.RetiredSession.SessionRid}",
            async cancellationToken =>
            {
                while (!cancellationToken.IsCancellationRequested)
                {
                    try
                    {
                        if (sender.TrySendBoundSessionReplacedNotification(
                                targetNodeRid,
                                record))
                        {
                            ZLinkFrameworkDebugLog.SpotDiscovery(
                                $"bound_session_replaced_sent actor={record.ActorAuthority.ActorId} "
                                + $"target={targetNodeRid} session={record.RetiredSession.SessionRid}");
                            return;
                        }
                        ZLinkFrameworkDebugLog.SpotDiscovery(
                            $"bound_session_replaced_retry actor={record.ActorAuthority.ActorId} "
                            + $"target={targetNodeRid} session={record.RetiredSession.SessionRid}");
                    }
                    catch (Exception failure)
                    {
                        TryReportUnhandledCallbackException(failure);
                    }

                    if (DateTime.UtcNow >= deadline)
                        return;
                    await Task.Delay(
                            TimeSpan.FromMilliseconds(10),
                            cancellationToken)
                        .ConfigureAwait(false);
                }
            }))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"bound_session_replaced_detached_not_started actor={record.ActorAuthority.ActorId} "
                + $"target={targetNodeRid} session={record.RetiredSession.SessionRid}");
        }
    }
}
