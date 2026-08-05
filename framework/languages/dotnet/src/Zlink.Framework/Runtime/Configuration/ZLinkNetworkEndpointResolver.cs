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
            return explicitEndpoint;
        return $"tcp://{listenerBindHost ?? network.BindHost}:{port.GetValueOrDefault()}";
    }

    public static string Advertise(
        string boundEndpoint,
        string? listenerAdvertiseHost,
        string? listenerBindHost,
        ZLinkNetworkOptionsModel network)
    {
        var endpoint = new Uri(boundEndpoint, UriKind.Absolute);
        if (!string.Equals(endpoint.Scheme, "tcp", StringComparison.OrdinalIgnoreCase))
            return boundEndpoint;

        var bindHost = listenerBindHost ?? network.BindHost;
        var advertiseHost = listenerAdvertiseHost
            ?? network.AdvertiseHost
            ?? (!IsWildcard(bindHost) ? bindHost : null);
        if (advertiseHost is null)
            throw new ZLinkConfigurationException(
                "AdvertiseHost is required when BindHost is a wildcard address.");

        return new UriBuilder(endpoint) { Host = advertiseHost }.Uri
            .GetComponents(UriComponents.SchemeAndServer, UriFormat.Unescaped);
    }

    public static bool IsWildcard(string host) =>
        string.Equals(host, "0.0.0.0", StringComparison.Ordinal)
        || string.Equals(host, "::", StringComparison.Ordinal);
}
