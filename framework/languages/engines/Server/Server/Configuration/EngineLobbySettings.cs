using System.Text.Json;

namespace EngineLobby.Server;

public sealed record EngineLobbySettings(
    string RedisEndpoint,
    string RedisKeyPrefix,
    string MeshEndpoint,
    string StreamEndpoint,
    string HttpEndpoint
)
{
    public static EngineLobbySettings Load(string path)
    {
        var settings = JsonSerializer.Deserialize<EngineLobbySettings>(File.ReadAllText(path));
        if (settings is null)
            throw new InvalidOperationException(
                $"Could not read Engine Lobby settings from '{path}'."
            );

        foreach (
            var value in new[]
            {
                settings.RedisEndpoint,
                settings.RedisKeyPrefix,
                settings.MeshEndpoint,
                settings.StreamEndpoint,
                settings.HttpEndpoint,
            }
        )
        {
            if (string.IsNullOrWhiteSpace(value))
                throw new InvalidOperationException(
                    "Engine Lobby settings cannot contain blank values."
                );
        }

        return settings;
    }
}
