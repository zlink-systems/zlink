using System.Text.Json;
namespace MyGame.Shared;

public sealed record NodeSettings(string Id, string RedisEndpoint, string KeyPrefix, string MeshEndpoint,
    string? StreamEndpoint = null, int Capacity = GameNames.DefaultCapacity, int RegionLimit = GameNames.DefaultRegionLimit)
{
    public static NodeSettings Load(string[] args)
    {
        var index = Array.IndexOf(args, "--config");
        if (index < 0 || index + 1 >= args.Length) throw new ArgumentException("Pass --config <path>.");
        var settings = JsonSerializer.Deserialize<NodeSettings>(File.ReadAllText(args[index + 1]))
            ?? throw new InvalidDataException("Missing node settings.");
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.Id);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.RedisEndpoint);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.KeyPrefix);
        ArgumentException.ThrowIfNullOrWhiteSpace(settings.MeshEndpoint);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(settings.Capacity);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(settings.RegionLimit);
        return settings;
    }
}
