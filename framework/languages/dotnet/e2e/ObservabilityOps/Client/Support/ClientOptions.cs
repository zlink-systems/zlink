using Zlink.Framework.E2E.Configuration;
namespace ObservabilityOps.Client.Support;

internal sealed record ClientOptions(
    string PlayAUrl, string PlayBUrl, string PlayCUrl, string PlayDUrl,
    string SessionUrl, string SessionEndpoint,
    string WorkflowAUrl, string WorkflowBUrl, string RedisEndpoint, string Scenario, string LogDir,
    string C5Phase)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}
