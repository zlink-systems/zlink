using Zlink.Framework.E2E.Configuration;
namespace StoreFailure.Client.Support;

internal sealed record ClientOptions(
    string ConsumerUrl,
    string ConsumerProject,
    string ConsumerNwUrl,
    string ProviderProject,
    string ProviderAUrl,
    string ProviderAEndpoint,
    string ProviderAEvidenceFile,
    string ProviderBUrl,
    string ProviderBEndpoint,
    string ProviderBEvidenceFile,
    string ProviderCUrl,
    string ProviderCEndpoint,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string RedisContainer,
    string ConfigDir,
    string LogDir,
    string Scenario,
    int LocationHeartbeatMs,
    int LocationLeaseTtlMs,
    int LocationPollingMs,
    int LocationGraceMs)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);

    public TimeSpan OwnerLeaseRenewInterval => TimeSpan.FromMilliseconds(LocationHeartbeatMs);

    public TimeSpan OwnerLeaseTtl => TimeSpan.FromMilliseconds(LocationLeaseTtlMs);

    public TimeSpan PollingInterval => TimeSpan.FromMilliseconds(LocationPollingMs);

    public TimeSpan StoreFailureGrace => TimeSpan.FromMilliseconds(LocationGraceMs);
}
