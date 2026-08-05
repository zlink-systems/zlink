using Zlink.Framework.E2E.Configuration;
namespace RuntimeMonitoring.Client.Support;

internal sealed record ClientOptions(
    string RedisEndpoint,
    string RedisKeyPrefix,
    string RedisContainer,
    string ServiceUrl,
    string ServiceChannelEndpoint,
    string ServiceBUrl,
    int ServiceBProcessId,
    string ServiceBChannelEndpoint,
    string ServiceBSpotRouterEndpoint,
    string ServiceBSpotPubEndpoint,
    string FilteredServiceUrl,
    string FilteredChannelEndpoint,
    string ThrowServiceUrl,
    string ThrowChannelEndpoint,
    string FilteredServiceProject,
    string ServiceProject,
    string ValidationHostProject,
    string ConfigDir,
    string Scenario,
    string LogDir)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}
