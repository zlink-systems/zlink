using System.Globalization;
using System.Text;
using Systems.Zlink;
using Zlink.Framework.Contracts.Locations;

#if ZLINK_REDIS_LOCATION_KEY_FORMATTER
namespace Zlink.Framework.Locations.Redis.Internal;
#else
namespace Zlink.Framework.Internal.Locations;
#endif

/// <summary>
/// Owns the backend-neutral canonical representation of location identities.
/// Store implementations remain responsible for their own namespaces and
/// prefixes around this representation.
/// </summary>
internal static class ZLinkCanonicalLocationKeyFormatter
{
    internal static string EncodeMeshNodeKey(ZLinkMeshNodeDescriptorKey key) =>
        Encode(key.MeshName, key.Rid.ToHex());

    internal static string EncodeSpotKey(ZLinkSpotLocationKey key)
    {
        RequireSpotId(key.SpotId);
        return Encode(key.SpotId);
    }

    internal static string EncodeActorKey(ZLinkActorLocationKey key)
    {
        RequireGlobalId(key.ActorId, nameof(key.ActorId));
        return Encode(key.ActorId);
    }

    internal static string EncodeClientServerKey(
        ZLinkClientServerServerDescriptorKey key) =>
        Encode(key.ChannelName, key.ServerRid.ToHex());

    internal static string EncodeFanoutKey(
        ZLinkFanoutPublisherDescriptorKey key) =>
        Encode(key.ChannelName, key.PublisherRid.ToHex());

    internal static string CanonicalName(ZLinkLocationAutoConnectType type) => type switch
    {
        ZLinkLocationAutoConnectType.RouteMesh => "route-mesh",
        ZLinkLocationAutoConnectType.ClientServer => "client-server",
        ZLinkLocationAutoConnectType.DealerMesh => "dealer-mesh",
        ZLinkLocationAutoConnectType.Fanout => "fanout",
        ZLinkLocationAutoConnectType.SpotMesh => "spot-mesh",
        _ => throw new ArgumentOutOfRangeException(nameof(type), type, "Unknown auto-connect type.")
    };

    internal static string CanonicalName(ZLinkLocationRole role) => role switch
    {
        ZLinkLocationRole.Spot => "spot",
        ZLinkLocationRole.Router => "router",
        ZLinkLocationRole.Dealer => "dealer",
        ZLinkLocationRole.Pub => "pub",
        ZLinkLocationRole.Sub => "sub",
        _ => throw new ArgumentOutOfRangeException(nameof(role), role, "Unknown location role.")
    };

    internal static bool IsKnown(ZLinkLocationAutoConnectType type) => type is
        ZLinkLocationAutoConnectType.RouteMesh
        or ZLinkLocationAutoConnectType.ClientServer
        or ZLinkLocationAutoConnectType.DealerMesh
        or ZLinkLocationAutoConnectType.Fanout
        or ZLinkLocationAutoConnectType.SpotMesh;

    internal static bool IsKnown(ZLinkLocationRole role) => role is
        ZLinkLocationRole.Spot
        or ZLinkLocationRole.Router
        or ZLinkLocationRole.Dealer
        or ZLinkLocationRole.Pub
        or ZLinkLocationRole.Sub;

    private static string NormalizePeerIdentity(RoutingId? nodeRid, string? endpoint) =>
        nodeRid is { } rid ? rid.ToHex() : endpoint ?? string.Empty;

    private static string Encode(params string[] segments)
    {
        var builder = new StringBuilder();
        foreach (var segment in segments)
        {
            builder
                .Append(segment.Length.ToString(CultureInfo.InvariantCulture))
                .Append(':')
                .Append(segment);
        }

        return builder.ToString();
    }

    private static void RequireSpotId(string spotId)
        => RequireGlobalId(spotId, nameof(spotId));

    private static void RequireGlobalId(string value, string parameterName)
    {
        int size;
        try
        {
            size = new UTF8Encoding(
                encoderShouldEmitUTF8Identifier: false,
                throwOnInvalidBytes: true).GetByteCount(value);
        }
        catch (EncoderFallbackException exception)
        {
            throw new ArgumentException(
                "The logical ID must contain valid UTF-8 text.",
                parameterName,
                exception);
        }

        if (size is < 1 or > 255 || value.Contains('\0'))
        {
            throw new ArgumentOutOfRangeException(
                parameterName,
                "The logical ID must be 1 to 255 UTF-8 bytes without NUL.");
        }
    }
}
