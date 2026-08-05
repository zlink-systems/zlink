namespace RuntimeMonitoring.Server.Service.Support;

using Zlink.Framework.E2E.Configuration;

internal sealed record ServerOptions(
    string Role,
    string HttpUrl,
    string LogDir,
    string Rid,
    string? EvidenceFile = null,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null,
    string? ChannelEndpoint = null,
    string? SpotRouterEndpoint = null,
    string? SpotPubEndpoint = null)
{
    public static ServerOptions Parse(string[] args, string defaultRole)
        => E2eConfiguration.Load<ServerOptions>(args) with { Role = defaultRole };
}
