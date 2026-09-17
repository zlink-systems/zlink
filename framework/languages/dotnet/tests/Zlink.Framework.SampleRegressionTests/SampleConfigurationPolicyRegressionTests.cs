using Xunit;
using System.Text;
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
        var roots = new[] { Path.Combine(ResolveDotnetRoot(), "samples") };
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
    public void PowerShellSampleRunnerCannotRemoveRedisBySharedPrefix()
    {
        var samplesRoot = Path.Combine(ResolveDotnetRoot(), "samples");
        var helper = File.ReadAllText(Path.Combine(samplesRoot, "sample_runner.ps1"));
        var aggregate = File.ReadAllText(Path.Combine(samplesRoot, "run_samples.ps1"));

        Assert.DoesNotContain("Remove-SampleRedisScope", helper, StringComparison.Ordinal);
        Assert.DoesNotContain("Remove-SampleRedisScope", aggregate, StringComparison.Ordinal);
        Assert.Contains("Remove-SampleRedisContainer", helper, StringComparison.Ordinal);
        Assert.Matches(
            @"(?s)function Remove-SampleRedisContainer \{.*?if \(\$ContainerId -notmatch '\^\[0-9a-f\]\{12,64\}\$'\) \{ return \}.*?Invoke-SampleDockerCommand -Arguments @\(\""rm\"", \""-fv\"", \$ContainerId\)",
            helper);
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
        Assert.Contains("config_name=\"$name-replacement\"", runner, StringComparison.Ordinal);
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

    /// <summary>
    /// A Ready owner failure is not an automatic replacement, so the Zone Spots a killed
    /// ZoneNode owned stay registered to the dead incarnation. A process that comes back with
    /// the same NodeId therefore has to announce readiness with no zones — the ZoneWorld
    /// README fixes that for every restart, graceful or abrupt. A restart that reused the
    /// cold-start configuration would demand two zones it can never obtain and burn its whole
    /// claim budget, which is what ZW-B4, ZW-C2, ZW-C3 and ZW-E5 hit (#555).
    /// </summary>
    [Fact]
    public void ZoneWorldRestartsUseTheZeroZoneReplacementConfiguration()
    {
        var sample = ResolveSampleRoot("ZoneWorld");
        var shell = File.ReadAllText(Path.Combine(sample, "run_sample.sh"));
        var powershell = File.ReadAllText(Path.Combine(sample, "run_sample.ps1"));

        // Every restart selects the node's own replacement configuration, not just zone-node-2's:
        // ZW-B4 picks the node to stop from what the client observed, so either node can restart.
        Assert.Contains("config_name=\"$name-replacement\"", shell, StringComparison.Ordinal);
        Assert.Contains("$ConfigName = \"$Name-replacement\"", powershell, StringComparison.Ordinal);
        // One replacement configuration, not a separate crash variant: the stop kind does not
        // change what a restarted node may claim.
        Assert.DoesNotContain("crash-replacement", shell, StringComparison.Ordinal);
        Assert.DoesNotContain("crash-replacement", powershell, StringComparison.Ordinal);

        // Positive: no replacement configuration in either runner asks for a zone.
        Assert.Empty(ZoneWorldReplacementConfigsThatClaimZones(shell));
        Assert.Empty(ZoneWorldReplacementConfigsThatClaimZones(powershell));

        // Negative control: the same check reports the pre-fix shape, where the replacement
        // configuration carried no empty-zone-set intent.
        Assert.NotEmpty(ZoneWorldReplacementConfigsThatClaimZones(
            shell.Replace("\"allowEmptyZoneSet\": True,", string.Empty, StringComparison.Ordinal)));
        Assert.NotEmpty(ZoneWorldReplacementConfigsThatClaimZones(
            powershell.Replace("allowEmptyZoneSet = $true", string.Empty, StringComparison.Ordinal)));

        // A replacement claims nothing at all. The empty-zone-set branch returns before the
        // cold-start claim loop, so a restarted node can never settle on one zone — a state
        // that is neither the two a cold start needs nor the none a replacement announces, and
        // one the bootstrap could not leave.
        var bootstrap = File.ReadAllText(Path.Combine(
            sample, "Server", "ZoneNode", "Infrastructure", "ZLink", "Actors", "BotSpawner.cs"));
        Assert.Equal(1, Regex.Matches(bootstrap, @"settings\.AllowEmptyZoneSet").Count);
        Assert.True(
            bootstrap.IndexOf("if (settings.AllowEmptyZoneSet)", StringComparison.Ordinal)
            < bootstrap.IndexOf("EnsureZoneAsync(zoneId", StringComparison.Ordinal),
            "the replacement path must return before the cold-start claim loop");

        // ZW-E5 judges the restart from a connection opened before the stop: a node status
        // payload carries no incarnation token, so the replacement counts as ready only after
        // that same connection observed the old process leave. Both runners launch the client
        // first and only then take the node away.
        var shellLaunch = shell.IndexOf("run_client ZW-E5 &", StringComparison.Ordinal);
        Assert.True(shellLaunch > 0, "the ZW-E5 client must run while the node is taken away");
        Assert.True(
            shellLaunch
            < shell.IndexOf("scenario ZW-E5 restore armed", StringComparison.Ordinal));
        Assert.True(
            shell.IndexOf("scenario ZW-E5 restore armed", StringComparison.Ordinal)
            < shell.IndexOf("scenario ZW-E5 replacement waiting", StringComparison.Ordinal));
        var powershellLaunch = powershell.IndexOf(
            "Start-ZoneWorldClient \"ZW-E5\"", StringComparison.Ordinal);
        Assert.True(powershellLaunch > 0, "the ZW-E5 client must run while the node is taken away");
        Assert.True(
            powershellLaunch
            < powershell.IndexOf(
                "Stop-ZoneWorldNode \"zone-node-2\"", powershellLaunch, StringComparison.Ordinal));
    }

    /// <summary>
    /// Returns the header line of every ZoneNode replacement configuration a runner writes
    /// without turning the empty zone set on. An empty result is the contract; a non-empty one
    /// names the configuration that would make a restarted node demand zones.
    /// </summary>
    private static IReadOnlyList<string> ZoneWorldReplacementConfigsThatClaimZones(string runner)
    {
        var offenders = new List<string>();
        var lines = runner.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        for (var index = 0; index < lines.Length; index++)
        {
            var header = lines[index];
            if (!header.Contains("-replacement\"", StringComparison.Ordinal)) continue;
            if (!header.Contains("write(", StringComparison.Ordinal)
                && !header.Contains("Write-ZoneWorldConfig", StringComparison.Ordinal)) continue;

            var body = new StringBuilder();
            for (var line = index + 1; line < lines.Length; line++)
            {
                var text = lines[line].Trim();
                if (text is "}" or "})") break;
                body.Append(text);
            }

            if (!body.ToString().Contains("allowEmptyZoneSet", StringComparison.OrdinalIgnoreCase))
                offenders.Add(header.Trim());
        }

        return offenders;
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
        Assert.Contains("WaitFor<JoinWorldRes>()", support, StringComparison.Ordinal);
        Assert.DoesNotContain("Request(new JoinWorldReq", support, StringComparison.Ordinal);
        Assert.DoesNotContain("ExpectNone<", support, StringComparison.Ordinal);
        Assert.DoesNotContain("catch (Exception)", scenarios, StringComparison.Ordinal);
        Assert.Contains(".Connector.WaitFor<", scenarios, StringComparison.Ordinal);
        Assert.Contains(".Connector.ExpectNone<", scenarios, StringComparison.Ordinal);
        Assert.True(
            scenarios.IndexOf("var droppedWait =", StringComparison.Ordinal)
            < scenarios.IndexOf("scenario ZW-C2 armed", StringComparison.Ordinal));
        Assert.True(
            scenarios.IndexOf("var goneWait =", StringComparison.Ordinal)
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
    public void LocalNugetDefaultsDoNotCrossPlatformBoundariesOrOverrideExplicitRoots()
    {
        var props = File.ReadAllText(Path.Combine(ResolveDotnetRoot(), "Directory.Build.props"));

        Assert.Contains(
            "'$(ZLinkLocalPackageRoot)' != ''\">$(ZLinkLocalPackageRoot)/nuget",
            props,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "'$(ZLinkLocalPackageRoot)' != '' and Exists('$(ZLinkLocalPackageRoot)/nuget')",
            props,
            StringComparison.Ordinal);
        var windowsDefault = props.Split('\n').Single(static line =>
            line.Contains(".artifacts/windows/nuget", StringComparison.Ordinal));
        var nonWindowsDefault = props.Split('\n').Single(static line =>
            line.Contains(".artifacts/wsl/nuget", StringComparison.Ordinal));
        Assert.Contains("$([MSBuild]::IsOSPlatform('Windows'))", windowsDefault,
            StringComparison.Ordinal);
        Assert.DoesNotContain("!$([MSBuild]::IsOSPlatform('Windows'))", windowsDefault,
            StringComparison.Ordinal);
        Assert.Contains("!$([MSBuild]::IsOSPlatform('Windows'))", nonWindowsDefault,
            StringComparison.Ordinal);
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
    public void DotnetSampleRunnersSeparateCheckedRedisAndApplicationPorts()
    {
        var sampleRoot = Path.Combine(ResolveDotnetRoot(), "samples");
        var samples = new[]
        {
            "TicTacToe",
            "Bingo",
            "SupportChat",
            "ShoppingMall",
            "DeliveryDispatch",
            "GameQuest",
            "ZoneWorld"
        };

        foreach (var sample in samples)
        {
            var shellRunner = File.ReadAllText(Path.Combine(
                sampleRoot,
                sample,
                "run_sample.sh"));
            Assert.Single(
                Regex.Matches(
                    shellRunner,
                    @"random\.randint\(22100, 23999\)").Cast<Match>());
            Assert.Single(
                Regex.Matches(
                    shellRunner,
                    Regex.Escape(
                        "sock.bind((\"127.0.0.1\", port))")).Cast<Match>());
            Assert.DoesNotContain(
                "sock.bind((\"127.0.0.1\", 0))",
                shellRunner,
                StringComparison.Ordinal);
            Assert.Contains("redis-common.sh", shellRunner,
                StringComparison.Ordinal);
            Assert.Contains("zlink_redis_start_scoped_assign", shellRunner,
                StringComparison.Ordinal);
            Assert.Contains("zlink_redis_remove_by_id", shellRunner,
                StringComparison.Ordinal);
            Assert.DoesNotContain("docker rm -fv", shellRunner,
                StringComparison.Ordinal);

            var powershellRunner = File.ReadAllText(Path.Combine(
                sampleRoot,
                sample,
                "run_sample.ps1"));
            Assert.Contains("New-SamplePorts", powershellRunner,
                StringComparison.Ordinal);
            Assert.Contains("Start-SampleRedisContainer", powershellRunner,
                StringComparison.Ordinal);
        }

        var powershellHelper = File.ReadAllText(Path.Combine(
            sampleRoot,
            "sample_runner.ps1"));
        Assert.Contains("$applicationMinimumPort = 22100", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("$applicationMaximumPort = 23999", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("$firstPort -lt $applicationMinimumPort", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("$lastPort -gt $applicationMaximumPort", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("[System.Net.IPAddress]::Loopback,", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("$port)", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("$listener.Server.ExclusiveAddressUse = $true",
            powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("$listener.Start()", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("Configured sample port $port is unavailable.",
            powershellHelper,
            StringComparison.Ordinal);
        Assert.DoesNotContain("[System.Net.IPAddress]::Loopback, 0",
            powershellHelper,
            StringComparison.Ordinal);

        var shellRedisHelper = File.ReadAllText(Path.Combine(
            sampleRoot,
            "redis-common.sh"));
        Assert.Contains("local redis_min_port=22000", shellRedisHelper,
            StringComparison.Ordinal);
        Assert.Contains("local redis_max_port=22099", shellRedisHelper,
            StringComparison.Ordinal);
        Assert.Contains("sock.bind((\"127.0.0.1\", int(sys.argv[1])))",
            shellRedisHelper,
            StringComparison.Ordinal);
        Assert.Contains("-p \"127.0.0.1:${port}:6379\"", shellRedisHelper,
            StringComparison.Ordinal);
        Assert.Contains("zlink_redis_remove_attempt", shellRedisHelper,
            StringComparison.Ordinal);
        Assert.Contains("zlink_redis_remove_by_id", shellRedisHelper,
            StringComparison.Ordinal);
        Assert.Contains("[[ \"${container_id}\" =~ ^[0-9a-f]{12,64}$ ]] || return 1",
            shellRedisHelper,
            StringComparison.Ordinal);
        Assert.Contains("timeout -k 2s \"${docker_timeout_seconds}s\" docker rm -fv",
            shellRedisHelper,
            StringComparison.Ordinal);
        Assert.DoesNotContain("127.0.0.1::6379", shellRedisHelper,
            StringComparison.Ordinal);

        Assert.Contains("$redisMinimumPort = 22000", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("$redisMaximumPort = 22099", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("Test-SampleTcpPortAvailable -Port $port",
            powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("127.0.0.1:$($port):6379", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("Remove-SampleRedisAttempt", powershellHelper,
            StringComparison.Ordinal);
        Assert.Matches(
            @"(?s)function Remove-SampleRedisContainer \{.*?if \(\$ContainerId -notmatch '\^\[0-9a-f\]\{12,64\}\$'\) \{ return \}.*?Invoke-SampleDockerCommand -Arguments @\(\""rm\"", \""-fv\"", \$ContainerId\)",
            powershellHelper);
        Assert.Matches(
            @"(?s)Invoke-SampleDockerCommand -Arguments @\(\s*""create"".*?catch \{\s*Remove-SampleRedisAttempt -Name \$name -ContainerId \$containerId",
            powershellHelper);
        Assert.Matches(
            @"(?s)Invoke-SampleDockerCommand -Arguments @\(\s*""start"", \$containerId\).*?catch \{\s*Remove-SampleRedisAttempt -Name \$name -ContainerId \$containerId",
            powershellHelper);
        Assert.DoesNotContain("127.0.0.1::6379", powershellHelper,
            StringComparison.Ordinal);
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
        Assert.Contains("sample_runner.ps1", zoneWorldPowerShellRunner, StringComparison.Ordinal);
        Assert.Contains("Start-SampleDotnetAssembly", zoneWorldPowerShellRunner, StringComparison.Ordinal);
        Assert.Contains("Wait-ZoneWorldLog", zoneWorldPowerShellRunner, StringComparison.Ordinal);
        var peerAdmissionStart = zoneWorldPowerShellRunner.IndexOf(
            "function Wait-ZoneWorldPeerAdmission", StringComparison.Ordinal);
        var nextFunctionStart = zoneWorldPowerShellRunner.IndexOf(
            "function Wait-ZoneWorldEvidenceWhileRunning", peerAdmissionStart, StringComparison.Ordinal);
        Assert.True(peerAdmissionStart >= 0 && nextFunctionStart > peerAdmissionStart);
        var peerAdmission = zoneWorldPowerShellRunner[peerAdmissionStart..nextFunctionStart];
        Assert.Contains("$firstPath = Get-ZoneWorldErrorLogPath $FirstName", peerAdmission,
            StringComparison.Ordinal);
        Assert.Contains("$secondPath = Get-ZoneWorldErrorLogPath $SecondName", peerAdmission,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Get-ZoneWorldLogPath", peerAdmission, StringComparison.Ordinal);
        Assert.Contains("$firstPeerErrorLine = Get-ZoneWorldNextErrorLogLine \"zone-node-1\"",
            zoneWorldPowerShellRunner, StringComparison.Ordinal);
        Assert.Contains(
            "Wait-ZoneWorldPeerAdmission $Name $localRid $firstNodeErrorLine \"zone-node-1\" $peerRid $firstPeerErrorLine",
            zoneWorldPowerShellRunner, StringComparison.Ordinal);
        Assert.Contains("$BrowserSmoke", zoneWorldPowerShellRunner, StringComparison.Ordinal);
        Assert.Contains("Stop-SampleProcesses", zoneWorldPowerShellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("run_sample.sh", zoneWorldPowerShellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("Get-Command bash", zoneWorldPowerShellRunner, StringComparison.Ordinal);
    }

    [Fact]
    public void SampleRunnersFailWhenRoleCleanupRequiresSigkill()
    {
        var samplesRoot = Path.Combine(ResolveDotnetRoot(), "samples");
        var shellRunner = File.ReadAllText(Path.Combine(samplesRoot, "run_samples.sh"));
        var shellHelper = File.ReadAllText(Path.Combine(samplesRoot, "redis-common.sh"));
        var powershellHelper = File.ReadAllText(Path.Combine(samplesRoot, "sample_runner.ps1"));

        Assert.Contains("ZLINK_SAMPLE_TEARDOWN_STATUS_FILE", shellRunner, StringComparison.Ordinal);
        Assert.Contains("Sample role ${role} (pid ${pid}) exited during cleanup with status 137 (SIGKILL).",
            shellRunner, StringComparison.Ordinal);
        Assert.True(shellRunner.IndexOf("[[ -s \"${teardown_status_file}\" ]]", StringComparison.Ordinal) <
            shellRunner.IndexOf("cat \"${output_file}\"", StringComparison.Ordinal));
        Assert.Contains("builtin kill \"$@\"", shellHelper, StringComparison.Ordinal);
        Assert.Contains("ZLINK_SAMPLE_TEARDOWN_STATUS_FILE", shellHelper, StringComparison.Ordinal);
        Assert.Contains("$script:SampleProcessNames[$process.Id] = $Name", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("$process.ExitCode -eq 137 -or $process.ExitCode -eq -9", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("exited during cleanup with status -9 (SIGKILL).", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("CreateNewProcessGroup = 0x00000200", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains(
            "GenerateConsoleCtrlEvent(CtrlBreakEvent, unchecked((uint)processGroupId))",
            powershellHelper,
            StringComparison.Ordinal);
        Assert.DoesNotContain("GenerateConsoleCtrlEvent(CtrlBreakEvent, 0)", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("for ($i = 0; $i -lt 300; $i++)", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("required forced termination (taskkill /F).", powershellHelper,
            StringComparison.Ordinal);
        Assert.Contains("throw ($teardownFailures -join [Environment]::NewLine)", powershellHelper,
            StringComparison.Ordinal);

        var zoneWorldRunner = File.ReadAllText(Path.Combine(
            samplesRoot, "ZoneWorld", "run_sample.ps1"));
        var cleanup = zoneWorldRunner[zoneWorldRunner.LastIndexOf("finally {", StringComparison.Ordinal)..];
        var configurationCleanup = cleanup.IndexOf("try { Remove-SampleConfigurationFiles",
            StringComparison.Ordinal);
        var processCleanup = cleanup.IndexOf("try { Stop-SampleProcesses }",
            StringComparison.Ordinal);
        var redisCleanup = cleanup.IndexOf("try { Remove-SampleRedisContainer",
            StringComparison.Ordinal);
        Assert.True(configurationCleanup >= 0 && configurationCleanup < processCleanup);
        Assert.True(processCleanup < redisCleanup);
        Assert.Contains("throw ($cleanupFailures -join [Environment]::NewLine)", cleanup,
            StringComparison.Ordinal);
    }

    [Fact]
    public void ZoneWorldBrowserLoadsRunnerProvidedStaticConfiguration()
    {
        var browserRoot = Path.GetFullPath(Path.Combine(
            ResolveDotnetRoot(), "..", "shared_sample", "zoneworld", "client"));
        var runtime = File.ReadAllText(Path.Combine(browserRoot, "src", "shared", "config", "runtime.ts"));
        var liveTest = File.ReadAllText(Path.Combine(browserRoot, "tests", "live", "server.spec.ts"));
        var runner = File.ReadAllText(Path.Combine(ResolveSampleRoot("ZoneWorld"), "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(ResolveSampleRoot("ZoneWorld"), "run_sample.ps1"));

        Assert.Contains("fetch('/config.json'", runtime, StringComparison.Ordinal);
        Assert.DoesNotContain("import.meta.env", runtime, StringComparison.Ordinal);
        Assert.DoesNotContain("location.search", runtime, StringComparison.Ordinal);
        Assert.DoesNotContain("process.env", liveTest, StringComparison.Ordinal);
        Assert.DoesNotContain("ZONEWORLD_", runner, StringComparison.Ordinal);
        Assert.Contains("browser_dist/config.json", runner, StringComparison.Ordinal);
        Assert.Contains("browserDist \"config.json\"", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("gateway = $GatewayEndpoint", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("ops = $OpsEndpoint", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("lifecycleMarker = $browserMarker", powershellRunner, StringComparison.Ordinal);
    }

    [Fact]
    public void SampleClientsUseTheConnectorAssertionSurface()
    {
        var roots = new[] { Path.Combine(ResolveDotnetRoot(), "samples") };
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
    public void SampleClientsDoNotSynchronouslyUnwrapAsyncOperations()
    {
        var roots = new[] { Path.Combine(ResolveDotnetRoot(), "samples") };
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
}
