using Zlink.Framework.E2E.Configuration;
namespace ResilienceLifecycle.Client.Support;

internal sealed record ClientOptions(
    string ConsumerUrl,
    string TopologyUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string RedisContainer,
    string ProviderAUrl,
    int ProviderAProcessId,
    string ProviderAEndpoint,
    string ProviderAEvidenceFile,
    string ProviderBUrl,
    int ProviderBProcessId,
    string ProviderBEndpoint,
    string ProviderBEvidenceFile,
    string ProviderBRemapUrl,
    string ProviderBRemapEndpoint,
    string ProviderBGreenUrl,
    string ProviderBGreenEndpoint,
    string ProviderProject,
    string ConfigDir,
    string LogDir,
    string Scenario,
    int ConsumerProcessId = 0,
    string? RouteProxyControlUrl = null,
    string? ClientServerProxyControlUrl = null,
    string? ConsumerRouteProxyControlUrl = null,
    string? ProviderAClientServerEndpoint = null,
    string? ProviderBClientServerEndpoint = null,
    string? ProviderBClientServerRestartEndpoint = null)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}
