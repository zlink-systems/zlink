namespace GameQuest.Client.Configuration;

using Microsoft.Extensions.Configuration;

public static class SampleNames
{
    public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(10);
}

public static class QuestIds
{
    public const string FirstHunt = "first-hunt";
    public const string HerbGathering = "herb-gathering";
    public const string VisitRuins = "visit-ruins";
}

public static class QuestStatuses
{
    public const string RewardGranted = "RewardGranted";
}

public sealed record GameQuestTopology(
    string GameApiAHttpBaseUrl,
    string GameApiBHttpBaseUrl,
    string MissionAHttpBaseUrl,
    string MissionBHttpBaseUrl,
    string GameApiAStreamEndpoint,
    string GameApiBStreamEndpoint)
{
    public static GameQuestTopology Load(string[] args)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");

        var topology = new ConfigurationBuilder()
                           .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                           .Build()
                           .GetRequiredSection("Client")
                           .Get<GameQuestTopology>()
                       ?? throw new InvalidOperationException("GameQuest client configuration is empty.");
        foreach (var property in typeof(GameQuestTopology).GetProperties())
            if (string.IsNullOrWhiteSpace((string?)property.GetValue(topology)))
                throw new InvalidOperationException($"Client.{property.Name} is required.");
        return topology;
    }
}
