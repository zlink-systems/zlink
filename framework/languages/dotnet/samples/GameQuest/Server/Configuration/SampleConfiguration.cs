using GameQuest.Shared;
using Microsoft.Extensions.Configuration;

namespace GameQuest.Server.Configuration;

public static class SampleNames
{
    public const string MeshName = "gamequest";
    public const string SessionActorType = "gamequest.session.actor";
    public const string PlayerQuestSpotType = "gamequest.player-quest";
    public const string StreamNode = "gamequest.stream";
    public const string ProgressPacket = nameof(QuestProgressNotify);
    public const string CompletedPacket = nameof(QuestCompletedNotify);

    public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(10);

}

public static class QuestIds
{
    public const string FirstHunt = "first-hunt";
    public const string HerbGathering = "herb-gathering";
    public const string ClearTutorial = "clear-tutorial";
    public const string VisitRuins = "visit-ruins";
}

public static class QuestStatuses
{
    public const string Active = "Active";
    public const string Completed = "Completed";
    public const string RewardGranted = "RewardGranted";
}

public sealed record GameQuestTopology(
    string RedisEndpoint,
    string RedisKeyPrefix,
    string GameApiAHttpBaseUrl,
    string GameApiBHttpBaseUrl,
    string MissionAHttpBaseUrl,
    string MissionBHttpBaseUrl,
    string GameApiAMeshEndpoint,
    string GameApiBMeshEndpoint,
    string MissionAMeshEndpoint,
    string MissionBMeshEndpoint)
{
    public static GameQuestRuntimeConfiguration LoadGameApi(string[] args) => Load(args, "api");

    public static GameQuestRuntimeConfiguration LoadQuestMission(string[] args) => Load(args, "mission");

    private static GameQuestRuntimeConfiguration Load(string[] args, string role)
    {
        if (args.Length != 2 || args[0] != "--config")
            throw new ArgumentException("Usage: --config PATH");
        var settings = new ConfigurationBuilder()
                           .AddJsonFile(Path.GetFullPath(args[1]), optional: false, reloadOnChange: false)
                           .Build()
                           .GetRequiredSection("Sample")
                           .Get<GameQuestConfiguration>()
                       ?? throw new InvalidOperationException("GameQuest Sample configuration is empty.");
        settings.Validate(role);
        var topology = new GameQuestTopology(
            settings.RedisEndpoint,
            settings.RedisKeyPrefix,
            settings.GameApiAHttpBaseUrl,
            settings.GameApiBHttpBaseUrl,
            settings.MissionAHttpBaseUrl,
            settings.MissionBHttpBaseUrl,
            settings.GameApiAMeshEndpoint,
            settings.GameApiBMeshEndpoint,
            settings.MissionAMeshEndpoint,
            settings.MissionBMeshEndpoint);
        var streamBindEndpoint = string.Equals(settings.InstanceName, "api-b", StringComparison.Ordinal)
            ? settings.GameApiBStreamBindEndpoint
            : settings.GameApiAStreamBindEndpoint;
        return new GameQuestRuntimeConfiguration(
            topology,
            settings.InstanceName,
            settings.LogDirectory,
            streamBindEndpoint);
    }

    public QuestMissionInstanceTopology ForQuestMission(string missionName)
    {
        return string.Equals(missionName, "mission-b", StringComparison.Ordinal)
            ? new QuestMissionInstanceTopology(missionName, MissionBMeshEndpoint)
            : new QuestMissionInstanceTopology("mission-a", MissionAMeshEndpoint);
    }

    public string MissionHttpBaseUrl(string missionName) =>
        string.Equals(missionName, "mission-b", StringComparison.Ordinal)
            ? MissionBHttpBaseUrl
            : MissionAHttpBaseUrl;

    public string GameApiMeshEndpoint(string apiName) =>
        string.Equals(apiName, "api-b", StringComparison.Ordinal)
            ? GameApiBMeshEndpoint
            : GameApiAMeshEndpoint;

    public string MissionMeshEndpoint(string missionName) =>
        string.Equals(missionName, "mission-b", StringComparison.Ordinal)
            ? MissionBMeshEndpoint
            : MissionAMeshEndpoint;
}

public sealed record GameQuestRuntimeConfiguration(
    GameQuestTopology Topology,
    string InstanceName,
    string LogDirectory,
    string StreamBindEndpoint);

public sealed class GameQuestConfiguration
{
    public string InstanceName { get; init; } = "";
    public string LogDirectory { get; init; } = "";
    public string RedisEndpoint { get; init; } = "";
    public string RedisKeyPrefix { get; init; } = "";
    public string GameApiAHttpBaseUrl { get; init; } = "";
    public string GameApiBHttpBaseUrl { get; init; } = "";
    public string MissionAHttpBaseUrl { get; init; } = "";
    public string MissionBHttpBaseUrl { get; init; } = "";
    public string GameApiAStreamBindEndpoint { get; init; } = "";
    public string GameApiBStreamBindEndpoint { get; init; } = "";
    public string GameApiAMeshEndpoint { get; init; } = "";
    public string GameApiBMeshEndpoint { get; init; } = "";
    public string MissionAMeshEndpoint { get; init; } = "";
    public string MissionBMeshEndpoint { get; init; } = "";

    public void Validate(string role)
    {
        Require(InstanceName, nameof(InstanceName));
        Require(LogDirectory, nameof(LogDirectory));
        Require(RedisEndpoint, nameof(RedisEndpoint));
        Require(RedisKeyPrefix, nameof(RedisKeyPrefix));
        var isB = InstanceName.EndsWith("-b", StringComparison.Ordinal);
        if (role == "api")
        {
            Require(isB ? GameApiBHttpBaseUrl : GameApiAHttpBaseUrl,
                isB ? nameof(GameApiBHttpBaseUrl) : nameof(GameApiAHttpBaseUrl));
            Require(isB ? GameApiBStreamBindEndpoint : GameApiAStreamBindEndpoint,
                isB ? nameof(GameApiBStreamBindEndpoint) : nameof(GameApiAStreamBindEndpoint));
            Require(isB ? GameApiBMeshEndpoint : GameApiAMeshEndpoint,
                isB ? nameof(GameApiBMeshEndpoint) : nameof(GameApiAMeshEndpoint));
            return;
        }
        if (role != "mission")
            throw new InvalidOperationException($"Unknown GameQuest role '{role}'.");
        Require(GameApiAHttpBaseUrl, nameof(GameApiAHttpBaseUrl));
        Require(isB ? MissionBHttpBaseUrl : MissionAHttpBaseUrl,
            isB ? nameof(MissionBHttpBaseUrl) : nameof(MissionAHttpBaseUrl));
        Require(isB ? MissionBMeshEndpoint : MissionAMeshEndpoint,
            isB ? nameof(MissionBMeshEndpoint) : nameof(MissionAMeshEndpoint));
    }

    private static void Require(string value, string name)
    {
        if (string.IsNullOrWhiteSpace(value))
            throw new InvalidOperationException($"GameQuest Sample.{name} is required.");
    }
}

public sealed record QuestMissionInstanceTopology(
    string MissionName,
    string MeshEndpoint);
