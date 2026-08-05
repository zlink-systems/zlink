using Xunit;
using System.Text.RegularExpressions;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void Sample_And_E2e_Use_Stream_Connector_Assertions()
    {
        var sourceFiles = EnumerateSourceFiles(ResolveSamplesRoot())
            .Concat(EnumerateSourceFiles(ResolveE2eRoot()))
            .ToArray();
        var clientFiles = sourceFiles
            .Where(path => path.Contains(
                $"{Path.DirectorySeparatorChar}Client{Path.DirectorySeparatorChar}",
                StringComparison.Ordinal))
            .ToArray();
        var localAssertion = new Regex(
            @"(?:(?:private|public|internal|protected)\s+)?(?:static\s+)?(?:async\s+)?(?:void|Task|ValueTask)\s+(?:Ensure|That|RequireContains|RequireNoContains|ExpectFailureAsync|ExpectTimeoutAsync)\s*\(",
            RegexOptions.CultureInvariant);
        var offenders = sourceFiles
            .Where(path => localAssertion.IsMatch(File.ReadAllText(path)))
            .Select(path => NormalizeRelativePath(Path.GetRelativePath(ResolveDotnetRoot(), path)))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(offenders);
        var clientText = string.Join(Environment.NewLine, clientFiles.Select(File.ReadAllText));
        Assert.Contains("ZlinkStreamAssert.Ensure(", clientText, StringComparison.Ordinal);
        Assert.Contains("ExpectNone<", clientText, StringComparison.Ordinal);
        Assert.DoesNotContain("ScenarioAssert.That(", clientText, StringComparison.Ordinal);
        Assert.DoesNotContain("ScenarioContext.Require(", clientText, StringComparison.Ordinal);
        Assert.DoesNotContain("ReceivedCount(nameof(PlayerJoinedNotify))", clientText, StringComparison.Ordinal);

        foreach (var relativePath in new[]
                 {
                     "AutomaticTurnDispatch/Client/Scenarios/ShutdownAwaitProbe.cs",
                     "SpotService/Client/Scenarios/SmD8StreamReconnectRecoveryScenario.cs",
                     "SpotService/Client/Scenarios/SmD4MultipleActorBindingScenario.cs",
                     "SpotService/Client/Scenarios/SmD14TlsStreamValidationScenario.cs",
                     "SpotService/Client/Scenarios/SmG1BoundActorCrashRecoveryScenario.cs",
                     "SpotService/Client/Scenarios/SmB8ExplicitActorDestroyScenario.cs"
                 })
        {
            var source = File.ReadAllText(Path.Combine(
                ResolveE2eRoot(),
                relativePath.Replace('/', Path.DirectorySeparatorChar)));
            Assert.Contains("ZlinkStreamAssert.ExpectFailureAsync", source, StringComparison.Ordinal);
            Assert.DoesNotContain("Failed = false", source, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("catch\n        {", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void Only_TicTacToe_May_Use_Manual_Server_Connections()
    {
        var samplesRoot = ResolveSamplesRoot();
        var offenders = Directory
            .EnumerateDirectories(samplesRoot)
            .Where(static path => !string.Equals(Path.GetFileName(path), "TicTacToe", StringComparison.Ordinal))
            .SelectMany(EnumerateSourceFiles)
            .Select(path => (Path: path, Text: File.ReadAllText(path)))
            .SelectMany(source =>
            {
                var violations = new List<string>();
                if (source.Text.Contains(".PeerConnections.Connect(", StringComparison.Ordinal))
                    violations.Add("manual MeshNode peer connection");
                if (source.Path.Contains(
                        $"{Path.DirectorySeparatorChar}Server{Path.DirectorySeparatorChar}",
                        StringComparison.Ordinal)
                    && source.Text.Contains("ZLinkHttpClient.Create(", StringComparison.Ordinal))
                    violations.Add("server-to-server ZLinkHttpClient");
                return violations.Select(violation =>
                    $"{Path.GetRelativePath(samplesRoot, source.Path)}:{violation}");
            })
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.True(
            offenders.Length == 0,
            "TicTacToe is the only sample allowed to wire servers manually. "
            + "All other samples must use location-store automatic connections: "
            + string.Join(", ", offenders));
    }

    [Theory]
    [InlineData("Bingo")]
    [InlineData("DeliveryDispatch")]
    [InlineData("GameQuest")]
    [InlineData("ShoppingMall")]
    [InlineData("SupportChat")]
    [InlineData("ZoneWorld")]
    public void AutomaticSamplesUseAssemblyScanningAndLocationDiscovery(string sampleName)
    {
        var sources = EnumerateSourceFiles(ResolveSampleRoot(sampleName))
            .Select(File.ReadAllText)
            .ToArray();
        var combined = string.Join(Environment.NewLine, sources);

        Assert.Contains("AddHandlersFromAssembly", combined, StringComparison.Ordinal);
        Assert.DoesNotContain("DisableImplicitHandlerAutoRegistration", combined, StringComparison.Ordinal);
        Assert.DoesNotContain(".PeerConnections.Connect(", combined, StringComparison.Ordinal);
    }

    [Fact]
    public void SamplePayloadCodecsMatchTheCommonSampleContract()
    {
        var samplesRoot = ResolveSamplesRoot();
        var bingo = string.Join(Environment.NewLine,
            EnumerateSourceFiles(Path.Combine(samplesRoot, "Bingo")).Select(File.ReadAllText));
        Assert.Contains("Codecs.Use(ZLinkProtobufCodec.Default)", bingo, StringComparison.Ordinal);

        foreach (var sampleName in new[]
                 {
                     "TicTacToe",
                     "DeliveryDispatch",
                     "GameQuest",
                     "ShoppingMall",
                     "SupportChat",
                     "ZoneWorld"
                 })
        {
            var source = string.Join(Environment.NewLine,
                EnumerateSourceFiles(Path.Combine(samplesRoot, sampleName)).Select(File.ReadAllText));
            Assert.DoesNotContain("Codecs.Use(", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void Samples_Do_Not_Use_Location_Stores_Or_Resolvers_As_Business_Dependencies()
    {
        var forbidden = new[]
        {
            "IZLinkLocationStore",
            "IZLinkLocationStore",
            "IZLinkLocationStore",
            "IZLinkSpotHandleResolver",
            "IZLinkActorSpotHandleResolver"
        };

        var offenders = EnumerateSourceFiles(ResolveSamplesRoot())
            .SelectMany(file => forbidden
                .Where(token => File.ReadAllText(file).Contains(token, StringComparison.Ordinal))
                .Select(token => $"{Path.GetRelativePath(ResolveSamplesRoot(), file)}:{token}"))
            .ToArray();

        Assert.True(
            offenders.Length == 0,
            "Samples must not use location stores or actor address resolvers directly: "
            + string.Join(", ", offenders));
    }

    [Fact]
    public void Sample_Object_Creation_Uses_Framework_Placement()
    {
        string[] forbiddenPlacementTokens =
        [
            "PlacementProfile",
            "AffinityKey",
            "PreferredOwner",
            "TargetNodeRid",
            ".InNode(",
            ".OnNode("
        ];
        var samplesRoot = ResolveSamplesRoot();
        var sourceFiles = EnumerateSourceFiles(samplesRoot).ToArray();
        var placementOffenders = sourceFiles
            .SelectMany(file => forbiddenPlacementTokens
                .Where(token => File.ReadAllText(file).Contains(token, StringComparison.Ordinal))
                .Select(token =>
                    $"{NormalizeRelativePath(Path.GetRelativePath(samplesRoot, file))}:{token}"))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(placementOffenders);

        // Samples teach owner-neutral application routing. Infrastructure can expose
        // Node direct as a public capability, but sample workflows must not turn an
        // observed transport RID back into an application target.
        var nodeDirectUsers = sourceFiles
            .Where(file =>
            {
                var source = File.ReadAllText(file);
                return source.Contains("RequestToNode(", StringComparison.Ordinal)
                       || source.Contains("SendToNode(", StringComparison.Ordinal);
            })
            .Select(file => NormalizeRelativePath(Path.GetRelativePath(samplesRoot, file)))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(nodeDirectUsers);
    }

    [Fact]
    public void Sample_Session_Binding_Uses_BindOrGetAsync()
    {
        var allowedExplicitRebindFiles = new HashSet<string>(StringComparer.Ordinal)
        {
            NormalizeRelativePath(Path.Combine("e2e", "SpotService", "Server", "MultiNode", "Handlers", "MultiNodeSessionHandlers.cs")),
            NormalizeRelativePath(Path.Combine("e2e", "SpotService", "Server", "Play", "Handlers", "PlaySessionHandlers.cs")),
            NormalizeRelativePath(Path.Combine("e2e", "SpotService", "Server", "Session", "Handlers", "SessionSessionHandlers.cs")),
            NormalizeRelativePath(Path.Combine("e2e", "SpotActorTransfer", "Server", "ActorNode", "Program.cs")),
            NormalizeRelativePath(Path.Combine("e2e", "SpotActorTransfer", "Client", "Scenarios", "StE1ANewIncarnationExplicitBindScenario.cs")),
            NormalizeRelativePath(Path.Combine("e2e", "AutomaticTurnDispatch", "Server", "Session", "Support", "AwaitSession.cs"))
        };
        var sampleSessionFiles = new[] { "Bingo", "DeliveryDispatch", "SupportChat", "TicTacToe" }
            .Select(ResolveSampleRoot)
            .SelectMany(static root => EnumerateSessionRoots(root))
            .SelectMany(static root => EnumerateSourceFiles(root))
            .ToArray();
        var e2eSessionFiles = EnumerateSourceFiles(ResolveE2eRoot()).ToArray();
        var sessionFiles = sampleSessionFiles.Concat(e2eSessionFiles).ToArray();
        Assert.NotEmpty(sampleSessionFiles);
        Assert.NotEmpty(e2eSessionFiles);

        var sessionText = string.Join(Environment.NewLine, sessionFiles.Select(File.ReadAllText));
        var bindAsyncOffenders = sessionFiles
            .Where(file => File.ReadAllText(file).Contains(".BindAsync(", StringComparison.Ordinal))
            .Select(file => NormalizeRelativePath(Path.GetRelativePath(ResolveDotnetRoot(), file)))
            .Where(file => !allowedExplicitRebindFiles.Contains(file))
            .ToArray();

        Assert.Contains("BindOrGetAsync", sessionText, StringComparison.Ordinal);
        Assert.True(
            bindAsyncOffenders.Length == 0,
            "Session binding should use BindOrGetAsync unless the file intentionally exercises explicit rebinding: "
            + string.Join(", ", bindAsyncOffenders));
    }

    [Fact]
    public void Sample_Health_Checks_Use_Location_Readiness()
    {
        var hostFiles = EnumerateSourceFiles(ResolveSamplesRoot())
            .Where(static file => file.EndsWith("HostFactory.cs", StringComparison.Ordinal)
                                  || Path.GetFileName(file).Equals("Program.cs", StringComparison.Ordinal))
            .ToArray();
        Assert.NotEmpty(hostFiles);

        var allText = string.Join(Environment.NewLine, hostFiles.Select(File.ReadAllText));

        Assert.Contains("IZLinkLocationReadiness", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("IZLinkLocationRuntimeQuery", allText, StringComparison.Ordinal);
    }

    [Fact]
    public void EntrySpotCreationHooks_Use_Exact_Response_Type()
    {
        var hookDeclaration = new Regex(
            @"public\s+(?:async\s+)?(?<return>\S+)\s+OnCreateActorAsync\s*\(",
            RegexOptions.CultureInvariant);
        var offenders = EnumerateSourceFiles(ResolveSamplesRoot())
            .Concat(EnumerateSourceFiles(ResolveE2eRoot()))
            .SelectMany(path => hookDeclaration.Matches(File.ReadAllText(path))
                .Cast<Match>()
                .Where(match => !string.Equals(
                    match.Groups["return"].Value,
                    "ValueTask<ZLinkActorCreateResponse>",
                    StringComparison.Ordinal))
                .Select(_ => NormalizeRelativePath(Path.GetRelativePath(ResolveDotnetRoot(), path))))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(offenders);
    }

    private static void AssertShellRunnerUsesRedisDockerHelper(
        string shellRunner,
        string scope,
        string endpointVariable,
        string containerVariable = "REDIS_CONTAINER")
    {
        Assert.Contains("source \"${SCRIPT_DIR}/../redis-common.sh\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains(
            $"zlink_redis_start_scoped_assign {containerVariable} {endpointVariable} \"{scope}\" redis:7.2-alpine",
            shellRunner,
            StringComparison.Ordinal);
        Assert.Contains($"if [[ -n \"${{{containerVariable}}}\" ]]; then", shellRunner, StringComparison.Ordinal);
        Assert.Contains($"docker rm -fv \"${{{containerVariable}}}\"", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("docker run -d --rm", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("-p \"127.0.0.1::6379\"", shellRunner, StringComparison.Ordinal);
        Assert.Contains("RUN_SUCCEEDED=0", shellRunner, StringComparison.Ordinal);
        Assert.Contains("RUN_SUCCEEDED=1", shellRunner, StringComparison.Ordinal);
    }

    private static void AssertPowerShellRunnerUsesRedisDockerHelper(string powershellRunner, string scope)
    {
        Assert.Contains($"Start-SampleRedisContainer \"{scope}\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.Contains("Remove-SampleRedisContainer $RedisContainer", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("docker run", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("docker create", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("$RunSucceeded = $false", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("$RunSucceeded = $true", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("if (-not $RunSucceeded", powershellRunner, StringComparison.Ordinal);
    }

    [Fact]
    public void DotNet_Samples_Do_Not_Use_Legacy_Registry_Discovery()
    {
        var samplesRoot = ResolveSamplesRoot();
        var offenders = Directory
            .EnumerateDirectories(samplesRoot)
            .Where(static path => !string.Equals(Path.GetFileName(path), "TicTacToe", StringComparison.Ordinal))
            .SelectMany(EnumerateSourceFiles)
            .Select(path => (Path: path, Text: File.ReadAllText(path)))
            .Where(static source =>
                source.Text.Contains("UseDiscovery(", StringComparison.Ordinal)
                || source.Text.Contains("UseRegistrySpotResolver", StringComparison.Ordinal)
                || source.Text.Contains("AddZLinkRegistry", StringComparison.Ordinal)
                || source.Text.Contains("AddRegistryEvents", StringComparison.Ordinal))
            .Select(source => Path.GetRelativePath(samplesRoot, source.Path))
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Empty(offenders);
    }

    [Fact]
    public void DotNet_Docs_Keep_Actor_Destroy_Entry_Owned()
    {
        var dotnetRoot = ResolveDotnetRoot();
        var frameworkDocRoot = Path.GetFullPath(Path.Combine(dotnetRoot, "..", "..", "doc", "framework"));
        var dotnetDocRoot = Path.Combine(frameworkDocRoot, "dotnet");
        var dotnetContractRoot = Path.Combine(
            frameworkDocRoot,
            "common",
            "spec",
            "server",
            "languages",
            "dotnet");
        var docs = EnumerateMarkdownFiles(Path.Combine(dotnetDocRoot, "guide"))
            .Concat(EnumerateMarkdownFiles(dotnetContractRoot))
            .Concat(EnumerateMarkdownFiles(Path.Combine(dotnetDocRoot, "internals")))
            .Concat(Directory.EnumerateFiles(Path.Combine(dotnetRoot, "samples"), "README.md",
                SearchOption.AllDirectories))
            .Concat(Directory.EnumerateFiles(Path.Combine(dotnetRoot, "samples"), "README.ko.md",
                SearchOption.AllDirectories))
            .ToArray();
        var offenders = new List<string>();
        (string Needle, string Reason)[] forbidden =
        [
            ("destroyActor(", "lower camel destroy API"),
            ("destroyActorAsync", "lower camel async destroy API"),
            ("destroy_actor", "snake case destroy API"),
            ("OnActorLeft", "legacy PascalCase left callback"),
            ("onActorLeft", "legacy lower camel left callback"),
            ("on_actor_left", "legacy snake case left callback"),
            ("OnCreateActor(", "legacy PascalCase create callback"),
            ("on_actor_created", "legacy snake case create callback"),
            ("onPostActorJoined", "legacy post actor joined callback"),
            ("disconnect -> destroy", "disconnect-to-destroy arrow wording"),
            ("자동 삭제", "automatic deletion wording")
        ];

        foreach (var file in docs)
        {
            var text = File.ReadAllText(file);
            foreach (var (needle, reason) in forbidden)
                if (text.Contains(needle, StringComparison.Ordinal))
                    offenders.Add($"{Path.GetRelativePath(dotnetRoot, file)}: {reason}");
        }

        var actorSpec = File.ReadAllText(Path.Combine(
            dotnetContractRoot,
            "interfaces",
            "05-spots.ko.md"));
        var spotModelSpec = File.ReadAllText(Path.Combine(
            frameworkDocRoot,
            "common",
            "spec",
            "11-spot-model.ko.md"));
        var ticTacToeSampleSpec = File.ReadAllText(Path.Combine(
            frameworkDocRoot,
            "common",
            "sample",
            "tictactoe",
            "README.ko.md"));
        Assert.Contains("ValueTask DestroyActorAsync(", actorSpec, StringComparison.Ordinal);
        Assert.Contains("Entry Spot은 close operation 대신 Actor destroy", spotModelSpec, StringComparison.Ordinal);
        Assert.Contains("Entry Spot context의 `destroyActor`를 호출한다", ticTacToeSampleSpec, StringComparison.Ordinal);
        Assert.Contains("`destroyActor`는 `onLeaveActor`나 다른 lifecycle callback을 호출하지 않고", ticTacToeSampleSpec, StringComparison.Ordinal);
        Assert.Contains("native actor ref, framework registry, bound session binding을 정리한다", ticTacToeSampleSpec, StringComparison.Ordinal);
        Assert.Empty(offenders.Order(StringComparer.Ordinal));
    }

    private static void AssertLocationStoreHost(string hostFactory)
    {
        Assert.Contains("AddLocationStore(new ZLinkRedisLocationStore", hostFactory, StringComparison.Ordinal);
        Assert.Contains("redis.ConnectionString = topology.RedisEndpoint", hostFactory, StringComparison.Ordinal);
        Assert.Contains("redis.KeyPrefix = topology.RedisKeyPrefix", hostFactory, StringComparison.Ordinal);
    }

    private static void AssertSampleUsesProtobufPayloads(string sampleRoot)
    {
        var sourceFiles = EnumerateSourceFiles(sampleRoot).ToArray();
        var projectFiles = Directory
            .EnumerateFiles(sampleRoot, "*.csproj", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal))
            .ToArray();
        var protoFiles = Directory
            .EnumerateFiles(sampleRoot, "*.proto", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal))
            .ToArray();
        var allText = string.Join(
            Environment.NewLine,
            sourceFiles.Concat(projectFiles).Concat(protoFiles).Select(File.ReadAllText));
        var sharedProject = Path.Combine(sampleRoot, "Shared", "Bingo.Shared.csproj");
        var sharedProjectText = File.ReadAllText(sharedProject);
        var sharedContractSourceText = string.Join(
            Environment.NewLine,
            Directory
                .EnumerateFiles(Path.Combine(sampleRoot, "Shared", "Contracts"), "*.cs", SearchOption.AllDirectories)
                .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                          StringComparison.Ordinal)
                                      && !path.Contains(
                                          $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                          StringComparison.Ordinal))
                .Select(File.ReadAllText));

        Assert.NotEmpty(protoFiles);
        Assert.Contains("Google.Protobuf", sharedProjectText, StringComparison.Ordinal);
        Assert.Contains("Grpc.Tools", sharedProjectText, StringComparison.Ordinal);
        Assert.Contains("Protobuf Include=\"Contracts\\bingo_messages.proto\"", sharedProjectText, StringComparison.Ordinal);
        Assert.Contains("GrpcServices=\"None\"", sharedProjectText, StringComparison.Ordinal);
        Assert.DoesNotContain("record ", sharedContractSourceText, StringComparison.Ordinal);
        Assert.DoesNotContain("class AuthenticateReq", sharedContractSourceText, StringComparison.Ordinal);
        Assert.DoesNotContain("class BingoRoomJoinReq", sharedContractSourceText, StringComparison.Ordinal);
        Assert.Contains("ZLinkProtobufCodec.Default", allText, StringComparison.Ordinal);
        Assert.Contains("Zlink.Framework.Codecs.Protobuf", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("AddProtobuf", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("Stream.Connector.Protobuf", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("Zlink.Codecs.Protobuf", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("MessagePack", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("MsgPack", allText, StringComparison.Ordinal);
    }

    private static void AssertSampleUsesJsonPayloads(string sampleRoot)
    {
        var sourceFiles = EnumerateSourceFiles(sampleRoot).ToArray();
        var projectFiles = Directory
            .EnumerateFiles(sampleRoot, "*.csproj", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal))
            .ToArray();
        var protoFiles = Directory
            .EnumerateFiles(sampleRoot, "*.proto", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal))
            .ToArray();
        var allText = string.Join(
            Environment.NewLine,
            sourceFiles.Concat(projectFiles).Select(File.ReadAllText));

        Assert.Empty(protoFiles);
        Assert.DoesNotContain("Stream.Connector.Json", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("Zlink.Codecs.Json", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("Google.Protobuf", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("Grpc.Tools", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("AddProtobuf", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("FromProto", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("ToProto", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("MessagePackObject", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("AddMessagePack", allText, StringComparison.Ordinal);
    }

    private static void AssertCodecHelpersStayConfinedToRawLifecycleBoundaries(string sampleRoot)
    {
        var sampleName = Path.GetFileName(sampleRoot);
        var violations = new List<string>();
        foreach (var file in EnumerateSourceFiles(sampleRoot))
        {
            var text = File.ReadAllText(file);
            if (!ContainsRawCodecHelper(text)) continue;

            var relative = Path.GetRelativePath(sampleRoot, file).Replace('\\', '/');
            if (!IsAllowedRawCodecLifecycleFile(sampleName, relative)) violations.Add($"{sampleName}/{relative}");
        }

        Assert.Empty(violations.Order(StringComparer.Ordinal));
    }

    private static bool ContainsRawCodecHelper(string text)
    {
        return text.Contains(".ToJson()", StringComparison.Ordinal)
               || text.Contains(".FromJson<", StringComparison.Ordinal)
               || text.Contains(".ToProto()", StringComparison.Ordinal)
               || text.Contains(".FromProto<", StringComparison.Ordinal);
    }

    private static bool IsAllowedRawCodecLifecycleFile(string sampleName, string relative)
    {
        return (sampleName, relative) switch
        {
            ("Bingo", "Server/Session/Sessions/Handlers/AuthenticateSessionHandler.cs") => true,
            ("Bingo", "Server/Play/Application/RoomAllocation/BingoRoomAllocator.cs") => true,
            ("Bingo", "Server/Play/Infrastructure/ZLink/Handlers/AllocateBingoRoomHandler.cs") => true,
            ("Bingo", "Server/Play/Adapters/ZLink/Spots/BingoRoom.cs") => true,
            ("Bingo", "Server/Play/Adapters/ZLink/Spots/Handlers/MatchBingoActorHandler.cs") => true,
            ("Bingo", "Server/Play/Infrastructure/ZLink/Spots/BingoRoom.cs") => true,
            ("Bingo", "Server/Play/Infrastructure/ZLink/Spots/BingoRoomSettingsPayloadMapper.cs") => true,
            ("Bingo", "Server/Play/Infrastructure/ZLink/Spots/Handlers/MatchBingoActorHandler.cs") => true,
            ("Bingo", "Server/Play/Infrastructure/ZLink/Spots/Handlers/ObserveBingoEventsHandler.cs") => true,
            ("TicTacToe", "Server/Play/Adapters/ZLink/Spots/TicTacToeGame.cs") => true,
            ("TicTacToe", "Server/Play/Adapters/ZLink/Spots/Handlers/PlayActorJoinGameHandler.cs") => true,
            ("TicTacToe", "Server/Play/Infrastructure/ZLink/Spots/TicTacToeGame.cs") => true,
            ("TicTacToe", "Server/Play/Infrastructure/ZLink/Spots/Handlers/PlayActorJoinGameHandler.cs") => true,
            _ => false
        };
    }

    private static void AssertNoSampleRouteStore(string sampleRoot)
    {
        var sourceFiles = EnumerateSourceFiles(sampleRoot).ToArray();
        var fileNames = sourceFiles.Select(Path.GetFileName).ToHashSet(StringComparer.Ordinal);

        Assert.DoesNotContain("RegistryRemoteAddressStore.cs", fileNames);
        Assert.DoesNotContain("RegistryRemoteAddressPublisher.cs", fileNames);
        Assert.DoesNotContain("SpotRouteContracts.cs", fileNames);

        foreach (var file in sourceFiles)
        {
            var text = File.ReadAllText(file);
            Assert.DoesNotContain("RegistryRemoteAddressStore", text, StringComparison.Ordinal);
            Assert.DoesNotContain("RegistryRemoteAddressPublisher", text, StringComparison.Ordinal);
            Assert.DoesNotContain("AddActorRemoteAddressResolver<RegistryRemoteAddressStore>", text,
                StringComparison.Ordinal);
            Assert.DoesNotContain("AddSpotRouteRefResolver<RegistryRemoteAddressStore>", text,
                StringComparison.Ordinal);
            Assert.DoesNotContain("BindInitialActorRemoteAddressesAsync", text, StringComparison.Ordinal);
        }
    }

    private static void AssertNoSampleMetadataStore(string sampleRoot)
    {
        var sourceFiles = EnumerateSourceFiles(sampleRoot).ToArray();
        var fileNames = sourceFiles.Select(Path.GetFileName).ToHashSet(StringComparer.Ordinal);

        Assert.DoesNotContain("ActorSessionLocationStore.cs", fileNames);
        Assert.DoesNotContain("FileRegistryDiscoveryMetadata.cs", fileNames);
        Assert.DoesNotContain("InMemoryRegistryDiscoveryMetadata.cs", fileNames);
        Assert.DoesNotContain("IRegistryDiscoveryMetadata.cs", fileNames);
        Assert.DoesNotContain("RegistryMetadataEntry.cs", fileNames);
        Assert.DoesNotContain("SampleRuntime.cs", fileNames);

        foreach (var file in sourceFiles)
        {
            var text = File.ReadAllText(file);
            Assert.DoesNotContain("RegistryActorSessionLocationStore", text, StringComparison.Ordinal);
            Assert.DoesNotContain("IRegistryDiscoveryMetadata", text, StringComparison.Ordinal);
            Assert.DoesNotContain("FileRegistryDiscoveryMetadata", text, StringComparison.Ordinal);
            Assert.DoesNotContain("OpenRegistryMetadata", text, StringComparison.Ordinal);
            Assert.DoesNotContain("AddActorSessionBindingStore", text, StringComparison.Ordinal);
            Assert.DoesNotContain("IZLinkActorSessionClient", text, StringComparison.Ordinal);
            Assert.DoesNotContain("AddActorSessionBindingStore<RegistryActorSessionLocationStore>", text,
                StringComparison.Ordinal);
        }
    }

    private static void AssertNoSampleSessionRelayJson(string sampleRoot)
    {
        var sourceFiles = EnumerateSourceFiles(sampleRoot).ToArray();
        var fileNames = sourceFiles.Select(Path.GetFileName).ToHashSet(StringComparer.Ordinal);

        Assert.DoesNotContain("SessionRelayJson.cs", fileNames);
        foreach (var file in sourceFiles)
        {
            var text = File.ReadAllText(file);
            Assert.DoesNotContain("SessionRelayJson", text, StringComparison.Ordinal);
        }
    }

    private static void AssertSessionPayloadPolicy(string sampleRoot)
    {
        var sessionRoots = EnumerateSessionRoots(sampleRoot).ToArray();
        Assert.NotEmpty(sessionRoots);

        var sourceFiles = sessionRoots
            .SelectMany(static root => EnumerateSourceFiles(root))
            .ToArray();
        Assert.NotEmpty(sourceFiles);

        foreach (var file in sourceFiles)
        {
            var text = File.ReadAllText(file);
            Assert.DoesNotContain("payload.FromJson<", text, StringComparison.Ordinal);
            Assert.DoesNotContain("payload.Move()", text, StringComparison.Ordinal);
            Assert.DoesNotContain("using (payload)", text, StringComparison.Ordinal);
            Assert.DoesNotContain("await using (payload)", text, StringComparison.Ordinal);
            Assert.DoesNotContain(".Dispose()", text, StringComparison.Ordinal);
            Assert.DoesNotContain("IZLinkSessionActor? Actor", text, StringComparison.Ordinal);
            Assert.DoesNotContain("Actor { get; set; }", text, StringComparison.Ordinal);
        }
    }

    private static void AssertUsesAutoRegisteredSessionHandlers(string sampleRoot)
    {
        var sourceFiles = EnumerateSourceFiles(Path.Combine(sampleRoot, "Server", "Session")).ToArray();
        var allText = string.Join(Environment.NewLine, sourceFiles.Select(File.ReadAllText));

        Assert.Contains("IZLinkSessionPacketHandler<", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("Context.Handlers.AddHandler<", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("IZLinkSessionPacketDispatcher<", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("PacketName =>", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("IBingoSessionHandler", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("ISessionRelayPacketHandler", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("ToDictionary(static handler => handler.PacketName", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("BingoSessionContext", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("SessionRelayPacketContext", allText, StringComparison.Ordinal);
        Assert.DoesNotContain("SessionRelayState", allText, StringComparison.Ordinal);
    }

    private static void AssertSessionServerUsesSessionRelay(string sampleRoot)
    {
        var sessionHostFactory = Directory
            .EnumerateFiles(Path.Combine(sampleRoot, "Server", "Session"), "*HostFactory.cs",
                SearchOption.AllDirectories)
            .Single();
        var text = File.ReadAllText(sessionHostFactory);

        Assert.Contains("AddRouteMesh", text, StringComparison.Ordinal);
        Assert.Contains(".Listen(", text, StringComparison.Ordinal);
        Assert.Contains(".EnableActorDispatch(", text, StringComparison.Ordinal);
        Assert.DoesNotContain("AddScoped<IBingoSessionHandler", text, StringComparison.Ordinal);
        Assert.DoesNotContain("AddScoped<ISessionRelayPacketHandler", text, StringComparison.Ordinal);
    }

    private static void AssertSessionHandlersDoNotResolveActorRemoteAddresses(string sampleRoot)
    {
        var sessionRoot = Path.Combine(sampleRoot, "Server", "Session");
        var sourceFiles = EnumerateSourceFiles(sessionRoot)
            .Where(static file => file.EndsWith("Handler.cs", StringComparison.Ordinal))
            .ToArray();

        Assert.NotEmpty(sourceFiles);

        foreach (var file in sourceFiles)
        {
            var text = File.ReadAllText(file);
            Assert.DoesNotContain("IZLinkActorRemoteAddressResolver", text, StringComparison.Ordinal);
            Assert.DoesNotContain("ResolveActorRemoteAddressAsync", text, StringComparison.Ordinal);
        }
    }

    private static void AssertEnsureActorHandlersReturnSessionRelayRemoteAddresses(string sampleRoot)
    {
        var sessionRoot = Path.Combine(sampleRoot, "Server", "Session");
        var sourceFiles = EnumerateSourceFiles(sessionRoot)
            .Where(static file => Path.GetFileName(file).Equals(
                "AuthenticateSessionHandler.cs",
                StringComparison.Ordinal))
            .ToArray();

        Assert.NotEmpty(sourceFiles);

        foreach (var file in sourceFiles)
        {
            var text = File.ReadAllText(file);
            Assert.DoesNotContain("IZLinkActorRemoteAddressResolver", text, StringComparison.Ordinal);
            Assert.DoesNotContain("ResolveActorRemoteAddressAsync", text, StringComparison.Ordinal);
            Assert.DoesNotContain("GetRemoteAddressAsync", text, StringComparison.Ordinal);
            Assert.Contains(".GetOrCreate(authenticated.ActorId, SampleNames.PlayerActorType)", text,
                StringComparison.Ordinal);
            Assert.Contains("context.Actors.BindOrGetAsync", text, StringComparison.Ordinal);
            Assert.Contains("ZLinkActorCreateResult.Existing value => value.Actor", text,
                StringComparison.Ordinal);
            Assert.Contains("ZLinkActorCreateResult.Created value => value.Actor", text,
                StringComparison.Ordinal);
            Assert.DoesNotContain("ActorRefWire", text, StringComparison.Ordinal);
        }
    }

    private static void AssertActorLifecycleSpec(
        string sampleRoot,
        string entrySpotRelativePath,
        string userSpotRelativePath,
        string actorRelativePath,
        string sessionRelativePath)
    {
        var entrySpot = File.ReadAllText(Path.Combine(sampleRoot, entrySpotRelativePath));
        var userSpot = File.ReadAllText(Path.Combine(sampleRoot, userSpotRelativePath));
        var actor = File.ReadAllText(Path.Combine(sampleRoot, actorRelativePath));
        var session = File.ReadAllText(Path.Combine(sampleRoot, sessionRelativePath));

        Assert.Contains("OnCreateActorAsync", entrySpot, StringComparison.Ordinal);
        Assert.Contains("OnJoinedActorAsync", entrySpot, StringComparison.Ordinal);
        Assert.Contains("OnLeaveActorAsync", entrySpot, StringComparison.Ordinal);
        Assert.Contains("OnDisconnectActorAsync", entrySpot, StringComparison.Ordinal);
        Assert.Contains("DestroyActorAsync", entrySpot, StringComparison.Ordinal);
        Assert.Contains("DestroyAfterEntrySpotJoin", entrySpot, StringComparison.Ordinal);
        Assert.Contains("MarkDisconnected", entrySpot, StringComparison.Ordinal);

        Assert.Contains("OnLeaveActorAsync", userSpot, StringComparison.Ordinal);
        Assert.Contains("OnDisconnectActorAsync", userSpot, StringComparison.Ordinal);
        Assert.Contains("LeaveActorAsync", userSpot, StringComparison.Ordinal);
        Assert.Contains("MarkForDestroyAfterRoomLeave", userSpot, StringComparison.Ordinal);
        Assert.Contains("MarkDisconnected", userSpot, StringComparison.Ordinal);
        Assert.DoesNotContain("DestroyActorAsync", userSpot, StringComparison.Ordinal);

        Assert.Contains("DestroyAfterEntrySpotJoin", actor, StringComparison.Ordinal);
        Assert.Contains("MarkForDestroyAfterRoomLeave", actor, StringComparison.Ordinal);
        Assert.Contains("MarkDisconnected", actor, StringComparison.Ordinal);
        Assert.Contains("Disconnected", actor, StringComparison.Ordinal);

        Assert.Contains("OnDisconnectedAsync", session, StringComparison.Ordinal);
        Assert.Contains("NotifyDisconnectedAsync", session, StringComparison.Ordinal);
    }

    private static IEnumerable<string> EnumerateSourceFiles(string root)
    {
        return Directory
            .EnumerateFiles(root, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal));
    }

    private static IEnumerable<string> EnumerateMarkdownFiles(string root)
    {
        return Directory
            .EnumerateFiles(root, "*.md", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal)
                                  && !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                                      StringComparison.Ordinal));
    }

    private static IEnumerable<string> EnumerateSessionRoots(string sampleRoot)
    {
        var sessionRoot = Path.Combine(sampleRoot, "Server", "Session");
        if (Directory.Exists(sessionRoot)) yield return sessionRoot;

        var playSessionsRoot = Path.Combine(sampleRoot, "Server", "Play", "Sessions");
        if (Directory.Exists(playSessionsRoot)) yield return playSessionsRoot;

        var adapterSessionsRoot = Path.Combine(
            sampleRoot,
            "Server",
            "Play",
            "Adapters",
            "ZLink",
            "Sessions");
        if (Directory.Exists(adapterSessionsRoot)) yield return adapterSessionsRoot;

        var infrastructureSessionsRoot = Path.Combine(
            sampleRoot,
            "Server",
            "Play",
            "Infrastructure",
            "ZLink",
            "Sessions");
        if (Directory.Exists(infrastructureSessionsRoot)) yield return infrastructureSessionsRoot;
    }

    private static string ResolveSampleRoot(string sampleName)
    {
        return Path.Combine(ResolveSamplesRoot(), sampleName);
    }

    private static string ResolveSamplesRoot()
    {
        return Path.Combine(ResolveDotnetRoot(), "samples");
    }

    private static string ResolveE2eRoot()
    {
        return Path.Combine(ResolveDotnetRoot(), "e2e");
    }

    [Fact]
    public void Samples_And_E2E_Use_ZLinkHttpClient_Not_Raw_HttpClient()
    {
        // 규약: 샘플·e2e의 HTTP 클라이언트는 Zlink.HttpClient(ZLinkHttpClient)만 쓴다.
        // raw System.Net.Http.HttpClient 인스턴스화는 규약 위반이다.
        var root = ResolveDotnetRoot();
        var offenders = new List<string>();
        foreach (var dir in new[] { "samples", "e2e" })
        {
            var basePath = Path.Combine(root, dir);
            if (!Directory.Exists(basePath)) continue;
            foreach (var file in Directory.EnumerateFiles(basePath, "*.cs", SearchOption.AllDirectories))
            {
                if (file.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}")
                    || file.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}"))
                    continue;
                if (File.ReadAllText(file).Contains("new HttpClient"))
                    offenders.Add(NormalizeRelativePath(Path.GetRelativePath(root, file)));
            }
        }

        Assert.Empty(offenders);
    }

    private static string ResolveDotnetRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);

        while (current is not null)
        {
            var candidate = Path.Combine(
                current.FullName,
                "framework",
                "languages",
                "dotnet",
                "samples");

            if (Directory.Exists(candidate)) return Directory.GetParent(candidate)!.FullName;

            current = current.Parent;
        }

        throw new DirectoryNotFoundException(
            "Could not find framework/languages/dotnet/samples from test runtime.");
    }

    private static string NormalizeRelativePath(string path)
    {
        return path.Replace(Path.DirectorySeparatorChar, '/');
    }

}
