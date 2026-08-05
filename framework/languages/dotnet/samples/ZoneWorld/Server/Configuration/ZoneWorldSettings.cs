using Microsoft.Extensions.Configuration;
namespace ZoneWorld.Server.Configuration;

public sealed record ZoneWorldSettings(
    string RedisEndpoint,
    string RedisKeyPrefix,
    string LogDirectory,
    string BroadcastEndpoint);

public sealed record ZoneNodeSettings(
    string NodeId,
    string MeshEndpoint,
    string? FaultTickZone = null,
    bool DisableBots = false,
    bool SubscriberOnly = false);

public sealed record GatewaySettings(
    string StreamEndpoint,
    string MeshEndpoint);

public sealed record OpsSettings(
    string StreamEndpoint,
    string BroadcastEndpoint,
    string MeshEndpoint);

public sealed record ZoneWorldClientSettings(
    string GatewayEndpoint,
    string OpsEndpoint,
    string Scenarios = "all",
    bool StreamTrace = false);

public sealed record ZoneWorldConfiguration(
    ZoneWorldSettings Shared,
    ZoneNodeSettings? ZoneNode = null,
    GatewaySettings? Gateway = null,
    OpsSettings? Ops = null,
    ZoneWorldClientSettings? Client = null)
{
    public static ZoneWorldConfiguration Load(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");

        var configuration = new ConfigurationBuilder()
            .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
            .Build()
            .Get<ZoneWorldConfiguration>()
            ?? throw new InvalidOperationException("ZoneWorld configuration is empty.");
        configuration.Validate();
        return configuration;
    }

    private void Validate()
    {
        Required(Shared.RedisEndpoint, nameof(Shared.RedisEndpoint));
        Required(Shared.RedisKeyPrefix, nameof(Shared.RedisKeyPrefix));
        Required(Shared.LogDirectory, nameof(Shared.LogDirectory));
        Required(Shared.BroadcastEndpoint, nameof(Shared.BroadcastEndpoint));

        var roles = new object?[] { ZoneNode, Gateway, Ops, Client }.Count(value => value is not null);
        if (roles != 1)
            throw new InvalidOperationException("Exactly one ZoneWorld role must be configured.");
    }

    private static void Required(string value, string name)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"ZoneWorld configuration value '{name}' is required.");
    }
}
