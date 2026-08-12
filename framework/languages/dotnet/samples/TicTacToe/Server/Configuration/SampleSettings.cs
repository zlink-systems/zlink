using TicTacToe.Shared.Contracts;

namespace TicTacToe.Server.Configuration;

internal sealed record SampleSettings(
    string InstanceName,
    string ApiBindUrl,
    string MeshEndpoint,
    IReadOnlyList<string> PeerMeshEndpoints,
    string ApiChannelListenEndpoint,
    IReadOnlyList<string> ApiChannelPeerEndpoints,
    string PlayEndpoint,
    IReadOnlyList<string> PlayEndpoints,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string LogDirectory)
{
    public IReadOnlyList<PlayNodeInfo> PlayNodes =>
        PlayEndpoints
            .Select(static endpoint => new PlayNodeInfo(endpoint))
            .ToArray();

    public static SampleSettings LoadApi(string[] args)
    {
        var section = LoadSection(args);
        var playEndpoints = RequireList(section, nameof(PlayEndpoints), 2);
        return new SampleSettings(
            RequireString(section, nameof(InstanceName)),
            RequireString(section, nameof(ApiBindUrl)),
            RequireString(section, nameof(MeshEndpoint)),
            RequireList(section, nameof(PeerMeshEndpoints), 2),
            RequireTcpEndpoint(section, nameof(ApiChannelListenEndpoint)),
            [],
            string.Empty,
            playEndpoints,
            RequireString(section, nameof(RedisEndpoint)),
            RequireString(section, nameof(RedisKeyPrefix)),
            RequireString(section, nameof(LogDirectory)));
    }

    public static SampleSettings LoadPlay(string[] args)
    {
        var section = LoadSection(args);
        return new SampleSettings(
            RequireString(section, nameof(InstanceName)),
            string.Empty,
            RequireString(section, nameof(MeshEndpoint)),
            ReadList(section, nameof(PeerMeshEndpoints)),
            string.Empty,
            RequireTcpEndpointList(section, nameof(ApiChannelPeerEndpoints), 2),
            RequireString(section, nameof(PlayEndpoint)),
            RequireList(section, nameof(PlayEndpoints), 2),
            RequireString(section, nameof(RedisEndpoint)),
            RequireString(section, nameof(RedisKeyPrefix)),
            RequireString(section, nameof(LogDirectory)));
    }

    private static IConfigurationSection LoadSection(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config" || string.IsNullOrWhiteSpace(args[1]))
            throw new ArgumentException("Usage: --config PATH");

        return new ConfigurationBuilder()
            .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
            .Build()
            .GetRequiredSection("Sample");
    }

    private static string RequireString(IConfigurationSection section, string name)
    {
        var value = section[name];
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"Sample.{name} is required.")
            : value;
    }

    private static IReadOnlyList<string> RequireList(
        IConfigurationSection section,
        string name,
        int count)
    {
        var values = ReadList(section, name);
        return values.Count == count
            ? values
            : throw new InvalidOperationException($"Sample.{name} must contain {count} values.");
    }

    private static string RequireTcpEndpoint(
        IConfigurationSection section,
        string name)
    {
        var endpoint = RequireString(section, name);
        return IsTcpEndpoint(endpoint)
            ? endpoint
            : throw new InvalidOperationException(
                $"Sample.{name} must be an absolute tcp endpoint with an explicit port.");
    }

    private static IReadOnlyList<string> RequireTcpEndpointList(
        IConfigurationSection section,
        string name,
        int count)
    {
        var endpoints = RequireList(section, name, count);
        return endpoints.All(IsTcpEndpoint)
            ? endpoints
            : throw new InvalidOperationException(
                $"Sample.{name} must contain absolute tcp endpoints with explicit ports.");
    }

    private static bool IsTcpEndpoint(string endpoint)
    {
        return Uri.TryCreate(endpoint, UriKind.Absolute, out var uri)
               && string.Equals(uri.Scheme, "tcp", StringComparison.OrdinalIgnoreCase)
               && !string.IsNullOrWhiteSpace(uri.Host)
               && uri.Port is > 0 and <= 65535;
    }

    private static IReadOnlyList<string> ReadList(
        IConfigurationSection section,
        string name)
    {
        return section.GetSection(name)
            .GetChildren()
            .Select(static child => child.Value)
            .Where(static value => !string.IsNullOrWhiteSpace(value))
            .Select(static value => value!)
            .ToArray();
    }
}
