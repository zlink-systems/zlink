namespace ObservabilityOps.Server.Play.Support;

using Zlink.Framework.E2E.Configuration;

internal sealed record PlayOptions(
    string Rid,
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string RouterEndpoint,
    string PubEndpoint,
    string LogDir,
    bool MetricsEnabled = true,
    long ApplicationVersion = 0,
    string? MaintenanceWave = null,
    int PlacementWeight = 100,
    string? ManualPeerEndpoint = null,
    int LocationHeartbeatMs = 1000,
    int LocationLeaseTtlMs = 3000)
{
    public static PlayOptions Parse(string[] args)
        => E2eConfiguration.Load<PlayOptions>(args);
}
