using Zlink.Framework.E2E.Configuration;
namespace LocationMessaging.Client.Support;

internal sealed record ClientOptions(
    string ProviderAUrl,
    string ProviderBUrl,
    string WorkflowUrl,
    string DirectConsumerUrl,
    string SingleConsumerUrl,
    string StoreConsumerUrl,
    string BackpressureConsumerUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string ProviderProject,
    string ConsumerProject,
    string WorkflowProject,
    string ConfigDir,
    string LogDir,
    string Scenario)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}
