using Xunit;
using System.Text.RegularExpressions;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void E2E_Runners_Default_Local_Readiness_To_Three_Seconds()
    {
        var runners = Directory.EnumerateFiles(ResolveE2eRoot(), "run_e2e.sh", SearchOption.AllDirectories)
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(14, runners.Length);
        foreach (var runner in runners)
        {
            var text = File.ReadAllText(runner);
            Assert.Contains("LOCAL_READINESS_TIMEOUT_SECONDS=3", text, StringComparison.Ordinal);
            Assert.Contains("LOCAL_READINESS_POLL_SECONDS=0.1", text, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void LocationMessaging_Role_Requests_Do_Not_Retry_Route_Convergence()
    {
        var serverRoot = Path.Combine(ResolveE2eRoot(), "LocationMessaging", "Server");
        foreach (var sourceFile in Directory.EnumerateFiles(serverRoot, "*.cs", SearchOption.AllDirectories)
                     .Where(static path => !path.Contains(
                         $"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}")))
        {
            var source = File.ReadAllText(sourceFile);
            Assert.DoesNotContain("WithRetryAsync", source, StringComparison.Ordinal);
            Assert.DoesNotContain("IsRetriableRequestStartupFailure", source, StringComparison.Ordinal);
            Assert.DoesNotContain("IsRetriableStartupFailure", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void LocationMessaging_Negative_Oracles_Require_Exact_Public_Errors()
    {
        var root = Path.Combine(ResolveE2eRoot(), "LocationMessaging");
        var providerEndpoints = File.ReadAllText(Path.Combine(
            root, "Server", "Provider", "Endpoints", "ProviderEndpoints.cs"));
        var consumerEndpoints = File.ReadAllText(Path.Combine(
            root, "Server", "Consumer", "Endpoints", "ConsumerEndpoints.cs"));
        var messages = File.ReadAllText(Path.Combine(root, "Shared", "Messages.cs"));
        var scenarios = Path.Combine(root, "Client", "Scenarios");
        var rmC2 = File.ReadAllText(Path.Combine(scenarios, "RmC2TargetedRouteScenario.cs"));
        var rmC5 = File.ReadAllText(Path.Combine(scenarios, "RmC5MissingPacketScenario.cs"));
        var rmC8 = File.ReadAllText(Path.Combine(scenarios, "RmC8PayloadRoundTripScenario.cs"));
        var runtimeChannels = File.ReadAllText(Path.Combine(
            ResolveDotnetRoot(),
            "src", "Zlink.Framework", "Runtime", "Host", "ZLinkFrameworkRuntimeChannels.cs"));
        var routeRequestStart = runtimeChannels.IndexOf(
            "internal async ValueTask<IReadOnlyList<Message>> RequestToSpotViaRouterChannelAsync",
            StringComparison.Ordinal);
        var routeRequestEnd = runtimeChannels.IndexOf(
            "private static ZLinkFrameworkException CreateUnknownRouteTargetException",
            routeRequestStart,
            StringComparison.Ordinal);
        Assert.True(routeRequestStart >= 0 && routeRequestEnd > routeRequestStart,
            "The canonical Spot request implementation was not found.");
        var routeRequest = runtimeChannels[routeRequestStart..routeRequestEnd];

        Assert.DoesNotContain("catch (Exception", providerEndpoints, StringComparison.Ordinal);
        Assert.DoesNotContain("catch (Exception", consumerEndpoints, StringComparison.Ordinal);
        Assert.DoesNotContain("bool Failed", messages, StringComparison.Ordinal);
        Assert.Contains("ExpectedFailureRes(string ErrorKind)", messages, StringComparison.Ordinal);
        Assert.Contains("catch (ZLinkFrameworkException error)", providerEndpoints, StringComparison.Ordinal);
        Assert.Contains(
            "error.Kind == ZLinkFrameworkErrorKind.NotFound",
            providerEndpoints,
            StringComparison.Ordinal);
        Assert.Contains(
            "error.Kind == ZLinkFrameworkErrorKind.NotFound",
            consumerEndpoints,
            StringComparison.Ordinal);

        Assert.Contains("ZLinkFrameworkErrorKind.NotFound", rmC2, StringComparison.Ordinal);
        Assert.True(
            routeRequest.IndexOf("EnsureKnownRouteMeshPeer(", StringComparison.Ordinal)
            < routeRequest.IndexOf("_spotRouteRouter.RequestAsync(", StringComparison.Ordinal),
            "RM-C2 must reject an unknown topology target before invoking the route backend.");
        Assert.DoesNotContain("catch (ZLinkFrameworkException", routeRequest, StringComparison.Ordinal);
        Assert.Contains("ZLinkFrameworkErrorKind.NotFound", rmC5, StringComparison.Ordinal);
        Assert.Contains("reason=HandlerMissing", rmC5, StringComparison.Ordinal);
        Assert.Contains("action=ReplyError", rmC5, StringComparison.Ordinal);
        Assert.Contains("action=Drop", rmC5, StringComparison.Ordinal);
        Assert.Contains("ProviderEvidence.WaitFromEitherAsync", rmC5, StringComparison.Ordinal);
        Assert.DoesNotContain("Task.WhenAll(providerAEvidence, providerBEvidence)", rmC5, StringComparison.Ordinal);
        Assert.Contains("ZLinkFrameworkErrorKind.ProtocolError", rmC8, StringComparison.Ordinal);
        Assert.DoesNotContain(".Failed", rmC2 + rmC5 + rmC8, StringComparison.Ordinal);

        var providerEvidence = File.ReadAllText(Path.Combine(
            root, "Client", "Support", "ProviderEvidence.cs"));
        Assert.Contains("Task.WhenAny(pending)", providerEvidence, StringComparison.Ordinal);
        Assert.Contains("while (pending.Count > 0)", providerEvidence, StringComparison.Ordinal);
        Assert.Contains("failures.Add(error)", providerEvidence, StringComparison.Ordinal);
        Assert.Contains("cancellation.Cancel()", providerEvidence, StringComparison.Ordinal);
        Assert.Contains("ObserveCanceledLosersAsync", providerEvidence, StringComparison.Ordinal);
    }

    [Fact]
    public void LocationMessaging_Convergence_Oracles_Do_Not_Retry_Message_Traffic()
    {
        var root = Path.Combine(ResolveE2eRoot(), "LocationMessaging", "Client");
        var scenarios = Path.Combine(root, "Scenarios");
        foreach (var name in new[]
                 {
                     "RmB1ScaleOutScenario.cs",
                     "RmB2ScaleInScenario.cs",
                     "RmC4TimeoutIsolationScenario.cs",
                     "RmC7WeightedProviderScenario.cs"
                 })
        {
            var source = File.ReadAllText(Path.Combine(scenarios, name));
            Assert.DoesNotContain("Task.Delay", source, StringComparison.Ordinal);
            Assert.DoesNotContain("for (var attempt", source, StringComparison.Ordinal);
            Assert.DoesNotContain("catch (Exception", source, StringComparison.Ordinal);
            if (name is "RmB1ScaleOutScenario.cs" or "RmB2ScaleInScenario.cs"
                or "RmC7WeightedProviderScenario.cs")
                Assert.Contains(
                    "monitor-mesh|source=profile|kind=ConnectionReady|remote=",
                    source,
                    StringComparison.Ordinal);
            if (name is "RmB1ScaleOutScenario.cs" or "RmB2ScaleInScenario.cs"
                or "RmC7WeightedProviderScenario.cs")
            {
                Assert.Contains(".Zip(", source, StringComparison.Ordinal);
                Assert.Contains(".Select(result => result.First)", source, StringComparison.Ordinal);
                Assert.DoesNotContain(".Select(reply => reply.Value)", source, StringComparison.Ordinal);
            }
            if (name == "RmB2ScaleInScenario.cs")
            {
                Assert.Contains(
                    "monitor-mesh|source=profile|kind=Disconnected",
                    source,
                    StringComparison.Ordinal);
                Assert.DoesNotContain("|value=1", source, StringComparison.Ordinal);
                Assert.Contains("Result: \"Stopped\"", source, StringComparison.Ordinal);
                Assert.Contains("WaitForPeerRowGoneAsync(requester, \"api-b\")", source,
                    StringComparison.Ordinal);
                Assert.Contains("firstAfter.ProviderRid == \"api-a\"", source, StringComparison.Ordinal);
                Assert.DoesNotContain("ConfirmWeightPropagationWithTrafficAsync", source,
                    StringComparison.Ordinal);
            }
        }

        var launcher = File.ReadAllText(Path.Combine(root, "Support", "DynamicClusterLauncher.cs"));
        Assert.Contains("ReadinessTimeout = TimeSpan.FromSeconds(3)", launcher, StringComparison.Ordinal);
        Assert.Contains("ReadinessPollInterval = TimeSpan.FromMilliseconds(100)", launcher, StringComparison.Ordinal);
        Assert.Contains("GracefulShutdownTimeout = TimeSpan.FromSeconds(30)", launcher, StringComparison.Ordinal);
        Assert.Contains("WaitAsync(GracefulShutdownTimeout)", launcher, StringComparison.Ordinal);
        Assert.Contains("error.Kind is ZLinkFrameworkErrorKind.Unavailable", launcher, StringComparison.Ordinal);
        Assert.Contains("or ZLinkFrameworkErrorKind.DeadlineExceeded", launcher, StringComparison.Ordinal);
        Assert.DoesNotContain("error.RetryAdvice", launcher, StringComparison.Ordinal);
        Assert.Contains("startInfo.ArgumentList.Add(\"--no-build\")", launcher, StringComparison.Ordinal);
        Assert.Contains("Path.Combine(options.LogDir, \"dynamic\", scenarioName)", launcher,
            StringComparison.Ordinal);
        Assert.Contains("Path.Combine(options.ConfigDir, scenarioName)", launcher, StringComparison.Ordinal);
        Assert.Contains("var processName = $\"{scenarioName}-{name}\"", launcher, StringComparison.Ordinal);
        Assert.DoesNotContain("for (var i = 0; i < 120", launcher, StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Delay(250)", launcher, StringComparison.Ordinal);
        Assert.DoesNotContain("catch\n", launcher, StringComparison.Ordinal);
        Assert.DoesNotContain("catch {", launcher, StringComparison.Ordinal);

        var consumerEvidence = File.ReadAllText(Path.Combine(
            ResolveE2eRoot(), "LocationMessaging", "Server", "Consumer", "ConnectionEvidence.cs"));
        var providerEvidence = File.ReadAllText(Path.Combine(
            ResolveE2eRoot(), "LocationMessaging", "Server", "Provider", "Infrastructure",
            "EvidenceStore.cs"));
        var consumerEndpoints = File.ReadAllText(Path.Combine(
            ResolveE2eRoot(), "LocationMessaging", "Server", "Consumer", "Endpoints",
            "ConsumerEndpoints.cs"));
        var providerEndpoints = File.ReadAllText(Path.Combine(
            ResolveE2eRoot(), "LocationMessaging", "Server", "Provider", "Endpoints",
            "ProviderEndpoints.cs"));
        Assert.Contains("throw new TimeoutException", consumerEvidence, StringComparison.Ordinal);
        Assert.Contains("throw new TimeoutException", providerEvidence, StringComparison.Ordinal);
        Assert.DoesNotContain("return _entries.ToArray();", consumerEvidence, StringComparison.Ordinal);
        Assert.DoesNotContain("|| !await _signal.WaitAsync(remaining, cancellationToken))\n                return Snapshot();",
            providerEvidence, StringComparison.Ordinal);
        Assert.Contains("StatusCodes.Status504GatewayTimeout", consumerEndpoints, StringComparison.Ordinal);
        Assert.Contains("StatusCodes.Status504GatewayTimeout", providerEndpoints, StringComparison.Ordinal);
    }

    [Fact]
    public void Failure_Scenarios_Observe_The_First_Profile_Request_Result()
    {
        var root = ResolveE2eRoot();
        foreach (var configuration in new[] { "ResilienceLifecycle", "StoreFailure" })
        {
            var source = File.ReadAllText(Path.Combine(
                root, configuration, "Server", "Consumer", "ConsumerHostFactory.cs"));
            Assert.DoesNotContain("RequestProfileWithRetryAsync", source, StringComparison.Ordinal);
            Assert.DoesNotContain("IsRetriableStartupFailure", source, StringComparison.Ordinal);
            Assert.Contains("RequestProfileAsync(channel, request)", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void Failure_Runners_Start_Previously_Built_Applications_Directly()
    {
        var root = ResolveE2eRoot();
        foreach (var configuration in new[] { "ResilienceLifecycle", "StoreFailure" })
        {
            var runner = File.ReadAllText(Path.Combine(root, configuration, "run_e2e.sh"));

            Assert.Contains("setsid dotnet \"$application\" --config", runner, StringComparison.Ordinal);
            Assert.Contains("dotnet \"$CLIENT_APPLICATION\" --config", runner, StringComparison.Ordinal);
            Assert.DoesNotContain("dotnet run --no-build --project", runner, StringComparison.Ordinal);

            var processManager = File.ReadAllText(Directory.GetFiles(
                Path.Combine(root, configuration, "Client", "Support"),
                "*ProcessManager.cs").Single());
            Assert.Contains("startInfo.ArgumentList.Add(application);", processManager,
                StringComparison.Ordinal);
            Assert.DoesNotContain("startInfo.ArgumentList.Add(\"run\")", processManager,
                StringComparison.Ordinal);
        }
    }

    [Fact]
    public void Routed_E2e_Requests_Do_Not_Use_Application_Retry_Helpers()
    {
        var root = ResolveE2eRoot();
        foreach (var configuration in new[]
                 {
                     "AutomaticTurnDispatch", "LocationMessaging", "ResilienceLifecycle",
                     "SpotService", "StoreFailure"
                 })
        foreach (var sourcePath in Directory.GetFiles(
                     Path.Combine(root, configuration), "*.cs", SearchOption.AllDirectories))
        {
            var source = File.ReadAllText(sourcePath);
            Assert.DoesNotContain("WithRetryAsync", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void Resilience_Down_Window_Separates_Topology_From_Routing_Convergence()
    {
        var scenarios = Path.Combine(ResolveE2eRoot(), "ResilienceLifecycle", "Client", "Scenarios");
        var restart = File.ReadAllText(Path.Combine(scenarios, "RlA1ProviderRestartScenario.cs"));
        Assert.Contains("/profile/request/attempt/1000", restart, StringComparison.Ordinal);
        Assert.Contains("down.ErrorKind", restart, StringComparison.Ordinal);

        foreach (var name in new[]
                 {
                     "RlA5ProviderFlappingScenario.cs", "RlB2CrashDuringInflightScenario.cs",
                     "RlB3GracefulShutdownScenario.cs", "RlC3NodePauseRecoveryScenario.cs"
                 })
        {
            var source = File.ReadAllText(Path.Combine(scenarios, name));
            Assert.Contains("WaitUntilProviderExcludedAsync", source, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void StoreFailure_Disconnect_Probe_Uses_A_Timeout_Inside_Its_Convergence_Window()
    {
        var source = File.ReadAllText(Path.Combine(
            ResolveE2eRoot(),
            "StoreFailure",
            "Client",
            "Scenarios",
            "SfD2LongOutageRecoveryScenario.cs"));

        Assert.Contains("options.PollingInterval.TotalMilliseconds", source, StringComparison.Ordinal);
        Assert.Contains("probeTimeoutMilliseconds", source, StringComparison.Ordinal);
        Assert.DoesNotContain(
            "sf-d2-disconnect-probe-{Guid.NewGuid():N}\");",
            source,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Observability_Lease_Lateness_Uses_Typed_Heartbeat_Configuration()
    {
        var server = Path.Combine(ResolveE2eRoot(), "ObservabilityOps", "Server");
        foreach (var role in new[] { "Play", "Workflow" })
        {
            var host = File.ReadAllText(Path.Combine(server, role, $"{role}HostFactory.cs"));
            var options = File.ReadAllText(Path.Combine(server, role, "Support", $"{role}Options.cs"));

            Assert.Contains("options.LocationHeartbeatMs", host, StringComparison.Ordinal);
            Assert.Contains("options.LocationLeaseTtlMs", host, StringComparison.Ordinal);
            Assert.Contains("int LocationHeartbeatMs = 1000", options, StringComparison.Ordinal);
            Assert.Contains("int LocationLeaseTtlMs = 3000", options, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void LocationMessaging_Dynamic_Providers_Receive_The_Required_Message_Size_Limit()
    {
        var launcher = File.ReadAllText(Path.Combine(
            ResolveE2eRoot(),
            "LocationMessaging",
            "Client",
            "Support",
            "DynamicClusterLauncher.cs"));

        Assert.Contains("MaxMessageSize", launcher, StringComparison.Ordinal);
        Assert.Contains("2_097_152", launcher, StringComparison.Ordinal);
    }

    [Fact]
    public void PubSub_Handler_Delay_Is_Optional_For_Normal_Subscribers()
    {
        var options = File.ReadAllText(Path.Combine(
            ResolveE2eRoot(),
            "PubSub",
            "Server",
            "Subscriber",
            "Configuration",
            "SubscriberOptions.cs"));

        Assert.Contains("int HandlerDelayMs = 0", options, StringComparison.Ordinal);
    }

    [Fact]
    public void PubSub_Dynamic_Lifecycle_Uses_Delivery_Probes_Not_Internal_Socket_Events()
    {
        var root = Path.Combine(ResolveE2eRoot(), "PubSub");
        var host = File.ReadAllText(Path.Combine(
            root, "Server", "Subscriber", "SubscriberHostFactory.cs"));
        var observation = File.ReadAllText(Path.Combine(
            root, "Client", "Support", "SubscriberObservation.cs"));

        Assert.DoesNotContain("AddZLinkMonitoring", host, StringComparison.Ordinal);
        Assert.DoesNotContain("ZLinkSocketEvent", host, StringComparison.Ordinal);
        Assert.Contains("WaitForEventAsync", observation, StringComparison.Ordinal);

        foreach (var name in new[]
                 {
                     "PsA3LateSubscriberScenario.cs",
                     "PsA4SubscriberReconnectScenario.cs",
                     "PsB2PublisherRestartScenario.cs"
                 })
        {
            var scenario = File.ReadAllText(Path.Combine(root, "Client", "Scenarios", name));
            Assert.Contains("SubscriberObservation.WaitForEventAsync", scenario, StringComparison.Ordinal);
            Assert.DoesNotContain("Task.Delay(500)", scenario, StringComparison.Ordinal);
            Assert.DoesNotContain("i <= 42", scenario, StringComparison.Ordinal);
            Assert.DoesNotContain("i <= 8", scenario, StringComparison.Ordinal);
        }

        var restart = File.ReadAllText(Path.Combine(
            root, "Client", "Scenarios", "PsB2PublisherRestartScenario.cs"));
        Assert.Contains("\"sequence\", \"3\"", restart, StringComparison.Ordinal);
        Assert.Contains("\"seq=3|\"", restart, StringComparison.Ordinal);

        var lifecycle = File.ReadAllText(Path.Combine(
            ResolveDotnetRoot(),
            "src", "Zlink.Framework.AspNetCore", "ZLinkAutoConnectLifecycleCoordinator.cs"));
        Assert.Contains("FrameworkReadyAsync", lifecycle, StringComparison.Ordinal);
    }

    [Fact]
    public void LocationMessaging_RmC9_Fills_A_Bounded_High_Water_Mark()
    {
        var root = Path.Combine(ResolveE2eRoot(), "LocationMessaging");
        var scenario = File.ReadAllText(Path.Combine(
            root, "Client", "Scenarios", "RmC9BackpressureScenario.cs"));
        var consumer = File.ReadAllText(Path.Combine(root, "Server", "Consumer", "ConsumerHostFactory.cs"));
        var provider = File.ReadAllText(Path.Combine(root, "Server", "Provider", "ProviderHostFactory.cs"));

        Assert.Contains("private const ulong ApplicationHwmBytes = 1UL * 1024 * 1024", scenario, StringComparison.Ordinal);
        Assert.Contains("private const int BlockerPayloadBytes = 2 * 1024 * 1024", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("SendHighWaterMark = 4", consumer, StringComparison.Ordinal);
        Assert.Contains("ReceiveHighWaterMark = 4", provider, StringComparison.Ordinal);
        Assert.Contains("/profile/backpressure/release", scenario, StringComparison.Ordinal);
        Assert.Contains("/runtime/status", scenario, StringComparison.Ordinal);
        Assert.Contains("ApplicationReceivePaused", scenario, StringComparison.Ordinal);
        Assert.Contains("PendingPayloadBytes", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Delay(TimeSpan.FromSeconds(10))", scenario, StringComparison.Ordinal);
    }

    [Fact]
    public void State_Observation_Uses_Role_Server_Bounded_Waits()
    {
        var root = ResolveE2eRoot();
        var locationScenarios = new[]
        {
            "RmB1ScaleOutScenario.cs",
            "RmB2ScaleInScenario.cs",
            "RmA4SameRidFailoverScenario.cs"
        };
        foreach (var scenarioName in locationScenarios)
        {
            var source = File.ReadAllText(Path.Combine(
                root, "LocationMessaging", "Client", "Scenarios", scenarioName));
            Assert.DoesNotContain("Get(\"/locations/peers", source, StringComparison.Ordinal);
            Assert.Contains("Post(\"/locations/peers/wait\")", source, StringComparison.Ordinal);
        }

        var storeProbe = File.ReadAllText(Path.Combine(
            root, "StoreFailure", "Client", "Support", "SfProbe.cs"));
        Assert.DoesNotContain("last = await TryGetPeersAsync", storeProbe, StringComparison.Ordinal);
        Assert.DoesNotContain("last = await GetStatusAsync", storeProbe, StringComparison.Ordinal);
        Assert.Contains("Post(\"/query/peers/wait\")", storeProbe, StringComparison.Ordinal);
        Assert.Contains("Post(\"/query/status/wait\")", storeProbe, StringComparison.Ordinal);

        var trafficProbe = File.ReadAllText(Path.Combine(
            root, "ResilienceLifecycle", "Client", "Support", "ProviderTrafficProbe.cs"));
        Assert.DoesNotContain("provider.Get(\"/evidence\")", trafficProbe, StringComparison.Ordinal);
        Assert.Contains("provider.Post(\"/evidence/wait\")", trafficProbe, StringComparison.Ordinal);
    }

    [Fact]
    public void Config_9_And_10_Keep_One_Client_Scenario_Per_File()
    {
        var root = ResolveE2eRoot();
        AssertScenarioFiles(root, "SpotActorTransfer", "St", 36);
        AssertScenarioFiles(root, "ToActorMessaging", "Ta", 7);
    }

    [Fact]
    public void SpotService_Client_Exposes_Every_Scenario_As_A_Direct_Selector()
    {
        var root = Path.Combine(ResolveE2eRoot(), "SpotService", "Client");
        var program = File.ReadAllText(Path.Combine(root, "Program.cs"));
        var scenarioIds = Directory.GetFiles(Path.Combine(root, "Scenarios"), "Sm*Scenario.cs")
            .Select(Path.GetFileNameWithoutExtension)
            .Select(static name => Regex.Match(name!, @"^Sm(?<track>[A-Z])(?<number>\d+)"))
            .Select(static match => $"sm-{match.Groups["track"].Value.ToLowerInvariant()}{match.Groups["number"].Value}")
            .Order(StringComparer.Ordinal)
            .ToArray();

        Assert.Equal(58, scenarioIds.Length);
        foreach (var scenarioId in scenarioIds)
            Assert.Contains($"\"{scenarioId}\" =>", program, StringComparison.Ordinal);
    }

    [Fact]
    public void SpotService_Spot_Outbound_Channel_Uses_The_Owner_MeshNode()
    {
        var root = Path.Combine(ResolveE2eRoot(), "SpotService");
        var host = File.ReadAllText(Path.Combine(
            root, "Server", "Play", "PlayHostFactory.cs"));
        var runner = File.ReadAllText(Path.Combine(root, "run_e2e.sh"));

        Assert.Contains(
            "spot.Channel(SpotServiceNames.ExternalClientChannel).Server()",
            host,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "AddRouteMesh(SpotServiceNames.ExternalClientChannel)",
            host,
            StringComparison.Ordinal);
        Assert.DoesNotContain("external-client-endpoint", runner, StringComparison.Ordinal);
    }

    [Fact]
    public void SpotActorTransfer_Separates_Host_Endpoints_From_Actor_Runtime()
    {
        var actorNode = Path.Combine(
            ResolveE2eRoot(),
            "SpotActorTransfer",
            "Server",
            "ActorNode");
        var program = File.ReadAllText(Path.Combine(actorNode, "Program.cs"));
        var endpoints = File.ReadAllText(Path.Combine(actorNode, "ActorNodeEndpoints.cs"));
        var runtime = File.ReadAllText(Path.Combine(actorNode, "ActorRuntime.cs"));

        Assert.DoesNotContain("app.MapPost", program, StringComparison.Ordinal);
        Assert.Contains("ActorNodeEndpoints.Map(app, options)", program, StringComparison.Ordinal);
        Assert.Contains("app.MapPost(\"/actors\"", endpoints, StringComparison.Ordinal);
        Assert.DoesNotContain("class TransferActorRelocationAdapter", program, StringComparison.Ordinal);
        Assert.Contains("class TransferActorRelocationAdapter", runtime, StringComparison.Ordinal);
        Assert.DoesNotContain("app.MapPost", runtime, StringComparison.Ordinal);
    }

    [Fact]
    public void SpotActorTransfer_Cleanup_Fixtures_Use_Public_Contracts()
    {
        var root = Path.Combine(ResolveE2eRoot(), "SpotActorTransfer");
        var support = File.ReadAllText(Path.Combine(
            root, "Server", "ActorNode", "CleanupGatedLocationStore.cs"));
        var host = File.ReadAllText(Path.Combine(
            root, "Server", "ActorNode", "ActorNodeHostFactory.cs"));
        var b2 = File.ReadAllText(Path.Combine(
            root, "Client", "Scenarios", "StB2SourceCleanupFailureAfterSuccessScenario.cs"));
        var d2 = File.ReadAllText(Path.Combine(
            root, "Client", "Scenarios", "StD2StaleSourceReleaseFencingScenario.cs"));
        var runtime = File.ReadAllText(Path.Combine(
            root, "Server", "ActorNode", "ActorRuntime.cs"));

        Assert.Contains("CleanupGatedLocationStore", host, StringComparison.Ordinal);
        Assert.Contains("IZLinkLocationStore", support, StringComparison.Ordinal);
        Assert.Contains("mutation is ZLinkStoreMutation.Delete", support,
            StringComparison.Ordinal);
        Assert.Contains("inner.WriteAsync(request, cancellationToken)", support,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Zlink.Framework.Runtime", support, StringComparison.Ordinal);

        Assert.Contains("source_cleanup_wait", b2, StringComparison.Ordinal);
        Assert.Contains("source_cleanup_wait", runtime, StringComparison.Ordinal);
        Assert.Contains("CrashNodeAAndWaitUnavailableAsync", b2, StringComparison.Ordinal);
        Assert.DoesNotContain("ArmCleanupGateAsync", b2, StringComparison.Ordinal);
        Assert.DoesNotContain("AllowCleanupAttemptAsync", b2, StringComparison.Ordinal);
        Assert.DoesNotContain("ReleaseCleanupGateAsync", b2, StringComparison.Ordinal);

        Assert.Contains("before-stale-cleanup-release", d2, StringComparison.Ordinal);
        Assert.Contains("location_snapshot_before_cleanup", d2, StringComparison.Ordinal);
        Assert.Contains("ReleaseCleanupGateAsync", d2, StringComparison.Ordinal);
        Assert.Contains("source_cleanup_completed", d2, StringComparison.Ordinal);
        Assert.Contains("location_snapshot_after_cleanup", d2, StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Delay", b2 + d2, StringComparison.Ordinal);
    }

    [Fact]
    public void RuntimeMonitoring_Uses_A_Client_Trigger_And_Role_Evidence()
    {
        var root = Path.Combine(ResolveE2eRoot(), "RuntimeMonitoring");
        Assert.False(File.Exists(Path.Combine(
            root, "Server", "Trigger", "RuntimeMonitoring.Trigger.csproj")));

        var runner = File.ReadAllText(Path.Combine(root, "run_e2e.sh"));
        Assert.DoesNotContain("ZLINK_DEBUG_FRAMEWORK_TASKS", runner, StringComparison.Ordinal);
        Assert.DoesNotContain("/logs/throw-stderr", runner, StringComparison.Ordinal);

        var scenario = File.ReadAllText(Path.Combine(
            root, "Client", "Scenarios", "MonC1DispatchFailureScenario.cs"));
        Assert.DoesNotContain("throw-stderr", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("unhandled callback failed", scenario, StringComparison.Ordinal);
    }

    [Fact]
    public void ObservabilityOps_Is_A_Self_Contained_E2E_App()
    {
        var root = Path.Combine(ResolveE2eRoot(), "ObservabilityOps");
        var clientScenarios = Path.Combine(root, "Client", "Scenarios");
        Assert.True(Directory.Exists(clientScenarios));
        Assert.Equal(21, Directory.GetFiles(clientScenarios, "Obs*Scenario.cs").Length);

        foreach (var project in Directory.GetFiles(root, "*.csproj", SearchOption.AllDirectories))
        {
            var source = File.ReadAllText(project);
            Assert.DoesNotContain("samples/Bingo", source, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("samples/ShoppingMall", source, StringComparison.OrdinalIgnoreCase);
        }

        var runner = File.ReadAllText(Path.Combine(root, "run_e2e.sh"));
        Assert.DoesNotContain("samples/Bingo/Client", runner, StringComparison.Ordinal);
        Assert.DoesNotContain("python3 - \"$LOG_DIR", runner, StringComparison.Ordinal);
    }

    private static void AssertScenarioFiles(string root, string config, string idPrefix, int expectedCount)
    {
        var client = Path.Combine(root, config, "Client");
        var scenarioDirectory = Path.Combine(client, "Scenarios");
        Assert.True(Directory.Exists(scenarioDirectory), $"{config} Client/Scenarios is missing.");
        var scenarios = Directory.GetFiles(scenarioDirectory, $"{idPrefix}*Scenario.cs");
        Assert.Equal(expectedCount, scenarios.Length);

        var program = File.ReadAllText(Path.Combine(client, "Program.cs"));
        Assert.DoesNotContain("async Task Run", program, StringComparison.Ordinal);
        Assert.DoesNotContain("async () =>", program, StringComparison.Ordinal);
    }
}
