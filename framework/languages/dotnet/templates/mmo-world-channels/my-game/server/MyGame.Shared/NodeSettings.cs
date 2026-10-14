using System.Text.Json;

namespace MyGame.Shared;

public sealed record NodeSettings(
    string Id,
    string RedisEndpoint,
    string KeyPrefix,
    string MeshEndpoint,
    string WorldId,
    string? StreamEndpoint = null,
    string[]? ZoneIds = null,
    int ZoneCapacity = 0,
    string? HttpEndpoint = null,
    WorldInfo[]? Worlds = null)
{
    public string MeshName => GameNames.Mesh(WorldId);
    public static NodeSettings Load(string[] args)
    {
        var index = Array.IndexOf(args, "--config");
        if (index < 0 || index + 1 >= args.Length)
            throw new ArgumentException("Pass --config <path>.");
        var settings = JsonSerializer.Deserialize<NodeSettings>(File.ReadAllText(args[index + 1]))
            ?? throw new InvalidDataException("Node configuration is empty.");
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.Id);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.RedisEndpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.KeyPrefix);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.MeshEndpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.WorldId);
        return settings;
    }
}
