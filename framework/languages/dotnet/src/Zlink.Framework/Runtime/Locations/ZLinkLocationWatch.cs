namespace Zlink.Framework.Runtime.Locations;

internal enum ZLinkLocationKind
{
    Invalid = 0,
    MeshNode = 1,
    Spot = 2,
    Actor = 3
}

internal abstract record ZLinkLocationKey
{
    private ZLinkLocationKey() { }

    internal sealed record MeshNode(ZLinkMeshNodeDescriptorKey Key) : ZLinkLocationKey;
    internal sealed record Spot(ZLinkSpotLocationKey Key) : ZLinkLocationKey;
    internal sealed record Actor(ZLinkActorLocationKey Key) : ZLinkLocationKey;
}

/// <summary>
/// Optional change notification capability recognized on a registered
/// location store. Not part of the public store contract
/// (06-location-store): polling remains the correctness path and event
/// loss is tolerated, so the runtime consumes this internally only.
/// </summary>
internal interface IZLinkLocationWatchStore
{
    IAsyncEnumerable<ZLinkLocationChanged> WatchAsync(
        ZLinkLocationWatchFilter filter,
        CancellationToken cancellationToken = default);
}

internal sealed record ZLinkLocationWatchFilter(
    ZLinkLocationKind Kind,
    string? MeshName = null);

internal enum ZLinkLocationChangeType
{
    Upserted = 1,
    Removed = 2,
    Expired = 3
}

/// <summary>
/// Change events carry typed keys. Backends that transport encoded string
/// keys decode them inside the store implementation.
/// </summary>
internal sealed record ZLinkLocationChanged(
    ZLinkLocationKind Kind,
    ZLinkLocationKey Key,
    ZLinkLocationChangeType ChangeType,
    ulong Generation,
    DateTimeOffset UpdatedAt);
