using Zlink.Framework.E2E.Configuration;

namespace ChannelEgressRouting.Server;

public sealed record RoleOptions(
    string Role,
    string Rid,
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string EvidenceFile,
    string? StreamEndpoint = null,
    string[]? RouteServers = null,
    string[]? RouteClients = null,
    bool WorkflowClient = false,
    bool WorkflowServer = false,
    int WorkflowWeight = 100,
    string? WorkflowEndpoint = null,
    string? RouteEndpoint = null,
    string? RouteAdvertiseHost = null,
    string? InvalidMode = null)
{
    public static RoleOptions Parse(string[] args) =>
        E2eConfiguration.Load<RoleOptions>(args);
}
