namespace LocationMessaging.Server.Provider.Configuration;

using Zlink.Framework.E2E.Configuration;

internal sealed record ServerOptions(
    string Role,
    string HttpUrl,
    string LogDir,
    string Rid,
    int Weight,
    string? EvidenceFile = null,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null,
    string? ChannelEndpoint = null,
    string? RouteEndpoint = null,
    IReadOnlyList<string>? RoutePeers = null,
    ulong ApplicationHwmBytes = 0)
{
    public static ServerOptions Parse(string[] args, string defaultRole)
        => E2eConfiguration.Load<ServerOptions>(args) with { Role = defaultRole };
}
