using Microsoft.Extensions.Configuration;

namespace Bingo.Server.Configuration;

public static class SampleConfigurationLoader
{
    public static SampleRuntimeConfiguration<SampleApiNode> LoadApi(string[] args)
    {
        var settings = Load(args);
        settings.ValidateCommon();
        return settings.ToRuntime(new SampleApiNode(
            settings.Require(nameof(settings.MeshEndpoint), settings.MeshEndpoint),
            settings.Require(
                nameof(settings.MatchmakingMeshEndpoint),
                settings.MatchmakingMeshEndpoint)));
    }

    public static SampleRuntimeConfiguration<SampleMatchmakingNode> LoadMatchmaking(string[] args)
    {
        var settings = Load(args);
        settings.ValidateCommon(allowSingleNodeName: true);
        return settings.ToRuntime(new SampleMatchmakingNode(
            settings.Require(nameof(settings.MeshEndpoint), settings.MeshEndpoint)));
    }

    public static SampleRuntimeConfiguration<SamplePlayNode> LoadPlay(string[] args)
    {
        var settings = Load(args);
        settings.ValidateCommon();
        return settings.ToRuntime(new SamplePlayNode(
            settings.Require(nameof(settings.MeshEndpoint), settings.MeshEndpoint)));
    }

    public static SampleRuntimeConfiguration<SampleSessionNode> LoadSession(string[] args)
    {
        var settings = Load(args);
        settings.ValidateCommon();
        return settings.ToRuntime(new SampleSessionNode(
            settings.Require(nameof(settings.MeshEndpoint), settings.MeshEndpoint),
            settings.Require(nameof(settings.StreamEndpoint), settings.StreamEndpoint)));
    }

    private static SampleConfiguration Load(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");

        return new ConfigurationBuilder()
                   .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                   .Build()
                   .GetRequiredSection("Sample")
                   .Get<SampleConfiguration>()
               ?? throw new InvalidOperationException("Bingo Sample configuration is empty.");
    }
}

public sealed record SampleRuntimeConfiguration<TNode>(
    TNode Node,
    string NodeName,
    string LogDirectory,
    string RedisEndpoint,
    string RedisKeyPrefix);

public sealed class SampleConfiguration
{
    public string NodeName { get; init; } = "";
    public string LogDirectory { get; init; } = "";
    public string RedisEndpoint { get; init; } = "";
    public string RedisKeyPrefix { get; init; } = "";
    public string MeshEndpoint { get; init; } = "";
    public string MatchmakingMeshEndpoint { get; init; } = "";
    public string StreamEndpoint { get; init; } = "";

    public void ValidateCommon(bool allowSingleNodeName = false)
    {
        Require(nameof(NodeName), NodeName);
        Require(nameof(LogDirectory), LogDirectory);
        Require(nameof(RedisEndpoint), RedisEndpoint);
        Require(nameof(RedisKeyPrefix), RedisKeyPrefix);
        if (!allowSingleNodeName && NodeName is not ("a" or "b"))
            throw new InvalidOperationException("Bingo Sample.NodeName must be 'a' or 'b'.");
    }

    public string Require(string name, string value)
    {
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidOperationException($"Bingo Sample.{name} is required.")
            : value;
    }

    public SampleRuntimeConfiguration<TNode> ToRuntime<TNode>(TNode node)
    {
        return new SampleRuntimeConfiguration<TNode>(
            node,
            NodeName,
            LogDirectory,
            RedisEndpoint,
            RedisKeyPrefix);
    }
}

public sealed record SampleApiNode(
    string PlayMeshEndpoint,
    string MatchmakingMeshEndpoint);

public sealed record SampleMatchmakingNode(string MeshEndpoint);

public sealed record SamplePlayNode(string MeshEndpoint);

public sealed record SampleSessionNode(
    string MeshEndpoint,
    string StreamEndpoint);
