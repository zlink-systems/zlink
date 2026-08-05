namespace Zlink.Framework.Runtime.Locations;

internal sealed record ZLinkResolvedSpotLocation(
    string MeshName,
    string SpotId,
    ulong SpotGeneration,
    RoutingId OwnerNodeRid,
    ulong OwnerNodeGeneration,
    ZLinkSpotKind SpotKind,
    string SpotType,
    string OwnerId,
    long LeaseGeneration,
    DateTimeOffset UpdatedAt,
    ulong AuthorityOwnerGeneration)
{
    internal ZLinkSpotLocation ToPublic() =>
        new(
            MeshName,
            SpotId,
            SpotGeneration,
            OwnerNodeRid,
            OwnerNodeGeneration,
            SpotKind,
            SpotType,
            OwnerId,
            LeaseGeneration,
            UpdatedAt);
}

internal sealed record ZLinkResolvedActorLocation(
    string MeshName,
    string ActorId,
    string ActorType,
    ActorRef ActorRef,
    RoutingId OwnerNodeRid,
    ulong OwnerNodeGeneration,
    string SpotId,
    ulong SpotGeneration,
    ZLinkSpotKind SpotKind,
    ulong MembershipEpoch,
    string OwnerId,
    long LeaseGeneration,
    DateTimeOffset UpdatedAt,
    ulong AuthorityOwnerGeneration)
{
    internal ZLinkActorLocation ToPublic() =>
        new(
            MeshName,
            ActorId,
            ActorType,
            ActorRef,
            OwnerNodeRid,
            OwnerNodeGeneration,
            SpotId,
            SpotGeneration,
            SpotKind,
            OwnerId,
            LeaseGeneration,
            UpdatedAt);
}
