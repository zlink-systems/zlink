using LocationMessaging.Server.Consumer.Endpoints;
using LocationMessaging.Server.Consumer;
using Zlink.Framework.E2E.Configuration;
namespace LocationMessaging.Server.Consumer.Configuration;

internal sealed record ConsumerOptions(
    string HttpUrl,
    string LogDir,
    string TraceLabel,
    string? RedisEndpoint = null,
    string? RedisKeyPrefix = null,
    IReadOnlyList<string>? ProviderEndpoints = null,
    string MeshName = "profile",
    string? MeshEndpoint = null,
    string ObjectRole = "None",
    string RouteChannelRole = "Client",
    int RouteChannelWeight = 100,
    bool RegisterIndependentTopologies = false,
    bool RegisterWorkflowClient = false)
{
    public static ConsumerOptions Parse(string[] args)
        => E2eConfiguration.Load<ConsumerOptions>(args);
}
