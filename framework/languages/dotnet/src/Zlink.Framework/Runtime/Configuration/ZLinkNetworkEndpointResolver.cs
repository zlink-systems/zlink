namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkNetworkEndpointResolver
{
    public static string Bind(
        string? explicitEndpoint,
        int? port,
        string? listenerBindHost,
        ZLinkNetworkOptionsModel network)
    {
        if (explicitEndpoint is not null)
            return ZLinkEndpointNotation.Normalize(explicitEndpoint);
        return ZLinkEndpointNotation.Normalize(
            $"tcp://{listenerBindHost ?? network.BindHost}:{port.GetValueOrDefault()}");
    }

    public static string Advertise(
        string boundEndpoint,
        string? listenerAdvertiseHost,
        string? listenerBindHost,
        ZLinkNetworkOptionsModel network)
    {
        var endpoint = new Uri(boundEndpoint, UriKind.Absolute);

        // Per doc/plan/endpoint-notation-policy.ko.md §2.2 every scheme is
        // normalized identically (the previous tcp-only early return skipped
        // normalization entirely for other schemes). AdvertiseHost
        // substitution, however, only makes sense for a real network
        // transport: an inproc/ipc endpoint's "host" segment is an opaque
        // process-local identity, not something a remote peer dials, and
        // overwriting it with BindHost/AdvertiseHost would corrupt it.
        if (!ZLinkEndpointNotation.IsAuthorityScheme(endpoint.Scheme.ToLowerInvariant()))
            return ZLinkEndpointNotation.Normalize(boundEndpoint);

        var bindHost = listenerBindHost ?? network.BindHost;
        var advertiseHost = listenerAdvertiseHost
            ?? network.AdvertiseHost
            ?? (!IsWildcard(bindHost) ? bindHost : null);
        if (advertiseHost is null)
            throw new ZLinkConfigurationException(
                "AdvertiseHost is required when BindHost is a wildcard address.");

        var builder = new UriBuilder(endpoint) { Host = advertiseHost };
        return ZLinkEndpointNotation.Normalize(builder.Uri.ToString());
    }

    public static bool IsWildcard(string host) =>
        string.Equals(host, "0.0.0.0", StringComparison.Ordinal)
        || string.Equals(host, "::", StringComparison.Ordinal);
}
