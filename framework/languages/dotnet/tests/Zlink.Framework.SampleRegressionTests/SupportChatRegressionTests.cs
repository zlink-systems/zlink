using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void SupportChat_Uses_One_Physical_Mesh_And_Scanned_Channel_Handlers()
    {
        var sampleRoot = ResolveSampleRoot("SupportChat");
        var hosts = new[]
        {
            Path.Combine(sampleRoot, "Server", "Api", "ApiServerHostFactory.cs"),
            Path.Combine(sampleRoot, "Server", "Support", "SupportServerHostFactory.cs"),
            Path.Combine(sampleRoot, "Server", "Session", "SessionServerHostFactory.cs")
        };

        foreach (var host in hosts)
        {
            var source = File.ReadAllText(host);
            Assert.Equal(1, source.Split("AddRouteMesh(", StringSplitOptions.None).Length - 1);
            Assert.Contains("AddRouteMesh(SampleNames.MeshName)", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddRequestHandler<", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddSendHandler<", source, StringComparison.Ordinal);
            Assert.DoesNotContain("mesh.Channel(SampleNames.MeshName)", source, StringComparison.Ordinal);
        }

        Assert.Contains("AddClientServerChannel(SampleNames.ApiChannel).Server()", File.ReadAllText(hosts[0]),
            StringComparison.Ordinal);
        Assert.Contains("AddClientServerChannel(SampleNames.ApiChannel).Client()", File.ReadAllText(hosts[1]),
            StringComparison.Ordinal);
        Assert.Contains("AddClientServerChannel(SampleNames.ApiChannel).Client()", File.ReadAllText(hosts[2]),
            StringComparison.Ordinal);
    }

    [Fact]
    public void SupportChat_Local_Actor_Directory_Does_Not_Cache_Location_Ownership()
    {
        var sampleRoot = ResolveSampleRoot("SupportChat");
        var actorDirectory = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Support",
            "Infrastructure",
            "ZLink",
            "Actors",
            "SupportActorDirectory.cs"));
        var entrySpot = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Support",
            "Infrastructure",
            "ZLink",
            "Spots",
            "EntrySpot",
            "SupportEntrySpot.cs"));

        Assert.DoesNotContain("ActorRef", actorDirectory, StringComparison.Ordinal);
        Assert.DoesNotContain("FindAsync", entrySpot, StringComparison.Ordinal);
        Assert.Contains("directory.AddOrUpdate(actor)", entrySpot, StringComparison.Ordinal);
    }

    [Fact]
    public void SupportChat_Client_Gate_Exercises_All_Required_Rejections()
    {
        var sampleRoot = ResolveSampleRoot("SupportChat");
        var scenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "SupportChatClientScenario.cs"));

        Assert.Equal(
            7,
            scenario.Split("ZlinkStreamAssert.ExpectFailureAsync(", StringSplitOptions.None).Length - 1);
        Assert.Contains("new OpenConversationReq(\"unauthenticated\")", scenario, StringComparison.Ordinal);
        Assert.Contains("new SendChatMessageReq(\"unauthenticated\")", scenario, StringComparison.Ordinal);
        Assert.Contains("new OpenConversationReq(\"agent cannot open\")", scenario, StringComparison.Ordinal);
        Assert.Contains("new SendChatMessageReq(\"not a participant\")", scenario, StringComparison.Ordinal);
        Assert.Contains("nameof(ZlinkStreamErrorCode.RemoteError)", scenario, StringComparison.Ordinal);
    }

    [Fact]
    public void SupportChat_Typing_Uses_The_Common_One_Way_Message()
    {
        var sampleRoot = ResolveSampleRoot("SupportChat");
        var messages = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Contracts", "Messages.cs"));
        var scenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "SupportChatClientScenario.cs"));
        var handler = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support",
            "Infrastructure", "ZLink", "Spots", "ConversationSpot", "Handlers", "SetTypingHandler.cs"));

        Assert.Contains("record SetTypingMsg", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("SetTypingReq", messages, StringComparison.Ordinal);
        Assert.Contains("connector.Send(new SetTypingMsg", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("connector.Request(new SetTypingMsg", scenario, StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotActorSendHandler<ConversationSpot, SupportUserActor, SetTypingMsg>",
            handler, StringComparison.Ordinal);
        Assert.DoesNotContain("RequestHandler", handler, StringComparison.Ordinal);
    }

    [Fact]
    public void SupportChat_Registers_Stateful_Actor_Relocation_Adapter()
    {
        var sampleRoot = ResolveSampleRoot("SupportChat");
        var host = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "SupportServerHostFactory.cs"));
        var adapter = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Infrastructure", "ZLink",
            "Actors", "SupportUserActorRelocationAdapter.cs"));

        Assert.Contains("PreserveStateWith<SupportUserActorRelocationAdapter>()", host,
            StringComparison.Ordinal);
        Assert.Contains("IZLinkActorRelocationAdapter<SupportUserActor>", adapter, StringComparison.Ordinal);
        Assert.Contains("ValueTask<byte[]> CaptureAsync", adapter, StringComparison.Ordinal);
        Assert.Contains("ValueTask RestoreAsync", adapter, StringComparison.Ordinal);
    }

    [Fact]
    public void SupportChat_Runner_Uses_Isolated_Docker_Redis_And_Location_Store()
    {
        var sampleRoot = ResolveSampleRoot("SupportChat");
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));
        var readme = File.ReadAllText(Path.Combine(sampleRoot, "README.ko.md"));
        var apiHost = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Api", "ApiServerHostFactory.cs"));
        var sessionHost = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Session", "SessionServerHostFactory.cs"));
        var supportHost = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "SupportServerHostFactory.cs"));
        var topology = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "SampleTopology.cs"));
        var sharedMessages = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Contracts", "Messages.cs"));
        var serverContracts = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "SupportServerContracts.cs"));
        var assignment = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Application",
            "ConversationAssignment", "AgentAssignmentService.cs"));
        var availability = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Application",
            "ConversationAssignment", "AgentAvailabilityDirectory.cs"));
        var availableHandler = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Infrastructure",
            "ZLink", "Spots", "EntrySpot", "Handlers", "SetAgentAvailableHandler.cs"));
        var session = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Session", "Sessions",
            "SupportChatSession.cs"));
        var supportActor = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Infrastructure",
            "ZLink", "Actors", "SupportUserActor.cs"));
        var relocationAdapter = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Infrastructure",
            "ZLink", "Actors", "SupportUserActorRelocationAdapter.cs"));
        var joinConversationHandler = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Infrastructure",
            "ZLink", "Spots", "EntrySpot", "Handlers", "JoinConversationActorHandler.cs"));
        var entrySpot = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Infrastructure", "ZLink",
            "Spots", "EntrySpot", "SupportEntrySpot.cs"));
        var conversationSpot = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Support", "Infrastructure",
            "ZLink", "Spots", "ConversationSpot", "ConversationSpot.cs"));
        var clientScenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "SupportChatClientScenario.cs"));
        var openConversation = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server",
            "Api",
            "Handlers",
            "OpenConversationHandler.cs"));

        Assert.Contains("RUN_ID=\"$(basename \"${RUN_DIR}\")-$$-${RANDOM}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("SAMPLE_LOG_DIR=\"${RUN_DIR}/sample-logs\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("SUPPORTCHAT_LOG_DIR=\"${SAMPLE_LOG_DIR}\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("SUPPORTCHAT_REDIS_KEY_PREFIX=\"supportchat:dotnet:${RUN_ID}:\"", shellRunner,
            StringComparison.Ordinal);
        Assert.Contains("REDIS_CONTAINER=\"zlink-supportchat-dotnet-redis-${RUN_ID}\"", shellRunner,
            StringComparison.Ordinal);
        AssertShellRunnerUsesRedisDockerHelper(
            shellRunner,
            "zlink-supportchat-dotnet-redis",
            "SUPPORTCHAT_REDIS_ENDPOINT");
        Assert.DoesNotContain("if [[ -z \"${SUPPORTCHAT_REDIS_ENDPOINT:-}\" ]]", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("when SUPPORTCHAT_REDIS_ENDPOINT is not set", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("SUPPORTCHAT_BASE_PORT", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_API_CHANNEL_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_SUPPORT_CHANNEL_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_SESSION_SPOT_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_SESSION_ROUTER_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_SUPPORT_ROUTER_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("export SUPPORTCHAT_SUPPORT_ROUTER_ENDPOINT=", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_ENTRY_SPOT_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_ENTRY_SPOT_ROUTER_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_CONVERSATION_SPOT_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_CONVERSATION_SPOT_ROUTER_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("export SUPPORTCHAT_CONVERSATION_SPOT_ENDPOINT=", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("export SUPPORTCHAT_CONVERSATION_SPOT_ROUTER_ENDPOINT=", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_STREAM_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_RECONNECT_STREAM_ENDPOINT:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("export SUPPORTCHAT_RECONNECT_STREAM_ENDPOINT=", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_REDIS_KEY_PREFIX:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("${SUPPORTCHAT_LOG_DIR:-", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("rm -f \"${SUPPORTCHAT_LOG_DIR}\"/*.log", shellRunner, StringComparison.Ordinal);
        Assert.Contains("supportchat=completed", shellRunner, StringComparison.Ordinal);
        Assert.Contains("supportchat-closed-typing-ignore=verified", shellRunner, StringComparison.Ordinal);
        Assert.Contains("supportchat-server-evidence=completed", shellRunner, StringComparison.Ordinal);
        Assert.Contains("status=WaitingForAgent", shellRunner, StringComparison.Ordinal);
        Assert.Contains("status=Active", shellRunner, StringComparison.Ordinal);
        Assert.Contains("status=WaitingForClose", shellRunner, StringComparison.Ordinal);
        Assert.Contains("status=Closed", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", shellRunner, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("SUPPORTCHAT_STARTUP_DELAY_SECONDS", shellRunner, StringComparison.Ordinal);

        Assert.Contains("$RunId = \"$PID-$([Guid]::NewGuid().ToString('N'))\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$SampleLogDir = Join-Path $RunDir \"sample-logs\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$ports = New-SamplePorts -Count 4 -BasePort 0", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("$SUPPORTCHAT_REDIS_KEY_PREFIX = \"supportchat:dotnet:${RunId}:\"",
            powershellRunner, StringComparison.Ordinal);
        AssertPowerShellRunnerUsesRedisDockerHelper(powershellRunner, "zlink-supportchat-dotnet-redis");
        Assert.Contains("if ($RedisContainer)", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("$SUPPORTCHAT_LOG_DIR = $SampleLogDir", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("if (-not $SUPPORTCHAT_REDIS_ENDPOINT)", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("when SUPPORTCHAT_REDIS_ENDPOINT is not set", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_BASE_PORT", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_API_CHANNEL_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_SUPPORT_CHANNEL_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_SESSION_SPOT_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_SESSION_ROUTER_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_SUPPORT_ROUTER_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_SUPPORT_ROUTER_ENDPOINT =", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_ENTRY_SPOT_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_ENTRY_SPOT_ROUTER_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_CONVERSATION_SPOT_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_CONVERSATION_SPOT_ROUTER_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_CONVERSATION_SPOT_ENDPOINT =", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_CONVERSATION_SPOT_ROUTER_ENDPOINT =", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_STREAM_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_RECONNECT_STREAM_ENDPOINT) {", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("$SUPPORTCHAT_RECONNECT_STREAM_ENDPOINT =", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("if ($SUPPORTCHAT_LOG_DIR)", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("Set-DefaultEnv", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("Remove-Item -Path (Join-Path $SampleLogDir \"*.log\")", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("supportchat=completed", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("supportchat-closed-typing-ignore=verified", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("supportchat-server-evidence=completed", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("status=WaitingForAgent", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("status=Active", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("status=WaitingForClose", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("status=Closed", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", powershellRunner, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("SUPPORTCHAT_STARTUP_DELAY_SECONDS", powershellRunner, StringComparison.Ordinal);

        Assert.Contains("RedisEndpoint", topology, StringComparison.Ordinal);
        Assert.Contains("RedisKeyPrefix", topology, StringComparison.Ordinal);
        Assert.DoesNotContain("Environment.GetEnvironmentVariable", topology, StringComparison.Ordinal);
        Assert.DoesNotContain("public sealed record ActorRef", sharedMessages, StringComparison.Ordinal);
        Assert.DoesNotContain("EnsureSupportUserActorReq", sharedMessages, StringComparison.Ordinal);
        Assert.DoesNotContain("public sealed record ActorRef", serverContracts, StringComparison.Ordinal);
        Assert.DoesNotContain("ActorRef", serverContracts, StringComparison.Ordinal);
        Assert.Contains(".GetOrCreate(actorId, SampleNames.SupportActorType)", session,
            StringComparison.Ordinal);
        Assert.Contains(
            ".Create(SampleNames.ConversationSpotType)",
            openConversation,
            StringComparison.Ordinal);
        Assert.Contains(
            ".InMesh(SampleNames.MeshName)",
            openConversation,
            StringComparison.Ordinal);
        Assert.DoesNotContain("NodeRid", openConversation, StringComparison.Ordinal);
        Assert.DoesNotContain("SupportChannel", session, StringComparison.Ordinal);
        Assert.Contains("ZLinkActorJoinCompletion.Accepted accepted", supportActor, StringComparison.Ordinal);
        Assert.Contains("CurrentRef = accepted.Actor", supportActor, StringComparison.Ordinal);
        Assert.Contains("reply.Decode<JoinConversationRes>()", supportActor, StringComparison.Ordinal);
        Assert.Contains("JoinConversationFailedNotify", supportActor, StringComparison.Ordinal);
        Assert.Contains(".Defer()", joinConversationHandler, StringComparison.Ordinal);
        Assert.Contains("Context.BoundSession", joinConversationHandler, StringComparison.Ordinal);
        Assert.Contains(".Async(cancellationToken)", joinConversationHandler, StringComparison.Ordinal);
        Assert.Contains("public sealed record SupportUserActorCreateReq", serverContracts, StringComparison.Ordinal);
        Assert.Contains("CompletedJoinOperations", relocationAdapter, StringComparison.Ordinal);
        AssertLocationStoreHost(apiHost);
        AssertLocationStoreHost(sessionHost);
        AssertLocationStoreHost(supportHost);
        Assert.Contains("_reservations.Values.Count", assignment, StringComparison.Ordinal);
        Assert.Contains("availability.SetAvailable(rosterActorId, displayName, isAvailable, activeConversations)",
            assignment, StringComparison.Ordinal);
        Assert.Contains("int activeConversations", availability, StringComparison.Ordinal);
        Assert.Contains("assignment.SetAvailable(actor.ActorId, actor.DisplayName, message.IsAvailable)",
            availableHandler, StringComparison.Ordinal);
        Assert.Contains("assignment.SetAvailable(actor.ActorId, actor.DisplayName, false)", entrySpot,
            StringComparison.Ordinal);
        Assert.Contains("support conversation: state changed", conversationSpot, StringComparison.Ordinal);
        Assert.Contains("supportchat-closed-typing-ignore=verified", clientScenario, StringComparison.Ordinal);
        Assert.Contains("ExpectNone<TypingChangedNotify>()", clientScenario, StringComparison.Ordinal);

        Assert.Contains("외부 Redis endpoint 재사용 mode는 제공하지 않는다", readme, StringComparison.Ordinal);
        Assert.Contains("실행별 key prefix를 역할들이 읽는 임시 config 파일에 기록한다", readme,
            StringComparison.Ordinal);
        Assert.Contains("동시에 실행되는 다른 테스트와 섞이지 않는다", readme, StringComparison.Ordinal);
        Assert.Contains("표준 .NET diagnostics", readme, StringComparison.Ordinal);
    }
}
