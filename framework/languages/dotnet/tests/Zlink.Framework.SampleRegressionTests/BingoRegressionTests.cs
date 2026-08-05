using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void Bingo_Separates_Matchmaking_Play_And_Api_Channel_Topologies()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var hosts = new[]
        {
            Path.Combine(sampleRoot, "Server", "Api", "ApiServerHostFactory.cs"),
            Path.Combine(sampleRoot, "Server", "Matchmaking", "MatchmakingServerHostFactory.cs"),
            Path.Combine(sampleRoot, "Server", "Play", "PlayServerHostFactory.cs"),
            Path.Combine(sampleRoot, "Server", "Session", "SessionServerHostFactory.cs")
        };

        var api = File.ReadAllText(hosts[0]);
        var matchmaking = File.ReadAllText(hosts[1]);
        var play = File.ReadAllText(hosts[2]);
        var names = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Configuration",
            "SampleNames.cs"));
        var matchHandler = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Api",
            "Handlers",
            "MatchBingoHandler.cs"));
        Assert.Contains("AddHandlersFromAssemblyOf", api, StringComparison.Ordinal);
        Assert.Contains("AddHandlerGroup(\"api\")", api, StringComparison.Ordinal);
        Assert.DoesNotContain("AddRequestHandler<", api, StringComparison.Ordinal);
        Assert.Contains("AddRouteMesh(SampleNames.PlayMeshName)", api, StringComparison.Ordinal);
        Assert.Contains("AddRouteMesh(SampleNames.MatchmakingMeshName)", api, StringComparison.Ordinal);
        Assert.Contains("AddClientServerChannel(SampleNames.ApiChannel)", api, StringComparison.Ordinal);
        Assert.Contains("AddInstanceSpotFactory<BingoMatchmaker>", matchmaking, StringComparison.Ordinal);
        Assert.Contains("RoomChannel = \"bingo.room\"", names, StringComparison.Ordinal);
        Assert.DoesNotContain("PlayChannel", names, StringComparison.Ordinal);
        Assert.Contains("mesh.Channel(SampleNames.RoomChannel).Server()", play, StringComparison.Ordinal);
        Assert.Contains(".InstanceSpot(SampleNames.MatchmakerSpotType)", matchHandler,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Bingo_Registers_Stateful_Actor_Relocation_Adapter()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var host = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play", "PlayServerHostFactory.cs"));
        var adapter = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play", "Infrastructure", "ZLink",
            "Actors", "PlayerActorRelocationAdapter.cs"));

        Assert.Contains("PreserveStateWith<PlayerActorRelocationAdapter>()", host,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkActorRelocationAdapter<PlayerActor>", adapter, StringComparison.Ordinal);
        Assert.Contains("ValueTask<byte[]> CaptureAsync", adapter, StringComparison.Ordinal);
        Assert.Contains("ValueTask RestoreAsync", adapter, StringComparison.Ordinal);
    }

    [Fact]
    public void Bingo_Registers_Application_Signaled_Room_Relocation_And_Join_Dedupe()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var host = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Play", "PlayServerHostFactory.cs"));
        var roomAdapter = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Play", "Infrastructure", "ZLink", "Spots",
            "BingoRoomSpot", "BingoRoomRelocationAdapter.cs"));
        var actor = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Play", "Infrastructure", "ZLink", "Actors",
            "PlayerActor.cs"));
        var actorAdapter = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Play", "Infrastructure", "ZLink", "Actors",
            "PlayerActorRelocationAdapter.cs"));

        Assert.Contains(".RelocationReadiness(", host, StringComparison.Ordinal);
        Assert.Contains("ApplicationSignaled", host, StringComparison.Ordinal);
        Assert.Contains(
            "PreserveStateWith<BingoRoomRelocationAdapter>()",
            host,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotRelocationAdapter<BingoRoom>", roomAdapter, StringComparison.Ordinal);
        Assert.Contains("LastCompletedJoinOperationId == operationId", actor, StringComparison.Ordinal);
        Assert.Contains("JoinOperationHigh", actorAdapter, StringComparison.Ordinal);
        Assert.Contains("JoinOperationLow", actorAdapter, StringComparison.Ordinal);
    }

    [Fact]
    public void Bingo_Uses_Framework_Defaults_Without_Sample_Metadata_Store()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");

        AssertNoSampleRouteStore(sampleRoot);
        AssertNoSampleMetadataStore(sampleRoot);
        AssertSessionServerUsesSessionRelay(sampleRoot);
        AssertSessionHandlersDoNotResolveActorRemoteAddresses(sampleRoot);
        AssertEnsureActorHandlersReturnSessionRelayRemoteAddresses(sampleRoot);
        AssertNoSampleSessionRelayJson(sampleRoot);
        AssertSessionPayloadPolicy(sampleRoot);
        AssertUsesAutoRegisteredSessionHandlers(sampleRoot);
    }

    [Fact]
    public void Bingo_Client_Gate_Verifies_Submitted_Cards_And_Matching_Draw_State()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var scenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "BingoClientScenario.cs"));

        Assert.Contains("client1Card.State.Players.Count == 2", scenario, StringComparison.Ordinal);
        Assert.Contains("client1Card.State.Players.All(static player => player.Card.Count == 9)", scenario,
            StringComparison.Ordinal);
        Assert.Contains("client2Drawn.Payload.State.Equals(client1Drawn.Payload.State)", scenario,
            StringComparison.Ordinal);
        Assert.Contains("connector.WaitFor<MatchBingoRes>()", scenario, StringComparison.Ordinal);
        Assert.Contains("connector.Send(new MatchBingoReq", scenario, StringComparison.Ordinal);
        Assert.Contains("connector.WaitFor<ObserveBingoEventsRes>()", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("State = new BingoRoomState()", File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Play",
            "Infrastructure",
            "ZLink",
            "Spots",
            "EntrySpot",
            "Handlers",
            "MatchBingoActorHandler.cs")), StringComparison.Ordinal);
    }

    [Fact]
    public void Bingo_Creates_A_New_Room_Through_Framework_Placement()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var handler = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Api", "Handlers",
            "MatchBingoHandler.cs"));
        var matchmaker = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Matchmaking",
            "Infrastructure", "ZLink", "BingoMatchmakerHandlers.cs"));

        Assert.Contains("RequestToSpot(", handler, StringComparison.Ordinal);
        Assert.Contains(".InstanceSpot(SampleNames.MatchmakerSpotType)", handler, StringComparison.Ordinal);
        Assert.Contains(".GetOrCreate(allocated.RoomId, SampleNames.RoomSpotType)", handler,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotRequestHandler<BingoMatchmaker", matchmaker, StringComparison.Ordinal);
        Assert.DoesNotContain("NodeRid", handler, StringComparison.Ordinal);
    }

    [Fact]
    public void Bingo_Placement_Phase_Is_Gated_By_A_Reversed_Play_Start_Order()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var runner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));

        var playBStart = runner.IndexOf(
            "start_server play-b",
            StringComparison.Ordinal);
        var playAStart = runner.IndexOf(
            "start_server play-a",
            StringComparison.Ordinal);
        var completed = runner.IndexOf(
            "echo \"bingo-placement=completed\"",
            StringComparison.Ordinal);
        var finalEvidence = runner.LastIndexOf(
            "require_log_count 2 \"bingo room: result reported",
            StringComparison.Ordinal);

        Assert.True(playBStart >= 0 && playAStart > playBStart);
        Assert.True(completed > finalEvidence);
        Assert.DoesNotContain("BINGO_NODE_RID", runner, StringComparison.Ordinal);
        Assert.DoesNotContain("TargetNodeRid", runner, StringComparison.Ordinal);
    }

    [Fact]
    public void Bingo_Player_Records_Are_Loaded_And_Reported_Through_Yielding_Room_Lifecycle()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var contracts = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Contracts", "bingo_messages.proto"));
        var room = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play", "Infrastructure", "ZLink",
            "Spots", "BingoRoomSpot", "BingoRoom.cs"));
        var apiHost = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Api", "ApiServerHostFactory.cs"));
        var scenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "BingoClientScenario.cs"));

        Assert.Contains("message GetPlayerRecordReq", contracts, StringComparison.Ordinal);
        Assert.Contains("message ReportBingoResultReq", contracts, StringComparison.Ordinal);
        Assert.Contains("int32 wins", contracts, StringComparison.Ordinal);
        Assert.Contains("int32 losses", contracts, StringComparison.Ordinal);
        Assert.Contains(".Yield<GetPlayerRecordRes>", room, StringComparison.Ordinal);
        Assert.Contains(".Yield<ReportBingoResultRes>", room, StringComparison.Ordinal);
        Assert.Contains("AddSingleton<BingoPlayerRecordStore>", apiHost, StringComparison.Ordinal);
        Assert.Contains("player.Wins == 0 && player.Losses == 0", scenario, StringComparison.Ordinal);
    }

    [Fact]
    public void Bingo_Client_Contract_Uses_Optional_Auth_Fields_And_No_Unlisted_Notifies()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var contracts = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Contracts",
            "bingo_messages.proto"));
        var playerActor = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play",
            "Infrastructure", "ZLink", "Actors", "PlayerActor.cs"));
        var entrySpot = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play",
            "Infrastructure", "ZLink", "Spots", "EntrySpot", "BingoEntrySpot.cs"));

        Assert.Contains("optional string actor_id = 2", contracts, StringComparison.Ordinal);
        Assert.Contains("optional string display_name = 3", contracts, StringComparison.Ordinal);
        Assert.Contains("optional string reason = 4", contracts, StringComparison.Ordinal);
        Assert.DoesNotContain("message BingoJoinFailedNotify", contracts, StringComparison.Ordinal);
        Assert.DoesNotContain("message BingoActorEntrySpotNotify", contracts, StringComparison.Ordinal);
        Assert.DoesNotContain("BingoJoinFailedNotify", playerActor, StringComparison.Ordinal);
        Assert.DoesNotContain("BingoActorEntrySpotNotify", entrySpot, StringComparison.Ordinal);
    }

    [Fact]
    public void Bingo_Runner_Uses_Isolated_Docker_Redis()
    {
        var sampleRoot = ResolveSampleRoot("Bingo");
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));
        var readme = File.ReadAllText(Path.Combine(sampleRoot, "README.md"));

        Assert.Contains("RUN_ID=\"$(basename \"${RUN_DIR}\")-$$-${RANDOM}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("BINGO_REDIS_KEY_PREFIX=\"bingo:dotnet:${RUN_ID}:\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("BINGO_LOG_DIR=\"${RUN_DIR}/sample-logs\"", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("BINGO_LOG_DIR=\"${SCRIPT_DIR}/logs\"", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("rm -f \"${BINGO_LOG_DIR}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("REDIS_CONTAINER=\"zlink-bingo-dotnet-redis-${RUN_ID}\"", shellRunner, StringComparison.Ordinal);
        AssertShellRunnerUsesRedisDockerHelper(shellRunner, "zlink-bingo-dotnet-redis", "BINGO_REDIS_ENDPOINT");
        Assert.DoesNotContain("if [[ -z \"${BINGO_REDIS_ENDPOINT:-}\" ]]", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("when BINGO_REDIS_ENDPOINT is not set", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("BINGO_STARTUP_DELAY_SECONDS", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("BINGO_STARTUP_SETTLE_SECONDS", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("auto-connect reconcile loops", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("intentionally derived here, not read", shellRunner, StringComparison.Ordinal);

        Assert.Contains("$RunId = \"$PID-$([Guid]::NewGuid().ToString('N'))\"", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("$BINGO_REDIS_KEY_PREFIX = \"bingo:dotnet:${RunId}:\"",
            powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("[Environment]::SetEnvironmentVariable", powershellRunner, StringComparison.Ordinal);
        AssertPowerShellRunnerUsesRedisDockerHelper(powershellRunner, "zlink-bingo-dotnet-redis");
        Assert.Contains("if ($RedisContainer)", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("if (-not $BINGO_REDIS_ENDPOINT)", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("when BINGO_REDIS_ENDPOINT is not set", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("BINGO_STARTUP_DELAY_SECONDS", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("BINGO_STARTUP_SETTLE_SECONDS", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("$BINGO_LOG_DIR = $SampleLogDir", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("$SampleLogDir = Join-Path $RunDir \"sample-logs\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Join-Path $ScriptDir \"logs\"", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("function Wait-LogContains", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", powershellRunner, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("intentionally derived here, not read", powershellRunner, StringComparison.Ordinal);

        Assert.Contains("always provisions a dedicated Redis Docker", readme, StringComparison.Ordinal);
        Assert.Contains("does not", readme, StringComparison.Ordinal);
        Assert.Contains("reuse an externally supplied Redis", readme, StringComparison.Ordinal);
        Assert.Contains("endpoint", readme, StringComparison.Ordinal);
        Assert.Contains("sample name and execution id", readme, StringComparison.Ordinal);
        Assert.Contains("public endpoints", readme, StringComparison.Ordinal);
        Assert.DoesNotContain("waits briefly", readme, StringComparison.Ordinal);
        Assert.Contains("parallel sample runs do not share location store", readme, StringComparison.Ordinal);
        Assert.Contains("or reservation keys", readme, StringComparison.Ordinal);
    }
}
