using Xunit;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Builder;
using Microsoft.Extensions.Configuration;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    private sealed class PolicyProbeOptions
    {
        public string Value { get; init; } = string.Empty;
    }

    [Fact]
    public void FrameworkHostBuildersRemoveDefaultConfigurationProviders()
    {
        var roots = new[]
        {
            Path.Combine(ResolveDotnetRoot(), "samples"),
            Path.Combine(ResolveDotnetRoot(), "e2e")
        };
        var hostBuilderPattern = new Regex(
            @"(?:WebApplication|Host)\.Create(?:Application|Default)?Builder\(",
            RegexOptions.CultureInvariant);
        var builderCount = 0;

        foreach (var root in roots)
            foreach (var sourcePath in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories)
                         .Where(static path => !path.Contains(
                             $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
                         .Where(static path => !path.Contains(
                             $"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}")))
            {
                var source = File.ReadAllText(sourcePath);
                foreach (Match match in hostBuilderPattern.Matches(source))
                {
                    builderCount++;
                    var boundary = Math.Min(source.Length, match.Index + 320);
                    var initialization = source[match.Index..boundary];
                    Assert.Contains(".Sources.Clear()", initialization, StringComparison.Ordinal);
                    if (!initialization.StartsWith("Host.CreateDefaultBuilder", StringComparison.Ordinal))
                        Assert.Contains("Configuration.AddInMemoryCollection()", initialization,
                            StringComparison.Ordinal);
                }
            }

        Assert.True(builderCount > 0);
    }

    [Fact]
    public void ClearedHostConfigurationIgnoresSameNameEnvironmentAndCommandLineValues()
    {
        const string environmentKey = "PolicyProbe__Value";
        var previous = Environment.GetEnvironmentVariable(environmentKey);
        try
        {
            Environment.SetEnvironmentVariable(environmentKey, "environment");
            var builder = WebApplication.CreateBuilder(["--PolicyProbe:Value=command-line"]);
            builder.Configuration.Sources.Clear();
            builder.Configuration.AddInMemoryCollection();
            builder.Configuration.AddInMemoryCollection(new Dictionary<string, string?>
            {
                ["PolicyProbe:Value"] = "configuration-file"
            });

            var options = builder.Configuration.GetSection("PolicyProbe").Get<PolicyProbeOptions>();

            Assert.NotNull(options);
            Assert.Equal("configuration-file", options.Value);
        }
        finally
        {
            Environment.SetEnvironmentVariable(environmentKey, previous);
        }
    }

    [Fact]
    public void DynamicDotnetLaunchersUsePrebuiltProjectsAndUniqueConfigurationArtifacts()
    {
        var e2eRoot = Path.Combine(ResolveDotnetRoot(), "e2e");
        var launchers = Directory.EnumerateFiles(e2eRoot, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains(
                $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
            .Select(path => (Path: path, Source: File.ReadAllText(path)))
            .Where(static file => file.Source.Contains("ArgumentList.Add(\"run\")", StringComparison.Ordinal))
            .ToArray();

        Assert.NotEmpty(launchers);
        foreach (var (path, source) in launchers)
            foreach (Match match in Regex.Matches(source, "ArgumentList\\.Add\\(\\\"run\\\"\\)"))
            {
                var boundary = Math.Min(source.Length, match.Index + 280);
                var launch = source[match.Index..boundary];
                var noBuildIndex = launch.IndexOf(
                    "ArgumentList.Add(\"--no-build\")",
                    StringComparison.Ordinal);
                var projectIndex = launch.IndexOf(
                    "ArgumentList.Add(\"--project\")",
                    StringComparison.Ordinal);
                Assert.True(
                    noBuildIndex >= 0 && projectIndex >= 0 && noBuildIndex < projectIndex,
                    $"{path} must launch the project already built by its runner.");
            }

        var pubSubLauncher = File.ReadAllText(Path.Combine(
            e2eRoot, "PubSub", "Client", "Support", "ServerProcessLauncher.cs"));
        Assert.Contains(
            "CreateServerStartInfo(options.PublisherProject, \"pub-restart\"",
            pubSubLauncher,
            StringComparison.Ordinal);

        var locationLauncher = File.ReadAllText(Path.Combine(
            e2eRoot, "LocationMessaging", "Client", "Support", "DynamicClusterLauncher.cs"));
        Assert.Contains("scenarioConfigDir", locationLauncher, StringComparison.Ordinal);
        Assert.Contains("scenarioLogDir", locationLauncher, StringComparison.Ordinal);
        Assert.Contains("var processName = $\"{scenarioName}-{name}\"", locationLauncher,
            StringComparison.Ordinal);
    }

    [Fact]
    public void DynamicServerReadinessUsesTheThreeSecondLocalBound()
    {
        var e2eRoot = Path.Combine(ResolveDotnetRoot(), "e2e");
        var pubSub = File.ReadAllText(Path.Combine(
            e2eRoot, "PubSub", "Client", "Support", "StateObservation.cs"));
        Assert.Contains("ReadinessTimeout = TimeSpan.FromSeconds(3)", pubSub, StringComparison.Ordinal);
        Assert.Contains("ReadinessPollInterval = TimeSpan.FromMilliseconds(100)", pubSub,
            StringComparison.Ordinal);
        Assert.DoesNotContain("TimeSpan.FromSeconds(20)", pubSub, StringComparison.Ordinal);
        Assert.DoesNotContain("ContinueWith", pubSub, StringComparison.Ordinal);

        foreach (var path in new[]
                 {
                     Path.Combine(e2eRoot, "ResilienceLifecycle", "Client", "Support",
                         "ResilienceProcessManager.cs"),
                     Path.Combine(e2eRoot, "StoreFailure", "Client", "Support",
                         "StoreFailureProcessManager.cs"),
                     Path.Combine(e2eRoot, "LocationMessaging", "Client", "Support",
                         "DynamicClusterLauncher.cs")
                 })
        {
            var source = File.ReadAllText(path);
            Assert.Contains("ReadinessTimeout = TimeSpan.FromSeconds(3)", source, StringComparison.Ordinal);
            Assert.Contains("ReadinessPollInterval = TimeSpan.FromMilliseconds(100)", source,
                StringComparison.Ordinal);
            Assert.DoesNotContain("for (var i = 0; i < 120", source, StringComparison.Ordinal);
            Assert.Contains("error.Kind is ZLinkFrameworkErrorKind.Unavailable", source, StringComparison.Ordinal);
            Assert.Contains("or ZLinkFrameworkErrorKind.DeadlineExceeded", source, StringComparison.Ordinal);
            Assert.DoesNotContain("error.RetryAdvice", source, StringComparison.Ordinal);
        }

        var monitoring = File.ReadAllText(Path.Combine(
            e2eRoot, "RuntimeMonitoring", "Client", "Scenarios", "MonD1FailureRecoveryScenario.cs"));
        Assert.Contains("for (var attempt = 0; attempt < 30; attempt++)", monitoring,
            StringComparison.Ordinal);
        Assert.DoesNotContain("for (var attempt = 0; attempt < 100; attempt++)", monitoring,
            StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("Bingo")]
    [InlineData("DeliveryDispatch")]
    [InlineData("GameQuest")]
    [InlineData("ShoppingMall")]
    [InlineData("SupportChat")]
    [InlineData("TicTacToe")]
    [InlineData("ZoneWorld")]
    public void CanonicalSampleApplicationCodeDoesNotReadEnvironmentVariables(string sampleName)
    {
        var sampleRoot = ResolveSampleRoot(sampleName);
        var sourceFiles = Directory.EnumerateFiles(sampleRoot, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}"));

        foreach (var sourceFile in sourceFiles)
        {
            var source = File.ReadAllText(sourceFile);
            Assert.DoesNotContain("Environment.GetEnvironmentVariable", source, StringComparison.Ordinal);
            Assert.DoesNotContain("DirectoryFromEnvironment", source, StringComparison.Ordinal);
        }
    }

    [Theory]
    [InlineData("Bingo")]
    [InlineData("DeliveryDispatch")]
    [InlineData("GameQuest")]
    [InlineData("ShoppingMall")]
    [InlineData("SupportChat")]
    [InlineData("TicTacToe")]
    [InlineData("ZoneWorld")]
    public void CanonicalSampleServersDoNotAcceptIndividualConfigurationOptions(string sampleName)
    {
        var serverRoot = Path.Combine(ResolveSampleRoot(sampleName), "Server");
        var sourceFiles = Directory.EnumerateFiles(serverRoot, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"));

        foreach (var sourceFile in sourceFiles)
        {
            var source = File.ReadAllText(sourceFile);
            Assert.DoesNotContain("\"--node\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--instance\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--role\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--mode\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--redis-endpoint\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--redis-key-prefix\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--log-dir\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("FromEnvironment", source, StringComparison.Ordinal);
        }
    }

    [Theory]
    [InlineData("Bingo")]
    [InlineData("DeliveryDispatch")]
    [InlineData("GameQuest")]
    [InlineData("ShoppingMall")]
    [InlineData("SupportChat")]
    [InlineData("TicTacToe")]
    [InlineData("ZoneWorld")]
    public void CanonicalSampleConfigurationLoadersUseIConfigurationBinding(string sampleName)
    {
        var configurationRoot = Path.Combine(ResolveSampleRoot(sampleName), "Server", "Configuration");
        var loaders = Directory.EnumerateFiles(configurationRoot, "*.cs", SearchOption.AllDirectories)
            .Select(File.ReadAllText)
            .Where(static source => source.Contains("\"--config\"", StringComparison.Ordinal))
            .ToArray();

        Assert.NotEmpty(loaders);
        foreach (var source in loaders)
        {
            Assert.Contains("ConfigurationBuilder", source, StringComparison.Ordinal);
            Assert.Contains("AddJsonFile", source, StringComparison.Ordinal);
            Assert.DoesNotContain("JsonSerializer.Deserialize", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void AllE2eApplicationsUseTypedFileConfigurationWithoutEnvironmentAccess()
    {
        var e2eRoot = Path.Combine(ResolveDotnetRoot(), "e2e");
        var sourceFiles = Directory.EnumerateFiles(e2eRoot, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}"));

        foreach (var sourceFile in sourceFiles)
        {
            var source = File.ReadAllText(sourceFile);
            Assert.DoesNotContain("Environment.GetEnvironmentVariable", source, StringComparison.Ordinal);
            Assert.DoesNotContain("Environment.SetEnvironmentVariable", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddEnvironmentVariables", source, StringComparison.Ordinal);
            Assert.DoesNotContain("StartsWith(\"--\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("TrimStart('-')", source, StringComparison.Ordinal);
            Assert.DoesNotContain("WriteArguments", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--redis-endpoint\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--redis-key-prefix\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--log-dir\"", source, StringComparison.Ordinal);
            Assert.DoesNotContain("\"--role\"", source, StringComparison.Ordinal);
        }

        var runners = Directory.EnumerateFiles(e2eRoot, "run_e2e.sh", SearchOption.AllDirectories);
        foreach (var runner in runners)
        {
            var source = File.ReadAllText(runner);
            Assert.Contains("umask 077", source, StringComparison.Ordinal);
            Assert.Contains("CONFIG_DIR=\"$(mktemp -d)\"", source, StringComparison.Ordinal);
            Assert.Contains("rm -rf \"$CONFIG_DIR\"", source, StringComparison.Ordinal);
            Assert.Contains("write_role_config.py", source, StringComparison.Ordinal);
            Assert.DoesNotContain("env ZLINK_E2E_RID", source, StringComparison.Ordinal);
            Assert.DoesNotContain("ZLINK_DEBUG_FRAMEWORK_", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void PowerShellSampleRunnerCannotRemoveRedisBySharedPrefix()
    {
        var samplesRoot = Path.Combine(ResolveDotnetRoot(), "samples");
        var helper = File.ReadAllText(Path.Combine(samplesRoot, "sample_runner.ps1"));
        var aggregate = File.ReadAllText(Path.Combine(samplesRoot, "run_samples.ps1"));

        Assert.DoesNotContain("Remove-SampleRedisScope", helper, StringComparison.Ordinal);
        Assert.DoesNotContain("Remove-SampleRedisScope", aggregate, StringComparison.Ordinal);
        Assert.Contains("Remove-SampleRedisContainer", helper, StringComparison.Ordinal);
    }

    [Fact]
    public void SpotServiceScenariosDoNotRetryConnectOrRequests()
    {
        var scenarios = Path.Combine(ResolveDotnetRoot(), "e2e", "SpotService", "Client", "Scenarios");
        var source = string.Join('\n', Directory.EnumerateFiles(scenarios, "*.cs")
            .Select(File.ReadAllText));

        Assert.DoesNotContain("Actor auth did not become routable", source, StringComparison.Ordinal);
        Assert.DoesNotContain("Last error:", source, StringComparison.Ordinal);

        var reconnect = File.ReadAllText(Path.Combine(scenarios, "SmD8StreamReconnectRecoveryScenario.cs"));
        var slowHandler = File.ReadAllText(Path.Combine(ResolveDotnetRoot(), "e2e", "SpotService", "Server",
            "Play", "Handlers", "PlayActorHandlers.cs"));
        Assert.DoesNotContain("Task.Delay", reconnect, StringComparison.Ordinal);
        Assert.Contains("actor-slow-ping-started", reconnect, StringComparison.Ordinal);
        Assert.Contains("actor-slow-ping-started", slowHandler, StringComparison.Ordinal);
    }

    [Fact]
    public void ServerConfigurationErrorsDoNotAdvertiseRemovedCliOptions()
    {
        var e2eRoot = Path.Combine(ResolveDotnetRoot(), "e2e");
        foreach (var configuration in new[]
                 {
                     "LocationMessaging", "RuntimeMonitoring", "RegistrationCodec",
                     "SpotService", "StoreFailure", "ResilienceLifecycle"
                 })
        {
            var serverRoot = Path.Combine(e2eRoot, configuration, "Server");
            var source = string.Join('\n', Directory.EnumerateFiles(serverRoot, "*.cs", SearchOption.AllDirectories)
                .Where(path => !path.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}", StringComparison.Ordinal)
                               && !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}", StringComparison.Ordinal))
                .Select(File.ReadAllText));
            Assert.DoesNotMatch("\\\"--(?!config(?:\\\"|=))", source);
        }
    }

    [Fact]
    public void ZoneWorldRunnerExercisesPrefixUuidLifecycleWithProcessEvidence()
    {
        var sample = ResolveSampleRoot("ZoneWorld");
        var runner = File.ReadAllText(Path.Combine(sample, "run_sample.sh"));
        var reportHandler = File.ReadAllText(Path.Combine(sample, "Server", "Ops", "Infrastructure", "ZLink",
            "Handlers", "OpsReportHandlers.cs"));

        foreach (var id in new[] { "ZW-G1", "ZW-G2", "ZW-G3", "ZW-G4", "ZW-G5" })
            Assert.Contains(id, runner, StringComparison.Ordinal);
        Assert.Contains("routing_id_of", runner, StringComparison.Ordinal);
        Assert.Contains("is_zone_node_rid", runner, StringComparison.Ordinal);
        Assert.Contains("node status observed. node=", runner, StringComparison.Ordinal);
        Assert.Contains("ZW-G2-rid", runner, StringComparison.Ordinal);
        Assert.DoesNotContain("allocation_field", runner, StringComparison.Ordinal);
        Assert.DoesNotContain("WaitingForSlot", runner, StringComparison.Ordinal);
        Assert.DoesNotContain("zone node allocation ready", runner, StringComparison.Ordinal);
        Assert.DoesNotContain("sleep 2", runner, StringComparison.Ordinal);
        Assert.Contains("$0\" --g4-child ZW-G4", runner, StringComparison.Ordinal);
        Assert.Contains("if scenario_selected ZW-G3", runner, StringComparison.Ordinal);
        Assert.Contains("if scenario_selected ZW-G5", runner, StringComparison.Ordinal);
        Assert.Contains("config_name=\"zone-node-replacement\"", runner, StringComparison.Ordinal);
        Assert.Contains("start \"$name\" \"$SERVER_BIN\" --config \"$CONFIG_DIR/$config_name.json\"", runner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("stop_node zone-node-replacement", runner, StringComparison.Ordinal);
        Assert.Contains("run_client ZW-G2", runner, StringComparison.Ordinal);
        Assert.Contains("fixed_rid_scan_status", runner, StringComparison.Ordinal);
        Assert.True(
            runner.LastIndexOf("if scenario_selected ZW-G3", StringComparison.Ordinal)
            > runner.IndexOf("runner_scenario ZW-F2", StringComparison.Ordinal));
        Assert.Contains("tee -a \"$LOG_DIR/client.log\"", runner, StringComparison.Ordinal);
        Assert.Contains("\"$G_RUNNER_LOG\" 2>/dev/null", runner, StringComparison.Ordinal);
        Assert.Contains("remove_owned_pid \"$pid\"", runner, StringComparison.Ordinal);
        Assert.Equal(2, Regex.Matches(runner, "remove_owned_pid \\\"\\$pid\\\"").Count);
        Assert.Contains("if [[ \"$G4_PROVEN\" == \"1\" ]]; then g_pass ZW-G4; fi", runner,
            StringComparison.Ordinal);
        Assert.Contains("node status observed. node={NodeId}, rid={NodeRid}", reportHandler,
            StringComparison.Ordinal);
        // The handler narrows IZLinkMessageContext to ZLinkRouteMessageContext before
        // reading the source identity, so the read happens through that local.
        Assert.Contains("ZLinkRouteMessageContext", reportHandler, StringComparison.Ordinal);
        Assert.Contains("route.SourceNodeRid", reportHandler, StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorldScenariosUseConnectorWaitContractsDirectly()
    {
        var clientRoot = Path.Combine(ResolveSampleRoot("ZoneWorld"), "Client");
        var support = File.ReadAllText(Path.Combine(clientRoot, "ScenarioSupport.cs"));
        var scenarios = File.ReadAllText(Path.Combine(clientRoot, "Scenarios.cs"));

        Assert.DoesNotContain("WaitAsync<", support, StringComparison.Ordinal);
        Assert.DoesNotContain("MoveAndWait", support, StringComparison.Ordinal);
        Assert.DoesNotContain("CollectAsync<", support, StringComparison.Ordinal);
        Assert.DoesNotContain("WaitFor<", support, StringComparison.Ordinal);
        Assert.DoesNotContain("ExpectNone<", support, StringComparison.Ordinal);
        Assert.DoesNotContain("catch (Exception)", scenarios, StringComparison.Ordinal);
        Assert.Contains(".Connector.WaitFor<", scenarios, StringComparison.Ordinal);
        Assert.Contains(".Connector.ExpectNone<", scenarios, StringComparison.Ordinal);
        Assert.True(
            scenarios.IndexOf("var goneWait =", StringComparison.Ordinal)
            < scenarios.IndexOf("scenario ZW-C2 armed", StringComparison.Ordinal));
        Assert.True(
            scenarios.IndexOf("var droppedWait =", StringComparison.Ordinal)
            < scenarios.IndexOf("scenario ZW-C3 armed", StringComparison.Ordinal));
        Assert.True(
            scenarios.IndexOf("var expiredWait =", StringComparison.Ordinal)
            < scenarios.IndexOf("scenario ZW-B4 armed", StringComparison.Ordinal));

    }

    [Fact]
    public void ZoneWorldOpsReplaysNodeStateAcrossStreamSessionReplacement()
    {
        var sample = ResolveSampleRoot("ZoneWorld");
        var registry = File.ReadAllText(Path.Combine(sample, "Server", "Ops", "Infrastructure", "ZLink",
            "Sessions", "OpsConsoleRegistry.cs"));

        Assert.Contains("Dictionary<string, NodeStatusNotify> _latestNodes", registry,
            StringComparison.Ordinal);
        Assert.Contains("lock (_nodeGate)", registry, StringComparison.Ordinal);
        Assert.Contains("ReplayNodesAsync", registry, StringComparison.Ordinal);
        Assert.Contains("public void Add(", registry, StringComparison.Ordinal);
        Assert.DoesNotContain("AddAsync(", registry, StringComparison.Ordinal);
        Assert.Contains("catch (Exception error)", registry, StringComparison.Ordinal);
        Assert.Contains("logger?.LogWarning", registry, StringComparison.Ordinal);
        Assert.Contains("Remove(console)", registry, StringComparison.Ordinal);
        Assert.Contains("ICollection<KeyValuePair<string, IZLinkSessionContext>>", registry,
            StringComparison.Ordinal);
        Assert.Contains("await context.Client.Reply(new WatchNodesRes", File.ReadAllText(
            Path.Combine(sample, "Server", "Ops", "Infrastructure", "ZLink", "Handlers",
                "OpsSessionHandlers.cs")), StringComparison.Ordinal);
        Assert.True(
            registry.IndexOf("consoles = _consoles.Values.ToArray()", StringComparison.Ordinal)
            < registry.IndexOf("foreach (var console in consoles)", StringComparison.Ordinal));
    }

    [Fact]
    public void AutomaticTurnDispatchScenariosOwnTheirVerificationFlows()
    {
        var clientRoot = Path.Combine(ResolveDotnetRoot(), "e2e", "AutomaticTurnDispatch", "Client");
        var scenarios = Path.Combine(clientRoot, "Scenarios");
        Assert.False(File.Exists(Path.Combine(scenarios, "ExecutionTurnScenarioSuite.cs")));

        var context = File.ReadAllText(Path.Combine(scenarios, "ExecutionTurnScenarioContext.cs"));
        Assert.DoesNotMatch("Td[A-G][0-9]+Async", context);
        Assert.DoesNotMatch("(?:public|internal)\\s+IZlinkStreamConnector\\s+\\w+", context);

        var scenarioFiles = Directory.EnumerateFiles(scenarios, "Td*.cs").OrderBy(path => path).ToArray();
        Assert.Equal(32, scenarioFiles.Length);
        foreach (var path in scenarioFiles)
        {
            var source = File.ReadAllText(path);
            Assert.DoesNotMatch("=>\\s*(?:Td\\w+|\\w+Probe)\\.RunAsync", source);
            Assert.DoesNotMatch("RunAsync\\([^)]*\\)\\s*=>", source);
        }

        var program = File.ReadAllText(Path.Combine(clientRoot, "Program.cs"));
        foreach (var probe in Directory.EnumerateFiles(scenarios, "*Probe.cs"))
            Assert.Contains(Path.GetFileNameWithoutExtension(probe), program, StringComparison.Ordinal);
    }

    [Fact]
    public void E2eRunnersFailTheFirstExecutionInsteadOfRetryingBindFailures()
    {
        var e2eRoot = Path.Combine(ResolveDotnetRoot(), "e2e");
        foreach (var runnerPath in new[]
                 {
                     Path.Combine(e2eRoot, "run_e2e_all.sh"),
                     Path.Combine(e2eRoot, "SpotService", "run_e2e.sh")
                 })
        {
            var runner = File.ReadAllText(runnerPath);
            Assert.DoesNotContain("BIND_RETRY_PATTERN", runner, StringComparison.Ordinal);
            Assert.DoesNotContain("retry after transient bind failure", runner, StringComparison.Ordinal);
            Assert.DoesNotContain("retrying child", runner, StringComparison.Ordinal);
            Assert.DoesNotContain("--max-attempts", runner, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void SpotActorTransferSourceDownAssertionCannotBeCaughtAsTransportFailure()
    {
        var scenario = File.ReadAllText(Path.Combine(
            ResolveDotnetRoot(), "e2e", "SpotActorTransfer", "Client", "Scenarios",
            "StC1SourceDownBeforeCommitScenario.cs"));

        Assert.DoesNotContain("TimeoutException or InvalidOperationException", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Delay", scenario, StringComparison.Ordinal);
        Assert.Contains("if (response is not null)", scenario, StringComparison.Ordinal);
        Assert.Contains("ZlinkStreamAssert.Ensure(response.Accepted", scenario, StringComparison.Ordinal);
        Assert.Contains("pending_admission_expired actor={actorId}", scenario, StringComparison.Ordinal);
        Assert.Contains("WaitRuntimeEvidenceAsync(context.NodeB, 30000", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("DrainAsync(context.NodeB)", scenario, StringComparison.Ordinal);

        var clientRoot = Path.Combine(ResolveDotnetRoot(), "e2e", "SpotActorTransfer", "Client");
        var clientSource = string.Join('\n', Directory.EnumerateFiles(clientRoot, "*.cs", SearchOption.AllDirectories)
            .Select(File.ReadAllText));
        Assert.DoesNotContain("WaitBoundPushAsync", clientSource, StringComparison.Ordinal);
        Assert.DoesNotContain(".PacketName(nameof(", clientSource, StringComparison.Ordinal);
        Assert.Contains(".WaitFor<BoundPushNotify>()", clientSource, StringComparison.Ordinal);
    }

    [Fact]
    public void E2eOptionalConstructorSettingsHaveExplicitDefaults()
    {
        var e2eRoot = Path.Combine(ResolveDotnetRoot(), "e2e");
        var optionRecords = Directory.EnumerateFiles(e2eRoot, "*.cs", SearchOption.AllDirectories)
            .Where(static path => !path.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
            .Select(path => (Path: path, Source: File.ReadAllText(path)))
            .Where(static file => file.Source.Contains("E2eConfiguration.Load<", StringComparison.Ordinal))
            .SelectMany(static file => Regex.Matches(
                    file.Source,
                    @"(?:internal|public)\s+sealed\s+record\s+\w+Options\s*\((?<parameters>.*?)\)\s*(?:\{|;)",
                    RegexOptions.Singleline)
                .Select(match => (file.Path, Parameters: match.Groups["parameters"].Value)));

        foreach (var (path, parameters) in optionRecords)
        {
            var missingDefaults = Regex.Matches(
                    parameters,
                    @"\b[\w<>]+\?\s+\w+\s*(?=,|$)",
                    RegexOptions.Multiline)
                .Select(static match => match.Value)
                .ToArray();
            Assert.True(
                missingDefaults.Length == 0,
                $"{path} has nullable constructor settings without '= null': {string.Join(", ", missingDefaults)}");
        }
    }

    [Fact]
    public void SampleRunnersDoNotExposeEnvironmentConfigurationFallbacks()
    {
        var forbidden = new[]
        {
            "SAMPLE_RUN_DIR",
            "KEEP_RUN_DIR",
            "BASE_PORT",
            "ZONEWORLD_BROWSER_SMOKE",
            "BINGO_API_A_CHANNEL_ENDPOINT:-"
        };

        foreach (var runner in Directory.EnumerateFiles(
                     Path.Combine(ResolveDotnetRoot(), "samples"),
                     "run_sample.sh",
                     SearchOption.AllDirectories))
        {
            var source = File.ReadAllText(runner);
            foreach (var marker in forbidden)
                Assert.DoesNotContain(marker, source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void IntegratedSampleRunnerIncludesEveryCommonSample()
    {
        var runner = File.ReadAllText(Path.Combine(
            ResolveDotnetRoot(), "samples", "run_samples.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(
            ResolveDotnetRoot(), "samples", "run_samples.ps1"));
        var expectedSamples = new[]
        {
            "TicTacToe",
            "Bingo",
            "SupportChat",
            "ShoppingMall",
            "DeliveryDispatch",
            "GameQuest",
            "ZoneWorld"
        };
        var defaultSampleList = runner.Split('\n').Single(static line =>
            line.StartsWith("SAMPLES=(", StringComparison.Ordinal));
        var defaultPowerShellSampleList = powershellRunner.Split('\n').Single(static line =>
            line.StartsWith("$knownSamples = @(", StringComparison.Ordinal));

        foreach (var sample in expectedSamples)
        {
            Assert.Contains(sample, defaultSampleList, StringComparison.Ordinal);
            Assert.Contains(sample, defaultPowerShellSampleList, StringComparison.Ordinal);
            Assert.True(
                File.Exists(Path.Combine(ResolveDotnetRoot(), "samples", sample, "run_sample.ps1")),
                $"PowerShell runner is missing for {sample}.");
        }

        Assert.Contains("${SCRIPT_DIR}/${sample}/run_sample.sh", runner, StringComparison.Ordinal);
        Assert.Contains("$ScriptDir \"$sample/run_sample.ps1\"", powershellRunner,
            StringComparison.Ordinal);

        var zoneWorldPowerShellRunner = File.ReadAllText(Path.Combine(
            ResolveDotnetRoot(), "samples", "ZoneWorld", "run_sample.ps1"));
        Assert.Contains("run_sample.sh", zoneWorldPowerShellRunner, StringComparison.Ordinal);
        Assert.Contains("bash", zoneWorldPowerShellRunner, StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorldBrowserLoadsRunnerProvidedStaticConfiguration()
    {
        var browserRoot = Path.GetFullPath(Path.Combine(
            ResolveDotnetRoot(), "..", "shared_sample", "zoneworld", "client"));
        var runtime = File.ReadAllText(Path.Combine(browserRoot, "src", "shared", "config", "runtime.ts"));
        var liveTest = File.ReadAllText(Path.Combine(browserRoot, "tests", "live", "server.spec.ts"));
        var runner = File.ReadAllText(Path.Combine(ResolveSampleRoot("ZoneWorld"), "run_sample.sh"));

        Assert.Contains("fetch('/config.json'", runtime, StringComparison.Ordinal);
        Assert.DoesNotContain("import.meta.env", runtime, StringComparison.Ordinal);
        Assert.DoesNotContain("location.search", runtime, StringComparison.Ordinal);
        Assert.DoesNotContain("process.env", liveTest, StringComparison.Ordinal);
        Assert.DoesNotContain("ZONEWORLD_", runner, StringComparison.Ordinal);
        Assert.Contains("browser_dist/config.json", runner, StringComparison.Ordinal);
    }

    [Fact]
    public void SampleAndE2eClientsUseTheConnectorAssertionSurface()
    {
        var roots = new[]
        {
            Path.Combine(ResolveDotnetRoot(), "samples"),
            Path.Combine(ResolveDotnetRoot(), "e2e")
        };
        foreach (var root in roots)
            foreach (var sourceFile in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories)
                         .Where(static path => path.Contains(
                             $"{Path.DirectorySeparatorChar}Client{Path.DirectorySeparatorChar}"))
                         .Where(static path => !path.Contains(
                             $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}")))
            {
                var source = File.ReadAllText(sourceFile);
                Assert.DoesNotContain("class ScenarioAssert", source, StringComparison.Ordinal);
                Assert.DoesNotContain("static class ScenarioAssert", source, StringComparison.Ordinal);
            }
    }

    [Fact]
    public void SampleAndE2eClientsDoNotSynchronouslyUnwrapAsyncOperations()
    {
        var roots = new[]
        {
            Path.Combine(ResolveDotnetRoot(), "samples"),
            Path.Combine(ResolveDotnetRoot(), "e2e")
        };
        foreach (var root in roots)
            foreach (var sourceFile in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories)
                         .Where(static path => path.Contains(
                             $"{Path.DirectorySeparatorChar}Client{Path.DirectorySeparatorChar}"))
                         .Where(static path => !path.Contains(
                             $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}")))
            {
                var source = File.ReadAllText(sourceFile);
                Assert.DoesNotContain(".AsTask().GetAwaiter().GetResult()", source, StringComparison.Ordinal);
            }
    }

    [Fact]
    public void ZoneWorldBotTimerAppliesBackpressureToActorMovement()
    {
        var zoneWorld = ResolveSampleRoot("ZoneWorld");
        var spot = File.ReadAllText(Path.Combine(
            zoneWorld,
            "Server", "ZoneNode", "Infrastructure", "ZLink", "Spots", "ZoneSpot.cs"));
        var handlers = File.ReadAllText(Path.Combine(
            zoneWorld,
            "Server", "ZoneNode", "Infrastructure", "ZLink", "Spots", "Handlers",
            "PlayerMoveHandlers.cs"));

        Assert.Contains(
            "SendToActor(playerId, new BotTickMsg())",
            spot,
            StringComparison.Ordinal);
        Assert.Contains(".Async(cancellationToken)", spot, StringComparison.Ordinal);
        Assert.DoesNotContain("FindAsync(playerId", spot, StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, BotTickMsg>",
            handlers, StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorldBotEntryRecordsIdentityBeforeDeferredJoin()
    {
        var sampleRoot = ResolveSampleRoot("ZoneWorld");
        var actor = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server", "ZoneNode", "Infrastructure", "ZLink", "Actors", "PlayerActor.cs"));
        var entry = File.ReadAllText(Path.Combine(
            sampleRoot,
            "Server", "ZoneNode", "Infrastructure", "ZLink", "Spots", "ZoneEntrySpot.cs"));

        Assert.Contains("public void PrepareEntry(bool isBot)", actor, StringComparison.Ordinal);
        Assert.Contains("actor.PrepareEntry(message.IsBot);", entry, StringComparison.Ordinal);
        Assert.True(
            entry.IndexOf("actor.PrepareEntry(message.IsBot);", StringComparison.Ordinal)
            < entry.IndexOf(".JoinSpot(", StringComparison.Ordinal),
            "bot identity must be recorded before the deferred join is scheduled");
    }

    [Fact]
    public void ZoneWorldPhysicalDisconnectUsesFrameworkLifecycleNotification()
    {
        var session = File.ReadAllText(Path.Combine(
            ResolveSampleRoot("ZoneWorld"),
            "Server", "Gateway", "Infrastructure", "ZLink", "Sessions", "PlayerSession.cs"));

        Assert.Contains("Physical disconnect is delivered", session, StringComparison.Ordinal);
        Assert.DoesNotContain("Context.Actors.Bound.ToArray()", session, StringComparison.Ordinal);
        Assert.DoesNotContain("NotifyDisconnectedAsync(cancellationToken)", session,
            StringComparison.Ordinal);
    }

    [Fact]
    public void EveryE2eScenarioStartsWithItsVerificationPurpose()
    {
        var e2eRoot = Path.Combine(ResolveDotnetRoot(), "e2e");
        var scenarioFiles = Directory.EnumerateFiles(
                e2eRoot,
                "*Scenario.cs",
                SearchOption.AllDirectories)
            .Where(static path => path.Contains(
                $"{Path.DirectorySeparatorChar}Client{Path.DirectorySeparatorChar}Scenarios{Path.DirectorySeparatorChar}"))
            .ToArray();

        Assert.NotEmpty(scenarioFiles);
        foreach (var scenarioFile in scenarioFiles)
        {
            var firstLine = File.ReadLines(scenarioFile).FirstOrDefault();
            Assert.True(
                firstLine?.StartsWith("// Verifies ", StringComparison.Ordinal) == true,
                $"{scenarioFile} must start with a short verification-purpose comment.");
        }
    }
}
