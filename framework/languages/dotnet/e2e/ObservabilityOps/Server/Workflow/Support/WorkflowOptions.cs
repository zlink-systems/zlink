namespace ObservabilityOps.Server.Workflow.Support;

using Zlink.Framework.E2E.Configuration;

internal sealed record WorkflowOptions(string Rid, string HttpUrl, string RedisEndpoint,
    string RedisKeyPrefix, string RouterEndpoint, string PubEndpoint, string LogDir,
    int LocationHeartbeatMs = 1000,
    int LocationLeaseTtlMs = 3000)
{
    public static WorkflowOptions Parse(string[] args)
        => E2eConfiguration.Load<WorkflowOptions>(args);
}
