using System.Text.Json;

namespace MyGame.Shared;

public sealed record NodeSettings(
    string Id,
    string RedisEndpoint,
    string KeyPrefix,
    string MeshEndpoint,
    string? StreamEndpoint = null,
    string[]? ZoneIds = null,
    int ZoneCapacity = 0,
    bool Bootstrap = true
)
{
    public static NodeSettings Load(string[] args)
    {
        var config = args.FirstOrDefault(arg =>
            arg.StartsWith("--config=", StringComparison.Ordinal)
        )?["--config=".Length..];
        var index = Array.IndexOf(args, "--config");
        if (config is null && index >= 0 && index + 1 < args.Length)
            config = args[index + 1];
        if (string.IsNullOrWhiteSpace(config))
            throw new ArgumentException("Pass --config <path>.");
        var settings =
            JsonSerializer.Deserialize<NodeSettings>(
                File.ReadAllText(config),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true }
            ) ?? throw new InvalidDataException("Node configuration is empty.");
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.Id);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.RedisEndpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.KeyPrefix);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.MeshEndpoint);
        return settings;
    }
}
