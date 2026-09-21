namespace Zlink.Framework.Runtime.Configuration;

internal static class ZLinkNetworkEndpointResolver
{
    public static string Bind(
        string? explicitEndpoint,
        int? port,
        string? listenerBindHost,
        ZLinkNetworkOptionsModel network
    )
    {
        if (explicitEndpoint is not null)
            return ZLinkEndpointNotation.Normalize(explicitEndpoint);
        var bindHost = listenerBindHost ?? network.BindHost;
        return ZLinkEndpointNotation.Normalize(
            $"tcp://{FormatAuthorityHost(bindHost)}:{port.GetValueOrDefault()}"
        );
    }

    public static string Advertise(
        string boundEndpoint,
        string? listenerAdvertiseHost,
        string? listenerBindHost,
        ZLinkNetworkOptionsModel network
    )
    {
        var endpoint = new Uri(boundEndpoint, UriKind.Absolute);

        // Every scheme is normalized identically (the previous tcp-only
        // early return skipped normalization entirely for other schemes).
        // AdvertiseHost substitution, however, only makes sense for a real
        // network transport: an inproc/ipc endpoint's "host" segment is an
        // opaque process-local identity, not something a remote peer dials,
        // and overwriting it with BindHost/AdvertiseHost would corrupt it.
        if (!ZLinkEndpointNotation.IsAuthorityScheme(endpoint.Scheme.ToLowerInvariant()))
            return ZLinkEndpointNotation.Normalize(boundEndpoint);

        var bindHost = listenerBindHost ?? network.BindHost;
        var advertiseHost = listenerAdvertiseHost ?? network.AdvertiseHost;
        if (advertiseHost is not null && IsWildcard(advertiseHost))
            throw new ZLinkConfigurationException("AdvertiseHost must not be a wildcard address.");

        advertiseHost ??= bindHost switch
        {
            "0.0.0.0" => "127.0.0.1",
            "::" => "::1",
            _ => bindHost,
        };

        var builder = new UriBuilder(endpoint) { Host = advertiseHost };
        return ZLinkEndpointNotation.Normalize(builder.Uri.ToString());
    }

    public static bool IsWildcard(string host) =>
        string.Equals(host, "0.0.0.0", StringComparison.Ordinal)
        || string.Equals(host, "::", StringComparison.Ordinal);

    private static string FormatAuthorityHost(string host) =>
        Uri.CheckHostName(host) == UriHostNameType.IPv6 ? $"[{host}]" : host;
}
