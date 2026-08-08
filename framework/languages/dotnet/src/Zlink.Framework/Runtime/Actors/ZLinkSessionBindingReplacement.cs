namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkSessionBindingReplacement
{
    internal static ZLinkActorPreviousBindingFence? CreateFence(
        ZLinkRemoteSessionPreviousBinding? previous)
    {
        if (previous is null) return null;
        return new ZLinkActorPreviousBindingFence(
            RoutingId.From(previous.TargetNodeRid),
            RoutingId.From(previous.SessionNodeRid),
            RoutingId.From(previous.SessionRid),
            previous.BindingToken,
            previous.BindingGeneration,
            previous.ObjectGeneration,
            ZLinkMeshName.FromBoundary(
                previous.MeshName,
                nameof(previous.MeshName)),
            previous.TargetNodeGeneration,
            previous.AuthorityOwnerGeneration,
            previous.OwnerLeaseGeneration,
            previous.SessionOwnerNodeGeneration,
            previous.AcceptedHighWater,
            previous.SessionOwnerId,
            previous.SessionOwnerLeaseGeneration);
    }

}
