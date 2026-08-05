namespace LocationMessaging.Server.Workflow.Configuration;

using Zlink.Framework.E2E.Configuration;

internal sealed record ServerOptions(
    string Role,
    string HttpUrl,
    string LogDir,
    string Rid,
    string WorkflowEndpoint,
    int Weight,
    string? EvidenceFile = null,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null)
{
    public static ServerOptions Parse(string[] args, string defaultRole)
        => E2eConfiguration.Load<ServerOptions>(args) with { Role = defaultRole };
}
