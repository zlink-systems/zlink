using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed partial class RegressionTests
{
    [Fact]
    public void DeliveryDispatch_Uses_One_Role_Specific_Mesh_Per_Host_And_Scanned_Channel_Handlers()
    {
        var sampleRoot = ResolveSampleRoot("DeliveryDispatch");
        var hosts = Directory.EnumerateFiles(
                Path.Combine(sampleRoot, "Server"), "*HostFactory.cs", SearchOption.AllDirectories)
            .ToArray();

        Assert.Equal(5, hosts.Length);
        foreach (var host in hosts)
        {
            var source = File.ReadAllText(host);
            Assert.Equal(1, source.Split("AddRouteMesh(", StringSplitOptions.None).Length - 1);
            var expectedMesh = host.Contains("CustomerGateway", StringComparison.Ordinal)
                               || host.Contains("Tracking", StringComparison.Ordinal)
                ? "AddRouteMesh(SampleNames.CustomerMeshName)"
                : "AddRouteMesh(SampleNames.CourierMeshName)";
            Assert.Contains(expectedMesh, source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddRequestHandler<", source, StringComparison.Ordinal);
            Assert.DoesNotContain("AddSendHandler<", source, StringComparison.Ordinal);
        }

        var dispatch = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Dispatch", "DispatchServerHostFactory.cs"));
        var tracking = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Tracking", "TrackingServerHostFactory.cs"));
        Assert.Contains("AddHandlerGroup(SampleNames.DispatchChannel)", dispatch, StringComparison.Ordinal);
        Assert.Contains("AddHandlerGroup(SampleNames.TrackingRouteChannel)", tracking, StringComparison.Ordinal);
        Assert.Contains("mesh.Objects().Client()", dispatch, StringComparison.Ordinal);
        Assert.Contains("mesh.Objects().Client()", tracking, StringComparison.Ordinal);
        Assert.Contains(
            "var dispatchChannel = options.AddClientServerChannel(SampleNames.DispatchChannel)",
            dispatch,
            StringComparison.Ordinal);
        Assert.Contains("dispatchChannel.Client()", dispatch, StringComparison.Ordinal);
        Assert.Contains("dispatchChannel.Server()", dispatch, StringComparison.Ordinal);
        Assert.Contains("AddClientServerChannel(SampleNames.TrackingRouteChannel).Server()", tracking,
            StringComparison.Ordinal);
    }

    [Fact]
    public void DeliveryDispatch_Client_Gate_Verifies_Status_Arrival_Order()
    {
        var sampleRoot = ResolveSampleRoot("DeliveryDispatch");
        var scenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "DeliveryDispatchClientScenario.cs"));
        var worker = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Dispatch", "DispatchWorker.cs"));

        Assert.Contains("WaitForSequence<DeliveryStatusNotify>()", scenario, StringComparison.Ordinal);
        Assert.Contains("ExpectNone<OfferDeliveryNotify>()", scenario, StringComparison.Ordinal);
        Assert.Contains("Status: DeliveryStatus.Assigned", scenario, StringComparison.Ordinal);
        Assert.Contains("Status: DeliveryStatus.Reassigned", scenario, StringComparison.Ordinal);
        Assert.Contains("Status: DeliveryStatus.Reassigned", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("WaitForStatusSequenceAsync(", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("ExpectNoPushAsync(", scenario, StringComparison.Ordinal);
        Assert.DoesNotContain("var assignedWait = WaitForStatusAsync", scenario, StringComparison.Ordinal);
        Assert.Contains("if (offer.CandidateIndex == 0)", worker, StringComparison.Ordinal);
    }

    [Fact]
    public void DeliveryDispatch_Runner_Uses_Isolated_Docker_Redis_And_Location_Store()
    {
        var sampleRoot = ResolveSampleRoot("DeliveryDispatch");
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));
        var readme = File.ReadAllText(Path.Combine(sampleRoot, "README.ko.md"));
        var topology = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "SampleTopology.cs"));

        Assert.DoesNotContain("if [[ -z \"${DELIVERYDISPATCH_REDIS_ENDPOINT:-}\" ]]", shellRunner,
            StringComparison.Ordinal);
        AssertShellRunnerUsesRedisDockerHelper(
            shellRunner,
            "zlink-deliverydispatch-dotnet-redis",
            "DELIVERYDISPATCH_REDIS_ENDPOINT");
        Assert.Contains("write_role_config", shellRunner, StringComparison.Ordinal);
        Assert.Contains("deliverydispatch=completed", shellRunner, StringComparison.Ordinal);
        Assert.Contains("topology=ready", shellRunner, StringComparison.Ordinal);
        Assert.Contains("deliverydispatch-reassignment=completed", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("deliverydispatch-server-evidence=completed", shellRunner, StringComparison.Ordinal);
        Assert.Contains("deliverydispatch-runner-evidence=completed", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", shellRunner, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("SAMPLE_LOG_DIR=\"${RUN_DIR}/sample-logs\"", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("FLOW_LOG_DIR", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_STARTUP_DELAY_SECONDS", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_STARTUP_SETTLE_SECONDS", shellRunner, StringComparison.Ordinal);

        Assert.DoesNotContain("if (-not $env:DELIVERYDISPATCH_REDIS_ENDPOINT)", powershellRunner,
            StringComparison.Ordinal);
        AssertPowerShellRunnerUsesRedisDockerHelper(powershellRunner, "zlink-deliverydispatch-dotnet-redis");
        Assert.Contains("if ($RedisContainer)", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("Write-RoleConfig", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("deliverydispatch=completed", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("topology=ready", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("deliverydispatch-reassignment=completed", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("deliverydispatch-server-evidence=completed", powershellRunner, StringComparison.Ordinal);
        Assert.Contains("deliverydispatch-runner-evidence=completed", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("message flow", powershellRunner, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("$SampleLogDir = Join-Path $RunDir \"sample-logs\"", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Join-Path $ScriptDir \"logs\"", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_STARTUP_DELAY_SECONDS", powershellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_STARTUP_SETTLE_SECONDS", powershellRunner,
            StringComparison.Ordinal);

        // The topology is bound from the role's configuration file, not pieced together from
        // the environment
        // (framework/doc/framework/common/sample-e2e-configuration-policy.ko.md §2.3).
        Assert.Contains("SampleTopologyOptions", topology, StringComparison.Ordinal);
        Assert.DoesNotContain("Environment.GetEnvironmentVariable", topology, StringComparison.Ordinal);
        foreach (var hostFactory in Directory.EnumerateFiles(Path.Combine(sampleRoot, "Server"), "*HostFactory.cs",
                     SearchOption.AllDirectories))
        {
            AssertLocationStoreHost(File.ReadAllText(hostFactory));
        }

        Assert.Contains("전용 Docker Redis", readme, StringComparison.Ordinal);
        Assert.Contains("외부 Redis endpoint", readme, StringComparison.Ordinal);
        Assert.Contains("`run_sample.sh`와 `run_sample.ps1`", readme, StringComparison.Ordinal);
        Assert.Contains("AssignDeliveryMsg", readme, StringComparison.Ordinal);
        Assert.DoesNotContain("`AssignDelivery`", readme, StringComparison.Ordinal);
    }

    [Fact]
    public void DeliveryDispatch_Contracts_Match_Common_Role_Model()
    {
        var sampleRoot = ResolveSampleRoot("DeliveryDispatch");
        var readme = File.ReadAllText(Path.Combine(sampleRoot, "README.ko.md"));
        var shellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        var powershellRunner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.ps1"));
        var topology = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Configuration", "SampleTopology.cs"));
        var messages = File.ReadAllText(Path.Combine(sampleRoot, "Shared", "Contracts", "Messages.cs"));
        var courierBinder = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "CourierSession", "CourierSessionBinder.cs"));
        var courierOffers = File.ReadAllText(Path.Combine(
            sampleRoot, "Server", "Dispatch", "DispatchZLinkAdapters.cs"));
        var customerSession = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CustomerGateway",
            "SubscribeDeliverySessionHandler.cs"));
        var customerStatusHandler = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CustomerGateway",
            "Spots", "EntrySpot", "Handlers", "DeliveryStatusUpdatedHandler.cs"));
        var customerAccess = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CustomerGateway",
            "CustomerActorAccess.cs"));
        var courierEntrySpot = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CourierActorNode",
            "Spots", "EntrySpot", "EntrySpot.cs"));
        var customerEntrySpot = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CustomerGateway",
            "Spots", "EntrySpot", "CustomerEntrySpot.cs"));
        var clientScenario = File.ReadAllText(Path.Combine(sampleRoot, "Client", "DeliveryDispatchClientScenario.cs"));

        Assert.Contains("record AssignDeliveryMsg", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("record FindCourierActorReq", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("record FindCustomerActorReq", messages, StringComparison.Ordinal);
        Assert.Contains("record DeliveryStatusUpdatedMsg", messages, StringComparison.Ordinal);
        Assert.Contains("record DeliveryStatusChangedReq", messages, StringComparison.Ordinal);
        Assert.Contains("record DeliveryStatusNotify", messages, StringComparison.Ordinal);
        Assert.Contains("long OccurredAtUnixMs", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("DateTimeOffset OccurredAt", messages, StringComparison.Ordinal);
        Assert.Contains("record BindCourierSessionReq", messages, StringComparison.Ordinal);
        Assert.Contains("record BindCourierSessionRes", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("ActorRef", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("record DeliveryStatusUpdatedRes", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("record BindCourierReq", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("record BindCourierRes", messages, StringComparison.Ordinal);
        Assert.DoesNotContain("record ReassignDelivery", messages, StringComparison.Ordinal);
        Assert.Contains(".GetOrCreate(courierId, SampleNames.CourierActorType)", courierBinder,
            StringComparison.Ordinal);
        Assert.DoesNotContain("RequestToNode(", courierBinder, StringComparison.Ordinal);
        Assert.Contains(".SendToActor(", courierOffers, StringComparison.Ordinal);
        Assert.Contains("FindAsync(customerId", customerAccess, StringComparison.Ordinal);
        Assert.Contains("GetOrCreate(", customerAccess, StringComparison.Ordinal);
        Assert.DoesNotContain("CustomerActorDirectory", customerAccess, StringComparison.Ordinal);
        Assert.DoesNotContain("RequireRef(", customerAccess, StringComparison.Ordinal);
        Assert.DoesNotContain("FindAsync(", courierEntrySpot, StringComparison.Ordinal);
        Assert.DoesNotContain("FindAsync(", customerEntrySpot, StringComparison.Ordinal);
        Assert.Contains("actors.Register(actor)", courierEntrySpot, StringComparison.Ordinal);
        Assert.Contains("actors.Register(actor)", customerEntrySpot, StringComparison.Ordinal);
        Assert.Contains("actors.FindAsync(CustomerId", customerSession, StringComparison.Ordinal);
        Assert.Contains("actors.EnsureAsync(CustomerId", customerSession, StringComparison.Ordinal);
        Assert.Contains("IZLinkSpotPacketHandler<CustomerEntrySpot, DeliveryStatusUpdatedMsg>",
            customerStatusHandler, StringComparison.Ordinal);
        Assert.DoesNotContain(".Actor.ObjectGeneration", clientScenario, StringComparison.Ordinal);
        Assert.Contains("WaitForSequence<DeliveryStatusNotify>()", clientScenario, StringComparison.Ordinal);
        Assert.Contains("ExpectNone<OfferDeliveryNotify>()", clientScenario, StringComparison.Ordinal);
        Assert.Contains("ZlinkStreamAssert.Ensure", clientScenario, StringComparison.Ordinal);
        Assert.DoesNotContain("WaitForStatusSequenceAsync(", clientScenario, StringComparison.Ordinal);
        Assert.Contains("deliverydispatch courier-session: bound courier=courier-a", shellRunner,
            StringComparison.Ordinal);
        Assert.Contains("deliverydispatch courier-session: bound courier=courier-b", shellRunner,
            StringComparison.Ordinal);
        Assert.DoesNotContain("CourierGateway", readme, StringComparison.Ordinal);
        Assert.DoesNotContain("deliverydispatch.courier", readme, StringComparison.Ordinal);
        Assert.DoesNotContain("courier-gateway", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("courier-gateway", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_COURIER_ROUTE", topology, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_CUSTOMER_ROUTE", topology, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_COURIER_ROUTE", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_CUSTOMER_ROUTE", shellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_COURIER_ROUTE", powershellRunner, StringComparison.Ordinal);
        Assert.DoesNotContain("DELIVERYDISPATCH_CUSTOMER_ROUTE", powershellRunner, StringComparison.Ordinal);
        Assert.False(Directory.Exists(Path.Combine(sampleRoot, "Server", "CourierGateway")));
    }

    [Fact]
    public void DeliveryDispatch_CourierSession_Bind_Registers_Binder_And_Replies_To_Client()
    {
        var sampleRoot = ResolveSampleRoot("DeliveryDispatch");
        var courierSessionBinder = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CourierSession",
            "CourierSessionBinder.cs"));
        var courierSessionHandler = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CourierSession",
            "BindCourierSessionHandler.cs"));
        var courierSessionHost = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CourierSession",
            "CourierSessionHostFactory.cs"));

        Assert.Contains(".GetOrCreate(courierId, SampleNames.CourierActorType)", courierSessionBinder,
            StringComparison.Ordinal);
        Assert.Contains("new EnsureCourierActorReq(courierId)", courierSessionBinder, StringComparison.Ordinal);
        Assert.Contains("context.Actors.BindOrGetAsync", courierSessionBinder, StringComparison.Ordinal);
        Assert.Contains("builder.Services.AddSingleton<CourierSessionBinder>()", courierSessionHost,
            StringComparison.Ordinal);
        Assert.Contains("await binder.BindAsync(request.CourierId", courierSessionHandler, StringComparison.Ordinal);
        Assert.Contains("context.Client.Reply(bound).Async(cancellationToken)", courierSessionHandler,
            StringComparison.Ordinal);
    }

    [Fact]
    public void DeliveryDispatch_Dispatch_Uses_Readiness_Health_Without_Business_Request_Retry()
    {
        var sampleRoot = ResolveSampleRoot("DeliveryDispatch");
        var dispatchHost = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Dispatch",
            "DispatchServerHostFactory.cs"));
        var dispatchAdapters = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Dispatch",
            "DispatchZLinkAdapters.cs"));
        var dispatchWorker = File.ReadAllText(Path.Combine(sampleRoot, "Server", "Dispatch",
            "DispatchWorker.cs"));

        Assert.Contains("IZLinkLocationReadiness", dispatchHost, StringComparison.Ordinal);
        Assert.Contains("readiness.IsPeerReadyAsync", dispatchHost, StringComparison.Ordinal);
        Assert.DoesNotContain("HasReadyCourierRouteAsync", dispatchHost, StringComparison.Ordinal);
        Assert.Contains("StatusCodes.Status503ServiceUnavailable", dispatchHost, StringComparison.Ordinal);
        Assert.DoesNotContain("new FindCourierActorReq(\"__health__\")", dispatchHost, StringComparison.Ordinal);
        Assert.DoesNotContain("RequestWithRetryAsync", dispatchAdapters, StringComparison.Ordinal);
        Assert.DoesNotContain("Task.Delay(100", dispatchAdapters, StringComparison.Ordinal);
        Assert.DoesNotContain("catch (Exception error)", dispatchAdapters, StringComparison.Ordinal);
        // The worker may not manufacture a courier timeout of its own — the deadline is a row in
        // the offer store, swept by a timer. It *may* catch around the sweeper and the queue pump,
        // because a supervision loop that dies on one bad delivery stops sweeping every other one.
        Assert.DoesNotContain("courier timeout", dispatchWorker, StringComparison.Ordinal);
        // Actor location is resolved by the framework. Dispatch does not select a physical node.
        Assert.Contains("SendToActor(", dispatchAdapters, StringComparison.Ordinal);
        // The offer is one-way and the decision comes back as its own message (common sample spec
        // §7.4). A request/reply offer would hold the courier node's serial queue open for as long
        // as the courier takes to answer, so the shape is asserted here, not just the behaviour.
        Assert.Contains("SendToActor(", dispatchAdapters, StringComparison.Ordinal);
        Assert.DoesNotContain("SendToSpot(", dispatchAdapters, StringComparison.Ordinal);
        Assert.DoesNotContain("OfferDeliveryReq", dispatchAdapters, StringComparison.Ordinal);
        Assert.DoesNotContain("OfferDeliveryRes ", dispatchAdapters, StringComparison.Ordinal);
        Assert.Contains("DeliveryOfferStore", dispatchWorker, StringComparison.Ordinal);

        // The shell is not the sample's configuration system: every role reads one file it is
        // given with --config, and no application file reads the environment
        // (framework/doc/framework/common/sample-e2e-configuration-policy.ko.md §2).
        foreach (var file in Directory.EnumerateFiles(sampleRoot, "*.cs", SearchOption.AllDirectories))
        {
            if (file.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal)
                || file.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal))
            {
                continue;
            }

            Assert.DoesNotContain("Environment.GetEnvironmentVariable", File.ReadAllText(file),
                StringComparison.Ordinal);
        }

        var runner = File.ReadAllText(Path.Combine(sampleRoot, "run_sample.sh"));
        Assert.DoesNotContain("export DELIVERYDISPATCH_DISPATCH", runner, StringComparison.Ordinal);
        Assert.Contains("--config", runner, StringComparison.Ordinal);
        Assert.Contains("OfferDeadlineSweeper", dispatchWorker, StringComparison.Ordinal);

        // Nothing on the courier node may wait for the courier's decision.
        var courierActor = File.ReadAllText(Path.Combine(sampleRoot, "Server", "CourierActorNode",
            "CourierActor.cs"));
        Assert.DoesNotContain("TaskCompletionSource", courierActor, StringComparison.Ordinal);
        Assert.Contains("RequestAsync<DeliveryStatusChangedReq, DeliveryStatusChangedRes>", dispatchAdapters,
            StringComparison.Ordinal);
    }
}
