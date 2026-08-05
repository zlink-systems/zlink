using Zlink.Framework.Internal.Locations;

namespace Zlink.Framework.Runtime.Locations;

/// <summary>
/// Maps framework-owned location store and bookkeeping keys to the shared
/// canonical identity representation.
/// </summary>
internal static class ZLinkLocationKeyCodec
{
    internal static string EncodeMeshNodeKey(ZLinkMeshNodeDescriptorKey key) =>
        ZLinkCanonicalLocationKeyFormatter.EncodeMeshNodeKey(key);

    internal static string EncodeSpotKey(ZLinkSpotLocationKey key) =>
        ZLinkCanonicalLocationKeyFormatter.EncodeSpotKey(key);

    internal static string EncodeActorKey(ZLinkActorLocationKey key) =>
        ZLinkCanonicalLocationKeyFormatter.EncodeActorKey(key);

}
