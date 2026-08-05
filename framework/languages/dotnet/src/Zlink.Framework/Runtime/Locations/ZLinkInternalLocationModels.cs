namespace Zlink.Framework.Contracts.Locations;

/// <summary>
/// Channel auto-connect uses a subset with the same numeric values; DealerMesh
/// remains location-only because the channel registration API does not expose it.
/// </summary>
internal enum ZLinkLocationAutoConnectType
{
    Invalid = 0,
    RouteMesh = 1,
    ClientServer = 2,
    DealerMesh = 3,
    Fanout = 4,
    SpotMesh = 5
}

internal readonly record struct ZLinkSpotLocationKey(string SpotId);

internal readonly record struct ZLinkActorLocationKey(string ActorId);

/// <summary>
/// Runtime projection for the current location of one logical Spot. The owner
/// identity and lease fence bind route admission to the same host lifetime.
/// </summary>
internal sealed record ZLinkSpotLocation(
    string MeshName,
    string SpotId,
    ulong SpotGeneration,
    RoutingId OwnerNodeRid,
    ulong OwnerNodeGeneration,
    ZLinkSpotKind SpotKind,
    string SpotType,
    string OwnerId,
    long LeaseGeneration,
    DateTimeOffset UpdatedAt);

/// <summary>
/// Runtime projection for the current location of one Actor. Authority and
/// membership generations remain in the authority payload.
/// </summary>
internal sealed record ZLinkActorLocation(
    string MeshName,
    string ActorId,
    string ActorType,
    ActorRef ActorRef,
    RoutingId OwnerNodeRid,
    ulong OwnerNodeGeneration,
    string SpotId,
    ulong SpotGeneration,
    ZLinkSpotKind SpotKind,
    string OwnerId,
    long LeaseGeneration,
    DateTimeOffset UpdatedAt);
