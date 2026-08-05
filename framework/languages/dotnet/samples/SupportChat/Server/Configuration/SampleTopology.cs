using Microsoft.Extensions.Configuration;

namespace SupportChat.Server.Configuration;

public sealed record SampleTopology(
    string RedisEndpoint,
    string RedisKeyPrefix,
    string MeshEndpoint,
    string StreamEndpoint)
{
    public SampleSessionNode PrimarySession => new(
        MeshEndpoint,
        StreamEndpoint);

    public static SampleRuntimeConfiguration LoadApi(string[] args) => Load(args, "api");

    public static SampleRuntimeConfiguration LoadSupport(string[] args) => Load(args, "support");

    public static SampleRuntimeConfiguration LoadSession(string[] args) => Load(args, "session");

    private static SampleRuntimeConfiguration Load(string[] args, string role)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");
        var settings = new ConfigurationBuilder()
                           .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                           .Build()
                           .GetRequiredSection("Sample")
                           .Get<SampleConfiguration>()
                       ?? throw new InvalidOperationException("SupportChat Sample configuration is empty.");
        settings.Validate(role);
        var topology = new SampleTopology(
            settings.RedisEndpoint,
            settings.RedisKeyPrefix,
            settings.MeshEndpoint,
            settings.StreamEndpoint);
        return new SampleRuntimeConfiguration(topology, settings.LogDirectory);
    }
}

public sealed record SampleRuntimeConfiguration(SampleTopology Topology, string LogDirectory);

public sealed class SampleConfiguration
{
    public string LogDirectory { get; init; } = "";
    public string RedisEndpoint { get; init; } = "";
    public string RedisKeyPrefix { get; init; } = "";
    public string MeshEndpoint { get; init; } = "";
    public string StreamEndpoint { get; init; } = "";

    public void Validate(string role)
    {
        Require(LogDirectory, nameof(LogDirectory));
        Require(RedisEndpoint, nameof(RedisEndpoint));
        Require(RedisKeyPrefix, nameof(RedisKeyPrefix));
        Require(MeshEndpoint, nameof(MeshEndpoint));
        switch (role)
        {
            case "api":
                break;
            case "support":
                break;
            case "session":
                Require(StreamEndpoint, nameof(StreamEndpoint));
                break;
            default:
                throw new InvalidOperationException($"Unknown SupportChat role '{role}'.");
        }
    }

    private static void Require(string value, string name)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"SupportChat Sample.{name} is required.");
    }
}

public sealed record SampleSessionNode(
    string MeshEndpoint,
    string StreamEndpoint);
