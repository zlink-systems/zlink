using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void TicTacToeUsesExplicitHandlerRegistrationWithoutAssemblyScanning()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var sourceFiles = Directory.GetFiles(sampleRoot, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
            .Select(File.ReadAllText)
            .ToArray();

        Assert.All(sourceFiles, static source =>
            Assert.DoesNotContain("AddHandlersFromAssembly", source, StringComparison.Ordinal));
        Assert.Contains(sourceFiles, static source =>
            source.Contains("AddRequestHandler<AuthenticatePlayerHandler", StringComparison.Ordinal));
        Assert.Contains(sourceFiles, static source =>
            source.Contains("AddActorPacket<PlayActorJoinGameHandler", StringComparison.Ordinal));
        Assert.Contains(sourceFiles, static source =>
            source.Contains("AddHandler<AuthenticatePlaySessionHandler>", StringComparison.Ordinal));
        Assert.Equal(2, sourceFiles.Count(static source =>
            source.Contains("DisableImplicitHandlerAutoRegistration()", StringComparison.Ordinal)));
    }

    [Fact]
    public void TicTacToe_Separates_Manual_Object_Mesh_And_Api_ClientServer_Channel()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var apiServer = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Api", "ApiServer.cs"));
        var playServer = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play", "PlayServer.cs"));
        var createGame = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Api", "Handlers", "CreateGameHttpHandler.cs"));
        var authenticate = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Play", "Infrastructure", "ZLink", "Sessions", "Handlers",
            "AuthenticatePlaySessionHandler.cs"));
        var settings = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Configuration", "SampleSettings.cs"));
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));

        Assert.Equal(1, apiServer.Split("AddRouteMesh(", StringSplitOptions.None).Length - 1);
        Assert.Equal(1, playServer.Split("AddRouteMesh(", StringSplitOptions.None).Length - 1);
        Assert.Contains("AddRouteMesh(SampleNodes.Mesh)", apiServer, StringComparison.Ordinal);
        Assert.Contains("AddRouteMesh(SampleNodes.Mesh)", playServer, StringComparison.Ordinal);

        Assert.Contains("mesh.Objects().Client()", apiServer, StringComparison.Ordinal);
        Assert.Contains("mesh.Objects().Server()", playServer, StringComparison.Ordinal);
        Assert.DoesNotContain("mesh.Channel(SampleChannels.Api)", apiServer, StringComparison.Ordinal);
        Assert.DoesNotContain("mesh.Channel(SampleChannels.Api)", playServer, StringComparison.Ordinal);

        Assert.Contains("AddClientServerChannel(SampleChannels.Api)", apiServer, StringComparison.Ordinal);
        Assert.Contains(".Server()", apiServer, StringComparison.Ordinal);
        Assert.Contains("AddRequestHandler<AuthenticatePlayerHandler", apiServer, StringComparison.Ordinal);
        Assert.Contains("AddClientServerChannel(SampleChannels.Api)", playServer, StringComparison.Ordinal);
        Assert.Contains(".Client()", playServer, StringComparison.Ordinal);

        Assert.DoesNotContain("Channel(SampleTopics.PlayerMilestoneChannel)", apiServer,
            StringComparison.Ordinal);
        Assert.Contains("mesh.Channel(SampleTopics.PlayerMilestoneChannel).Server()", playServer,
            StringComparison.Ordinal);

        Assert.Contains(".EnableActorDispatch()", playServer, StringComparison.Ordinal);
        Assert.Contains(".Create(SampleTypes.GameSpot)", createGame, StringComparison.Ordinal);
        Assert.Contains(".InMesh(SampleNodes.Mesh)", createGame, StringComparison.Ordinal);
        Assert.Contains(".Request(new TicTacToeGameCreateReq(", createGame, StringComparison.Ordinal);
        Assert.DoesNotContain(".GetOrCreate(", createGame, StringComparison.Ordinal);
        Assert.DoesNotContain("Guid.NewGuid", createGame, StringComparison.Ordinal);
        Assert.Contains("RequestToChannel(\n                SampleChannels.Api,",
            authenticate, StringComparison.Ordinal);
        var game = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Play", "Infrastructure", "ZLink", "Spots", "TicTacToeGameSpot",
            "TicTacToeGame.cs"));
        Assert.Contains("Context.Outbound.Publish(", game, StringComparison.Ordinal);
        Assert.DoesNotContain("ZLinkPublishResult", game, StringComparison.Ordinal);
        Assert.DoesNotContain("result.Detail", game, StringComparison.Ordinal);
        Assert.Contains("string MeshEndpoint", settings, StringComparison.Ordinal);
        Assert.Contains("IReadOnlyList<string> PeerMeshEndpoints", settings, StringComparison.Ordinal);
        Assert.Contains(
            "\"PeerMeshEndpoints\": [\"${PLAY_A_MESH_ENDPOINT}\", \"${PLAY_B_MESH_ENDPOINT}\"]",
            shellRunner,
            StringComparison.Ordinal);
        Assert.Contains(
            "play(\"play-a\", \"${PLAY_A_MESH_ENDPOINT}\", [], \"${PLAY_A_ENDPOINT}\")",
            shellRunner,
            StringComparison.Ordinal);
        Assert.Contains(
            "play(\"play-b\", \"${PLAY_B_MESH_ENDPOINT}\", [\"${PLAY_A_MESH_ENDPOINT}\"], \"${PLAY_B_ENDPOINT}\")",
            shellRunner,
            StringComparison.Ordinal);
        Assert.Contains(
            "PeerMeshEndpoints = @($playAMeshEndpoint, $playBMeshEndpoint)",
            powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains(
            "New-TicTacToePlaySettings -InstanceName \"play-a\" -MeshEndpoint $playAMeshEndpoint -PeerMeshEndpoints @()",
            powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains(
            "New-TicTacToePlaySettings -InstanceName \"play-b\" -MeshEndpoint $playBMeshEndpoint -PeerMeshEndpoints @($playAMeshEndpoint)",
            powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("while len(sockets) < 8", shellRunner, StringComparison.Ordinal);
        Assert.Contains("$ports = New-SamplePorts -Count 8 -BasePort 0", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("SpotPubSubEndpoint", settings + shellRunner + powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("PlayChannelEndpoint", settings + shellRunner + powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("SpotPubSubEndpoint", apiServer + playServer, StringComparison.Ordinal);
        Assert.DoesNotContain("SampleNodes.PlaySpot", apiServer + playServer, StringComparison.Ordinal);
    }

    [Fact]
    public void TicTacToe_Server_Assemblies_Preserve_Role_Boundaries()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var configurationProject = File.ReadAllText(
            Path.Combine(
                sampleRoot,
                "Server",
                "Configuration",
                "TicTacToe.Server.Configuration.csproj"));
        var apiProject = File.ReadAllText(
            Path.Combine(sampleRoot, "Server", "Api", "TicTacToe.Server.Api.csproj"));
        var playProject = File.ReadAllText(
            Path.Combine(sampleRoot, "Server", "Play", "TicTacToe.Server.Play.csproj"));

        Assert.Contains("<EnableDefaultCompileItems>false</EnableDefaultCompileItems>",
            configurationProject,
            StringComparison.Ordinal);
        Assert.Contains("<Compile Include=\"*.cs\"/>",
            configurationProject,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Api/**/*.cs", configurationProject, StringComparison.Ordinal);
        Assert.DoesNotContain("Play/**/*.cs", configurationProject, StringComparison.Ordinal);
        Assert.Contains(
            "<ProjectReference Include=\"../Configuration/TicTacToe.Server.Configuration.csproj\"",
            apiProject,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Play/**/*.cs", apiProject, StringComparison.Ordinal);
        Assert.Contains(
            "<ProjectReference Include=\"../Configuration/TicTacToe.Server.Configuration.csproj\"",
            playProject,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Api/**/*.cs", playProject, StringComparison.Ordinal);
    }

    [Fact]
    public void TicTacToe_Registers_Stateful_Actor_Relocation_Adapter()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var host = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play", "PlayServer.cs"));
        var adapter = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play", "Infrastructure", "ZLink",
            "Actors", "PlayActorRelocationAdapter.cs"));

        Assert.Contains("PreserveStateWith<PlayActorRelocationAdapter>()", host,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkActorRelocationAdapter<PlayActor>", adapter, StringComparison.Ordinal);
        Assert.Contains("ValueTask<byte[]> CaptureAsync", adapter, StringComparison.Ordinal);
        Assert.Contains("ValueTask RestoreAsync", adapter, StringComparison.Ordinal);
    }

    [Fact]
    public void TicTacToe_Runner_Uses_Isolated_Docker_Redis()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));
        var settings = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "SampleSettings.cs"));
        var readme = File.ReadAllText(Path.Combine(sampleRoot, "README.md"));

        Assert.Contains("RUN_ID=\"$(basename \"${RUN_DIR}\")-$$-${RANDOM}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("TICTACTOE_REDIS_KEY_PREFIX=\"tictactoe:dotnet:${RUN_ID}:\"", shellRunner,
            StringComparison.Ordinal);
        AssertShellRunnerUsesRedisDockerHelper(
            shellRunner,
            "zlink-tictactoe-dotnet-redis",
            "TICTACTOE_REDIS_ENDPOINT",
            "REDIS_CONTAINER_ID");
        Assert.Contains("SAMPLE_LOG_DIR=\"${RUN_DIR}/sample-logs\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("TICTACTOE_LOG_DIR=\"${SAMPLE_LOG_DIR}\"", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("if [[ -z \"${TICTACTOE_REDIS_ENDPOINT:-}\" ]]", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("when TICTACTOE_REDIS_ENDPOINT is not set", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("TICTACTOE_BASE_PORT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_API_A_BIND_URL", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_API_B_BIND_URL", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_API_A_PUBLIC_URL", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_API_B_PUBLIC_URL", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_API_A_CHANNEL_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_API_B_CHANNEL_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_PLAY_A_CHANNEL_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_PLAY_B_CHANNEL_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_PLAY_A_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_PLAY_B_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_SPOT_A_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_SPOT_B_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_SPOT_A_PUBSUB_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_SPOT_B_PUBSUB_ENDPOINT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${TICTACTOE_LOG_DIR:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("rm -f \"${TICTACTOE_LOG_DIR}\"/*.log", shellRunner, StringComparison.Ordinal);
        Assert.Contains("TICTACTOE_REDIS_KEY_PREFIX", shellRunner, StringComparison.Ordinal);
        Assert.Contains("\"RedisKeyPrefix\": \"${TICTACTOE_REDIS_KEY_PREFIX}\"", shellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("intentionally derived here, not read", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("sleep 2", shellRunner, StringComparison.Ordinal);
        Assert.Contains("${SCRIPT_DIR}/Server/Play/bin/Debug/net8.0/TicTacToe.Server.Play.dll",
            shellRunner,
            StringComparison.Ordinal);
        Assert.Contains("${SCRIPT_DIR}/Server/Api/bin/Debug/net8.0/TicTacToe.Server.Api.dll",
            shellRunner,
            StringComparison.Ordinal);
        Assert.Contains("dotnet \"${assembly}\" --config \"${config_file}\"", shellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("local mode=", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("dotnet run --no-build --project \"${SCRIPT_DIR}/Server/TicTacToe.Server.csproj\"",
            shellRunner,
            StringComparison.Ordinal);

        Assert.Contains("$RunId = \"$PID-$([Guid]::NewGuid().ToString('N'))\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$TICTACTOE_REDIS_KEY_PREFIX = \"tictactoe:dotnet:${RunId}:\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("Start-SampleRedisContainer \"zlink-tictactoe-dotnet-redis\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("Remove-SampleRedisContainer $redisContainerId", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("docker run", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("$SampleLogDir = Join-Path $RunDir \"sample-logs\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$ports = New-SamplePorts -Count 8 -BasePort 0", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("if (-not $TICTACTOE_REDIS_ENDPOINT)", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("when TICTACTOE_REDIS_ENDPOINT is not set", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_BASE_PORT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_API_A_BIND_URL", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_API_B_BIND_URL", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_API_A_PUBLIC_URL", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_API_B_PUBLIC_URL", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_API_A_CHANNEL_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_API_B_CHANNEL_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_PLAY_A_CHANNEL_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_PLAY_B_CHANNEL_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_PLAY_A_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_PLAY_B_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_SPOT_A_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_SPOT_B_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_SPOT_A_PUBSUB_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$TICTACTOE_SPOT_B_PUBSUB_ENDPOINT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("if ($TICTACTOE_LOG_DIR)", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("Remove-Item -Path (Join-Path $SampleLogDir \"*.log\")", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("TICTACTOE_REDIS_KEY_PREFIX", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("RedisKeyPrefix = $TICTACTOE_REDIS_KEY_PREFIX", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("intentionally derived here, not read", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("Start-Sleep -Seconds 2", powershellRunner, StringComparison.Ordinal);

        Assert.DoesNotContain("AddEnvironmentVariables(\"TICTACTOE_\")", settings, StringComparison.Ordinal);
        Assert.Contains("RequireString(section, nameof(RedisEndpoint))", settings, StringComparison.Ordinal);
        Assert.Contains("RequireString(section, nameof(RedisKeyPrefix))", settings, StringComparison.Ordinal);

        Assert.Contains("Redis is required as the sample's official Location Store provider",
            readme,
            StringComparison.Ordinal);
        Assert.Contains("always provisions a", readme, StringComparison.Ordinal);
        Assert.Contains("does not", readme, StringComparison.Ordinal);
        Assert.Contains("reuse an externally supplied Redis endpoint", readme, StringComparison.Ordinal);
        Assert.Contains("sample name and execution id", readme, StringComparison.Ordinal);
        Assert.Contains("`TICTACTOE_REDIS_KEY_PREFIX`", readme, StringComparison.Ordinal);
    }

    [Fact]
    public void TicTacToe_Runner_Verifies_Client_And_Server_Evidence()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));

        Assert.Contains("stream-inbound sample=TicTacToe", shellRunner, StringComparison.Ordinal);
        Assert.Contains("stream-inbound sample=TicTacToe .* seq=[0-9]", shellRunner, StringComparison.Ordinal);
        Assert.Contains("stream-inbound sample=TicTacToe .* name=.*Notify", shellRunner, StringComparison.Ordinal);
        Assert.Contains("observer-win-milestone=verified", shellRunner, StringComparison.Ordinal);
        Assert.Contains("actor: LeaveGameMsg completed. actor=player-x", shellRunner, StringComparison.Ordinal);
        Assert.Contains("actor: LeaveGameMsg completed. actor=player-o", shellRunner, StringComparison.Ordinal);
        Assert.Contains("entry spot: actor destroy completed. actor=player-x", shellRunner, StringComparison.Ordinal);
        Assert.Contains("entry spot: actor destroy completed. actor=player-o", shellRunner, StringComparison.Ordinal);
        Assert.Contains("grep -R -q \"dispatch-error\"", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", shellRunner, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("LeaveGameMsg", shellRunner, StringComparison.Ordinal);

        Assert.Contains("stream-inbound sample=TicTacToe", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("stream-inbound sample=TicTacToe .* seq=[0-9]", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("stream-inbound sample=TicTacToe .* name=.*Notify", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("observer-win-milestone=verified", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("actor: LeaveGameMsg completed. actor=player-x", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("actor: LeaveGameMsg completed. actor=player-o", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("entry spot: actor destroy completed. actor=player-x", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("entry spot: actor destroy completed. actor=player-o", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", powershellRunner, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Select-String -Pattern \"dispatch-error\" -List", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("Select-String -Pattern \"dispatch-error\" -Quiet", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("LeaveGameMsg", powershellRunner, StringComparison.Ordinal);
    }

    [Fact]
    public void TicTacToe_Framework_LocationStore_Uses_The_Sample_Redis_Prefix()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var settings = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "SampleSettings.cs"));
        var playServer = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play", "PlayServer.cs"));

        Assert.Contains("string RedisKeyPrefix", settings, StringComparison.Ordinal);
        Assert.Contains("RequireString(section, nameof(RedisKeyPrefix))", settings, StringComparison.Ordinal);
        Assert.DoesNotContain("\"--redis-key-prefix\"", settings, StringComparison.Ordinal);
        Assert.Contains("KeyPrefix = settings.RedisKeyPrefix", playServer, StringComparison.Ordinal);
    }

    [Fact]
    public void TicTacToe_ClientScenario_Matches_Common_Flow()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var messages = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Contracts", "Messages.cs"));
        var clientScenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "TicTacToeClientScenario.cs"));
        var authenticateHandler = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Play", "Infrastructure",
            "ZLink", "Sessions", "Handlers", "AuthenticatePlaySessionHandler.cs"));

        Assert.Contains("record LeaveGameMsg", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("LeaveGameReq", messages, StringComparison.Ordinal);

        Assert.Contains("client1SawClient2Join.Payload.RoomId == room.RoomId", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("client1Move1.State.LastMoveActorId == options.XActorId", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("client1Move1.State.LastMoveCell == 0", clientScenario, StringComparison.Ordinal);
        Assert.Contains("client2Move1.State.LastMoveActorId == options.OActorId", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("client2Move1.State.LastMoveCell == 3", clientScenario, StringComparison.Ordinal);
        Assert.Contains("client1Move2.State.LastMoveActorId == options.XActorId", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("client1Move2.State.LastMoveCell == 1", clientScenario, StringComparison.Ordinal);
        Assert.Contains("client2Move2.State.LastMoveActorId == options.OActorId", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("client2Move2.State.LastMoveCell == 4", clientScenario, StringComparison.Ordinal);
        Assert.Contains("client1FinalMove.State.LastMoveActorId == options.XActorId", clientScenario,
            StringComparison.Ordinal);
        Assert.Contains("client1FinalMove.State.LastMoveCell == 2", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("SampleSettings settings", authenticateHandler, StringComparison.Ordinal);
    }

    [Fact]
    public void TicTacToe_Docs_Match_ScaleOut_RoomRoute_Flow()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");
        var readme = File.ReadAllText(Path.Combine(sampleRoot, "README.md"));
        var clientReadme = File.ReadAllText(Path.Combine(sampleRoot, "Client", "README.md"));
        var samplesReadme = File.ReadAllText(Path.Combine(ResolveSamplesRoot(), "README.md"));

        Assert.Contains("two API roles", readme, StringComparison.Ordinal);
        Assert.Contains("two Play roles", readme, StringComparison.Ordinal);
        Assert.Contains("`PlayEndpoints` and `PlayNodes`", readme, StringComparison.Ordinal);
        Assert.Contains("host, guest, and observer", readme, StringComparison.Ordinal);
        Assert.Contains("observer milestone verification", readme, StringComparison.Ordinal);
        Assert.Contains("`LeaveGameMsg` completion for both players", readme, StringComparison.Ordinal);
        Assert.Contains("entry-spot actor destroy evidence", readme, StringComparison.Ordinal);
        Assert.DoesNotContain("the play server to create a game", readme, StringComparison.Ordinal);
        Assert.DoesNotContain("creates the two stream connectors", readme, StringComparison.Ordinal);

        Assert.Contains("--observer-actor-id", clientReadme, StringComparison.Ordinal);
        Assert.Contains("three STREAM connections", clientReadme, StringComparison.Ordinal);
        Assert.Contains("observer", clientReadme, StringComparison.Ordinal);
        Assert.Contains("ObserveMilestoneReq", clientReadme, StringComparison.Ordinal);
        Assert.Contains("WinMilestoneNotify", clientReadme, StringComparison.Ordinal);
        Assert.Contains("LeaveGameMsg", clientReadme, StringComparison.Ordinal);
        Assert.DoesNotContain("opens two STREAM connections", clientReadme, StringComparison.Ordinal);

        Assert.Contains("Two API roles", samplesReadme, StringComparison.Ordinal);
        Assert.Contains("two Play roles", samplesReadme, StringComparison.Ordinal);
        Assert.Contains("Manual MeshNode peers", samplesReadme, StringComparison.Ordinal);
        Assert.Contains("Redis room route store", samplesReadme, StringComparison.Ordinal);
        Assert.Contains("Server.Play -- --config ./appsettings.play-a.json", samplesReadme, StringComparison.Ordinal);
        Assert.Contains("Server.Play -- --config ./appsettings.play-b.json", samplesReadme, StringComparison.Ordinal);
        Assert.Contains("Server.Api -- --config ./appsettings.api-a.json", samplesReadme, StringComparison.Ordinal);
        Assert.Contains("Server.Api -- --config ./appsettings.api-b.json", samplesReadme, StringComparison.Ordinal);
        Assert.DoesNotContain("temporary appsettings.json", samplesReadme, StringComparison.Ordinal);
        Assert.DoesNotContain("-- play --config ./appsettings.json", samplesReadme, StringComparison.Ordinal);
        Assert.DoesNotContain("-- api --config ./appsettings.json", samplesReadme, StringComparison.Ordinal);
    }
    [Fact]
    public void Bingo_And_TicTacToe_Samples_Implement_Actor_Lifecycle_Spec()
    {
        AssertActorLifecycleSpec(
            ResolveSampleRoot("Bingo"),
            "Server/Play/Infrastructure/ZLink/Spots/EntrySpot/BingoEntrySpot.cs",
            "Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/BingoRoom.cs",
            "Server/Play/Infrastructure/ZLink/Actors/PlayerActor.cs",
            "Server/Session/Sessions/BingoSession.cs");
        AssertActorLifecycleSpec(
            ResolveSampleRoot("TicTacToe"),
            "Server/Play/Infrastructure/ZLink/Spots/EntrySpot/PlayEntrySpot.cs",
            "Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/TicTacToeGame.cs",
            "Server/Play/Infrastructure/ZLink/Actors/PlayActor.cs",
            "Server/Play/Infrastructure/ZLink/Sessions/PlaySession.cs");
    }
    [Fact]
    public void TicTacToe_SessionGateway_Sample_Is_Removed()
    {
        var samplesRoot = ResolveSamplesRoot();
        var dotnetRoot = Directory.GetParent(samplesRoot)!.FullName;
        var sampleRoot = Path.Combine(samplesRoot, "TicTacToe.SessionGateway");
        var solution = Path.Combine(dotnetRoot, "Zlink.Framework.sln");
        var solutionText = File.ReadAllText(solution);

        Assert.False(
            Directory.Exists(sampleRoot),
            "TicTacToe keeps only the direct Api + Play sample. The SessionGateway variant must not be restored.");
        Assert.DoesNotContain("TicTacToe.SessionGateway", solutionText, StringComparison.Ordinal);
        Assert.DoesNotContain("samples\\TicTacToe.SessionGateway", solutionText, StringComparison.Ordinal);
    }

    [Fact]
    public void TicTacToe_Play_Session_Uses_FrameworkPayloadLifetimePolicy()
    {
        var sampleRoot = ResolveSampleRoot("TicTacToe");

        AssertSessionPayloadPolicy(sampleRoot);
    }

    [Fact]
    public void Bingo_Uses_Protobuf_And_TicTacToe_Uses_Json_Sample_Payloads()
    {
        var bingoRoot = ResolveSampleRoot("Bingo");
        var ticTacToeRoot = ResolveSampleRoot("TicTacToe");

        AssertSampleUsesProtobufPayloads(bingoRoot);
        AssertSampleUsesJsonPayloads(ticTacToeRoot);
        AssertCodecHelpersStayConfinedToRawLifecycleBoundaries(bingoRoot);
        AssertCodecHelpersStayConfinedToRawLifecycleBoundaries(ticTacToeRoot);
    }

    [Fact]
    public void NonTicTacToe_DotNet_Samples_Avoid_Runtime_Packet_Handler_Registration()
    {
        var samplesRoot = ResolveSamplesRoot();
        var manualRegistrationTokens = new[]
        {
            "Context.Handlers.AddHandler<",
            "Context.Handlers.AddPacket<",
            "Context.Handlers.AddActorPacket<"
        };
        var manualRegistrations = EnumerateSourceFiles(samplesRoot)
            .Where(file => !file.Contains(
                $"{Path.DirectorySeparatorChar}TicTacToe{Path.DirectorySeparatorChar}",
                StringComparison.Ordinal))
            .Where(file =>
            {
                var text = File.ReadAllText(file);
                return manualRegistrationTokens.Any(token => text.Contains(token, StringComparison.Ordinal));
            })
            .ToArray();

        Assert.Empty(manualRegistrations);
    }
}
