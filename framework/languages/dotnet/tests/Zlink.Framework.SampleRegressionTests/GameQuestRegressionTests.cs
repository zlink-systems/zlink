using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void GameQuest_Uses_One_Physical_Mesh_And_Instance_Spot_Owners()
    {
        var sampleRoot = ResolveSampleRoot("GameQuest");
        var hosts = new[]
        {
            Path.Combine(sampleRoot, "Server", "GameApi", "Program.cs"),
            Path.Combine(sampleRoot, "Server", "QuestMission", "Program.cs")
        };

        foreach (var host in hosts)
        {
            var source = File.ReadAllText(host);
            Assert.Equal(1, source.Split("AddRouteMesh(", StringSplitOptions.None).Length - 1);
            Assert.Contains("AddRouteMesh(SampleNames.MeshName)", source, StringComparison.Ordinal);
            Assert.Contains("AddHandlersFromAssemblyOf", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddRequestHandler<", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddSendHandler<", source, StringComparison.Ordinal);
            Assert.DoesNotContain(".Channel(", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddClientServerChannel(", source, StringComparison.Ordinal);
        }

        Assert.Contains("AddInstanceSpotFactory<PlayerQuestSpot>", File.ReadAllText(hosts[1]),
            StringComparison.Ordinal);
    }

    [Fact]
    public void GameQuest_Runner_Uses_Isolated_Docker_Redis_And_Stream_Actions()
    {
        var sampleRoot = ResolveSampleRoot("GameQuest");
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));
        var messages = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Messages.cs"));
        var clientScenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "GameQuestClientScenario.cs"));
        var sessionHandlers = File.ReadAllText(Path.Combine(sampleRoot, "Server", "GameApi", "Session",
            "GameQuestSessionHandlers.cs"));
        var session = File.ReadAllText(Path.Combine(sampleRoot, "Server", "GameApi", "Session",
            "GameQuestSession.cs"));
        var playerSessionActor = File.ReadAllText(Path.Combine(sampleRoot, "Server", "GameApi", "Session",
            "PlayerSessionActor.cs"));
        var topology = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "SampleConfiguration.cs"));
        var gameApiStore = File.ReadAllText(Path.Combine(sampleRoot, "Server", "GameApi", "Infrastructure", "Store",
            "GameQuestStores.cs"));
        var questStore = File.ReadAllText(Path.Combine(sampleRoot, "Server", "QuestMission", "Infrastructure", "Store",
            "QuestStores.cs"));
        var questProcessor = File.ReadAllText(Path.Combine(sampleRoot, "Server", "QuestMission", "Application",
            "QuestEventProcessor.cs"));
        var redisJsonStore = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "RedisJsonStore.cs"));
        var gameApiProgram = File.ReadAllText(Path.Combine(sampleRoot, "Server", "GameApi", "Program.cs"));
        var missionProgram = File.ReadAllText(Path.Combine(sampleRoot, "Server", "QuestMission", "Program.cs"));
        var gameplayIngress = missionProgram;
        var playerQuestSpot = File.ReadAllText(Path.Combine(sampleRoot, "Server", "QuestMission", "Infrastructure",
            "ZLink", "Spots", "PlayerQuestSpot", "PlayerQuestSpot.cs"));
        var questDomain = File.ReadAllText(Path.Combine(sampleRoot, "Server", "QuestMission", "Domain",
            "QuestDomain.cs"));
        var actionService = File.ReadAllText(Path.Combine(sampleRoot, "Server", "GameApi", "Application",
            "GameplayActionService.cs"));
        var eventDispatcher = File.ReadAllText(Path.Combine(sampleRoot, "Server", "GameApi", "Infrastructure",
            "ZLink", "GameplayEventOwnerDispatcher.cs"));
        var progressSynchronizer = File.ReadAllText(Path.Combine(sampleRoot, "Server", "GameApi",
            "Infrastructure", "Http", "HttpQuestProgressSynchronizer.cs"));
        var progressNotifier = File.ReadAllText(Path.Combine(sampleRoot, "Server", "QuestMission",
            "Infrastructure", "ZLink", "GameApiQuestClients.cs"));
        var readme = File.ReadAllText(Path.Combine(sampleRoot, "README.ko.md"));

        Assert.Contains("RUN_ID=\"$(basename \"${RUN_DIR}\")-$$-${RANDOM}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("SAMPLE_LOG_DIR=\"${RUN_DIR}/sample-logs\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("GAMEQUEST_LOG_DIR=\"${SAMPLE_LOG_DIR}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("GAMEQUEST_REDIS_KEY_PREFIX=\"gamequest:dotnet:${RUN_ID}:\"", shellRunner,
            StringComparison.Ordinal);
        Assert.Contains("REDIS_CONTAINER=\"zlink-gamequest-dotnet-redis-${RUN_ID}\"", shellRunner,
            StringComparison.Ordinal);
        AssertShellRunnerUsesRedisDockerHelper(
            shellRunner,
            "zlink-gamequest-dotnet-redis",
            "GAMEQUEST_REDIS_ENDPOINT");
        Assert.DoesNotContain("GAMEQUEST_BASE_PORT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${GAMEQUEST_GAMEAPI_A_HTTP_BASE_URL:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${GAMEQUEST_REDIS_KEY_PREFIX:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("GAMEQUEST_STORE_DIR", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("GAMEQUEST_STARTUP_DELAY_SECONDS", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", shellRunner, StringComparison.OrdinalIgnoreCase);

        Assert.Contains("$RunId = \"$PID-$([Guid]::NewGuid().ToString('N'))\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$SampleLogDir = Join-Path $RunDir \"sample-logs\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$ports = New-SamplePorts -Count 10 -BasePort 0", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$GAMEQUEST_REDIS_KEY_PREFIX = \"gamequest:dotnet:${RunId}:\"", powershellRunner,
            StringComparison.Ordinal);
        AssertPowerShellRunnerUsesRedisDockerHelper(powershellRunner, "zlink-gamequest-dotnet-redis");
        Assert.DoesNotContain("Set-DefaultEnv", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("GAMEQUEST_BASE_PORT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("GAMEQUEST_STORE_DIR", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("GAMEQUEST_STARTUP_DELAY_SECONDS", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", powershellRunner, StringComparison.OrdinalIgnoreCase);

        Assert.Contains("record JoinSessionReq", messages, StringComparison.Ordinal);
        Assert.Contains("record JoinSessionRes", messages, StringComparison.Ordinal);
        Assert.Contains("record JoinSessionRes(string PlayerId, QuestProgress[] ActiveQuests)", messages,
            StringComparison.Ordinal);
        Assert.Contains("joined.PlayerId == \"player-alice\"", clientScenario, StringComparison.Ordinal);
        Assert.Contains("bobJoined.PlayerId == \"player-bob\"", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("SubscribeQuestReq", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("SubscribeQuestRes", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("ApplyGameplayEventReq", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("ApplyGameplayEventRes", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("BindQuestSession", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("UnbindQuestSession", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("TargetConnectionId", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("ApplyGameplayEventReq", topology, StringComparison.Ordinal);
        Assert.DoesNotContain("ApplyGameplayEventRes", topology, StringComparison.Ordinal);
        Assert.Contains("record GameplayMsg", messages, StringComparison.Ordinal);
        Assert.Contains("JsonElement Payload", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("byte[] Payload", messages, StringComparison.Ordinal);
        Assert.Contains("long Version", messages, StringComparison.Ordinal);
        Assert.Contains("LastSourceEventId", messages, StringComparison.Ordinal);
        Assert.Contains("apiAStream.Request(new JoinSessionReq", clientScenario, StringComparison.Ordinal);
        Assert.Contains("apiAStream.Request(new KillMonsterReq", clientScenario, StringComparison.Ordinal);
        Assert.Contains("apiAStream.Send(new EnterAreaReq", clientScenario, StringComparison.Ordinal);
        Assert.Contains("apiBStream.Send(new CollectItemReq", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("UnlockFeatureReq", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("CompleteMissionReq", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("UnlockFeatureReq", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("CompleteMissionReq", clientScenario, StringComparison.Ordinal);
        Assert.Contains("/self-check/owner/player-alice/close", clientScenario, StringComparison.Ordinal);
        Assert.Contains("SyncQuestProgressReq(\"player-alice\")", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("Post(\"/combat/kill\")", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("Post(\"/inventory/collect\")", clientScenario, StringComparison.Ordinal);
        Assert.Contains("class JoinSessionHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.Contains("JoinQuestSessionUseCase", sessionHandlers, StringComparison.Ordinal);
        Assert.DoesNotContain("SubscribeQuestHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.Contains("class KillMonsterHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.Contains("class CollectItemHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.Contains("class EnterAreaHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.DoesNotContain("class CompleteMissionHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.DoesNotContain("class UnlockFeatureHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.Contains("IZLinkEntrySpotActorRequestHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.Contains("IZLinkEntrySpotActorSendHandler", sessionHandlers, StringComparison.Ordinal);
        Assert.Contains("ZLinkSpotActorRequestHandler(nameof(KillMonsterReq))", sessionHandlers,
            StringComparison.Ordinal);
        Assert.DoesNotContain("IZLinkSessionPacketHandler<IZLinkSessionContext, KillMonsterReq>", sessionHandlers,
            StringComparison.Ordinal);
        Assert.Contains("await actor.RelayAsync(payload, cancellationToken)", session, StringComparison.Ordinal);
        Assert.Contains("BindOrGetAsync(actor, cancellationToken)", sessionHandlers, StringComparison.Ordinal);
        Assert.Contains("actor.Context.BoundSession", playerSessionActor, StringComparison.Ordinal);
        Assert.Contains("ZLinkSpotActorSendHandler(nameof(QuestProgressNotify))", playerSessionActor,
            StringComparison.Ordinal);
        Assert.DoesNotContain("BindSessionAsync", gameApiStore, StringComparison.Ordinal);
        Assert.DoesNotContain("ReadBindingHistoryAsync", gameApiStore, StringComparison.Ordinal);

        Assert.DoesNotContain("MapPost(\"/combat/kill\"", gameApiProgram, StringComparison.Ordinal);
        Assert.DoesNotContain("MapPost(\"/inventory/collect\"", gameApiProgram, StringComparison.Ordinal);
        Assert.DoesNotContain("MapPost(\"/mission/complete\"", gameApiProgram, StringComparison.Ordinal);
        Assert.DoesNotContain("MapPost(\"/world/enter\"", gameApiProgram, StringComparison.Ordinal);
        Assert.DoesNotContain("MapPost(\"/feature/unlock\"", gameApiProgram, StringComparison.Ordinal);
        Assert.DoesNotContain("AddFanoutChannel", gameApiProgram, StringComparison.Ordinal);
        Assert.DoesNotContain("AddFanoutChannel", missionProgram, StringComparison.Ordinal);

        Assert.DoesNotContain("StoreDirectory", topology, StringComparison.Ordinal);
        Assert.DoesNotContain("GAMEQUEST_STORE_DIR", topology, StringComparison.Ordinal);
        Assert.Contains(
            "PlayerQuestSpotType = \"gamequest.player-quest\"",
            topology,
            StringComparison.Ordinal);
        Assert.Contains("new RedisJsonStore(topology.RedisEndpoint)", gameApiStore, StringComparison.Ordinal);
        Assert.Contains("new RedisJsonStore(topology.RedisEndpoint)", questStore, StringComparison.Ordinal);
        Assert.Contains("topology.RedisKeyPrefix", gameApiStore, StringComparison.Ordinal);
        Assert.Contains("topology.RedisKeyPrefix", questStore, StringComparison.Ordinal);
        Assert.Contains("_database.LockTakeAsync", redisJsonStore, StringComparison.Ordinal);
        Assert.DoesNotContain("_database.LockTakeAsync", gameApiStore, StringComparison.Ordinal);
        Assert.DoesNotContain("_database.LockTakeAsync", questStore, StringComparison.Ordinal);
        Assert.DoesNotContain("File.ReadAllText", gameApiStore, StringComparison.Ordinal);
        Assert.DoesNotContain("File.WriteAllText", gameApiStore, StringComparison.Ordinal);
        Assert.DoesNotContain("File.ReadAllText", questStore, StringComparison.Ordinal);
        Assert.DoesNotContain("File.WriteAllText", questStore, StringComparison.Ordinal);
        Assert.DoesNotContain("MapPost(\"/internal/apply\"", missionProgram, StringComparison.Ordinal);
        Assert.Contains("AddRouteMesh(SampleNames.MeshName)", missionProgram,
            StringComparison.Ordinal);
        Assert.Contains("AddInstanceSpotFactory<PlayerQuestSpot>", missionProgram,
            StringComparison.Ordinal);
        Assert.Contains("IGameplayEventOwnerDispatcher", actionService, StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotClient", eventDispatcher, StringComparison.Ordinal);
        Assert.Contains("SendToSpot(gameplayEvent.PlayerId", eventDispatcher, StringComparison.Ordinal);
        Assert.Contains(".InstanceSpot(SampleNames.PlayerQuestSpotType)", eventDispatcher,
            StringComparison.Ordinal);
        Assert.Contains(".InMesh(SampleNames.MeshName)", eventDispatcher, StringComparison.Ordinal);
        Assert.Contains("new GameplayMsg", eventDispatcher, StringComparison.Ordinal);
        Assert.DoesNotContain("ZLinkHttpClient", eventDispatcher, StringComparison.Ordinal);
        Assert.DoesNotContain("interface IPlayerQuestOwnerProvisioner", questProcessor, StringComparison.Ordinal);
        Assert.DoesNotContain("IPlayerQuestOwnerProvisioner", gameplayIngress, StringComparison.Ordinal);
        Assert.DoesNotContain("ValueTask EnsureAsync", questProcessor, StringComparison.Ordinal);
        Assert.DoesNotContain("GetOrCreate(", eventDispatcher, StringComparison.Ordinal);
        Assert.DoesNotContain("IZLinkSpotManager", eventDispatcher, StringComparison.Ordinal);
        Assert.Contains("RequestToSpot(playerId", progressSynchronizer, StringComparison.Ordinal);
        Assert.Contains(".InstanceSpot(SampleNames.PlayerQuestSpotType)", progressSynchronizer,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkActorClient", progressNotifier, StringComparison.Ordinal);
        Assert.Contains("SendToActor(playerId", progressNotifier, StringComparison.Ordinal);
        Assert.DoesNotContain("SendToChannel", progressNotifier, StringComparison.Ordinal);
        Assert.Contains("IZLinkInstanceSpot", playerQuestSpot, StringComparison.Ordinal);
        Assert.Contains("IZLinkInstanceSpotContext", playerQuestSpot, StringComparison.Ordinal);
        Assert.DoesNotContain("OnCreateAsync", playerQuestSpot, StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotPacketHandler<PlayerQuestSpot, GameplayMsg>",
            playerQuestSpot, StringComparison.Ordinal);
        Assert.Contains("QuestContractMapper.ToDomain(message)", playerQuestSpot,
            StringComparison.Ordinal);
        Assert.Contains("ReadQuestStreamAsync(\n            gameplayFact.PlayerId,\n            definition.QuestId", questProcessor,
            StringComparison.Ordinal);
        Assert.Contains("Rehydrate(\n        QuestDefinition definition,\n        IReadOnlyList<QuestDomainEvent> stream)", questDomain,
            StringComparison.Ordinal);
        Assert.Contains("public QuestProgressDecision? Decide(GameplayFact gameplayFact)", questDomain,
            StringComparison.Ordinal);
        Assert.Contains("RecordOwnerRehydratedAsync", playerQuestSpot, StringComparison.Ordinal);
        Assert.Contains("SendToSpot(playerId, new ClosePlayerQuestMsg())", missionProgram,
            StringComparison.Ordinal);
        Assert.DoesNotContain("spots.FindAsync", missionProgram, StringComparison.Ordinal);
        Assert.Contains("Context.CloseAsync(cancellationToken)", playerQuestSpot,
            StringComparison.Ordinal);
        Assert.Contains("ReadOwnerRehydrateEvidenceAsync", gameApiProgram, StringComparison.Ordinal);
        Assert.Contains("rehydrates.GetValueOrDefault(\"player-alice\") >= 2", gameApiProgram, StringComparison.Ordinal);
        Assert.Contains("rehydrated:{pair.Key}:{pair.Value}", gameApiProgram, StringComparison.Ordinal);

        Assert.Contains("외부 Redis endpoint", readme, StringComparison.Ordinal);
        Assert.Contains("재사용 mode는 제공하지 않는다", readme, StringComparison.Ordinal);
        Assert.Contains("같은 stream으로", readme, StringComparison.Ordinal);
    }

    [Fact]
    public void GameQuest_Domain_Does_Not_Depend_On_Transport_Or_Persistence_Contracts()
    {
        var sampleRoot = ResolveSampleRoot("GameQuest");
        var domainFiles = Directory.GetFiles(
            Path.Combine(sampleRoot, "Server"),
            "*.cs",
            SearchOption.AllDirectories)
            .Where(static path => path.Contains(
                $"{Path.DirectorySeparatorChar}Domain{Path.DirectorySeparatorChar}",
                StringComparison.Ordinal));

        foreach (var domainFile in domainFiles)
        {
            var source = File.ReadAllText(domainFile);
            Assert.DoesNotContain("using GameQuest.Shared", source, StringComparison.Ordinal);
            Assert.DoesNotContain("using System.Text.Json", source, StringComparison.Ordinal);
            Assert.DoesNotContain("using Zlink.", source, StringComparison.Ordinal);
            Assert.DoesNotContain("using Systems.Zlink", source, StringComparison.Ordinal);
            Assert.DoesNotContain("using StackExchange.Redis", source, StringComparison.Ordinal);
        }

        var mapper = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "QuestMission",
            "Infrastructure",
            "Store",
            "QuestContractMapper.cs"));
        Assert.Contains("static class QuestContractMapper", mapper, StringComparison.Ordinal);
        Assert.Contains("JsonSerializer.SerializeToElement", mapper, StringComparison.Ordinal);
        Assert.DoesNotContain("JsonSerializer.SerializeToUtf8Bytes", mapper, StringComparison.Ordinal);
        Assert.DoesNotContain("JsonDocument.Parse", mapper, StringComparison.Ordinal);
    }
}
