namespace Zlink.Framework.Contracts.Locations;

internal sealed record ZLinkSpotTypeCapacityDelta(
    ZLinkPlacementObjectKind ObjectKind,
    string StableType,
    int Count);

internal sealed record ZLinkCapacityVector(
    int Actors,
    int Spots,
    ZLinkSpotTypeCapacityDelta? SpotType);
