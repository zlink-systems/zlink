using System.Collections;
using System.Net;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.LocationProvider;

namespace Zlink.Framework.UnitTests;

public sealed class ClientServerChannelRuntimeTests
{
    [Fact]
    public async Task GlobalClientServerMetadataFailureDisposesEachSendPartOnce()
    {
        await using var client = CreateClient(ReservePort());
        var runtime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var parts = new SingleAccessMessageParts();
        try
        {
            await Assert.ThrowsAsync<NotSupportedException>(async () =>
                await runtime.SendToChannelAsync(
                    "work",
                    parts,
                    CancellationToken.None,
                    new byte[] { 1 }));

            parts.AssertDisposedOnce();
        }
        finally
        {
            parts.DisposeRemaining();
        }
    }

    [Fact]
    public async Task GlobalClientServerMetadataFailureDisposesEachRequestPartOnce()
    {
        await using var client = CreateClient(ReservePort());
        var runtime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var parts = new SingleAccessMessageParts();
        try
        {
            await Assert.ThrowsAsync<NotSupportedException>(async () =>
                await runtime.RequestToChannelAsync(
                    "work",
                    parts,
                    TimeSpan.FromSeconds(1),
                    CancellationToken.None,
                    new byte[] { 1 }));

            parts.AssertDisposedOnce();
        }
        finally
        {
            parts.DisposeRemaining();
        }
    }

    [Fact]
    public async Task UnknownRouteChannelLookupDisposesSendPartsBeforeThrowing()
    {
        await using var client = CreateClient(ReservePort());
        var runtime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        var parts = new SingleAccessMessageParts();
        try
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await runtime.SendToChannelAsync(
                    "missing-route-channel",
                    parts,
                    CancellationToken.None));

            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
            parts.AssertDisposedOnce();
        }
        finally
        {
            parts.DisposeRemaining();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ServerOnlyClientServerChannelRejectsSendAndRequestAsNotConfigured()
    {
        await using var server = CreateServer(ReservePort());
        var runtime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var client = server.GetRequiredService<IZLinkRouteClient>();
            var sendError = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                client.SendToChannel("work", new EchoSend("send"))
                    .Async()
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.NotConfigured, sendError.Kind);

            var requestError = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                client.RequestToChannel("work", new EchoRequest("request"))
                    .Timeout(TimeSpan.FromMilliseconds(50))
                    .Async<EchoReply>()
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.NotConfigured, requestError.Kind);
            Assert.False(server.GetRequiredService<EchoProbe>().Received.Task.IsCompleted);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ClientRoleWithoutReadyTargetUsesAdmissionDeadlineNotConfigurationError()
    {
        await using var client = CreateClient(ReservePort());
        var runtime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                client.GetRequiredService<IZLinkRouteClient>()
                    .RequestToChannel("work", new EchoRequest("request"))
                    .Timeout(TimeSpan.FromMilliseconds(20))
                    .Async<EchoReply>()
                    .AsTask());

            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task CurrentSpotClientServerMetadataFailureUsesGlobalSendOwnership()
    {
        await using var client = CreateClient(ReservePort());
        var runtime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        var activation = new MetadataFailureSpotActivation();
        var endpoint = new ZLinkSpotOutboundEndpoint(
            activation,
            outbound: null!,
            runtime);
        activation.OutboundEndpoint = endpoint;
        var parts = new SingleAccessMessageParts();
        try
        {
            await Assert.ThrowsAsync<NotSupportedException>(async () =>
                await endpoint.SendToChannelAsync(
                    "work",
                    parts,
                    CancellationToken.None,
                    new byte[] { 1 }));

            parts.AssertDisposedOnce();
        }
        finally
        {
            parts.DisposeRemaining();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task CurrentSpotClientServerMetadataFailureUsesGlobalRequestOwnership()
    {
        await using var client = CreateClient(ReservePort());
        var runtime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        var activation = new MetadataFailureSpotActivation();
        var endpoint = new ZLinkSpotOutboundEndpoint(
            activation,
            outbound: null!,
            runtime);
        activation.OutboundEndpoint = endpoint;
        var parts = new SingleAccessMessageParts();
        try
        {
            await Assert.ThrowsAsync<NotSupportedException>(async () =>
                await endpoint.RequestToChannelAsync(
                    "work",
                    parts,
                    TimeSpan.FromSeconds(1),
                    CancellationToken.None,
                    new byte[] { 1 }));

            parts.AssertDisposedOnce();
        }
        finally
        {
            parts.DisposeRemaining();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ManualClient_RequestUsesDedicatedDealerAndServerRouter()
    {
        var port = ReservePort();
        await using var server = CreateServer(port);
        await using var client = CreateClient(port);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        Exception? runtimeFailure = null;
        serverRuntime.ErrorSink.UnhandledCallbackException += exception =>
            runtimeFailure = exception;

        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            var clientTransport =
                clientRuntime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => clientTransport.ReadyCount == 1,
                TimeSpan.FromSeconds(10));
            Assert.True(
                clientTransport.ReadyCount == 1,
                clientTransport.AdmissionDiagnostics);
            var serverState = await serverRuntime.EnsureStartedStateAsync(
                CancellationToken.None);
            var serverIdentity = GetServerBundle(serverState, "work")
                .ClientServerServer!;
            try
            {
                await WaitUntilAsync(
                    () => clientTransport.LivenessAckCount > 0
                        && serverIdentity.LivenessAckCount > 0,
                    TimeSpan.FromSeconds(8));
            }
            catch (TimeoutException exception)
            {
                throw new TimeoutException(
                    $"clientAck={clientTransport.LivenessAckCount}, clientProbe={clientTransport.ReceivedLivenessProbeCount}, clientSent={clientTransport.SentLivenessProbeCount}, serverAck={serverIdentity.LivenessAckCount}, serverProbe={serverIdentity.LivenessProbeCount}, serverReceived={serverIdentity.ReceivedLivenessProbeCount}, peers={serverIdentity.AdmittedPeerCount}, {clientTransport.AdmissionDiagnostics}",
                    exception);
            }
            await client.GetRequiredService<IZLinkRouteClient>()
                .SendToChannel("work", new EchoSend("queued"))
                .Async();
            Assert.Equal(
                "queued",
                await server.GetRequiredService<EchoProbe>().Received.Task
                    .WaitAsync(TimeSpan.FromSeconds(5)));

            EchoReply reply;
            try
            {
                reply = await client.GetRequiredService<IZLinkRouteClient>()
                    .RequestToChannel("work", new EchoRequest("ready"))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<EchoReply>();
            }
            catch when (runtimeFailure is not null)
            {
                throw new InvalidOperationException(
                    "ClientServer server runtime failed.",
                    runtimeFailure);
            }

            Assert.Equal("ready", reply.Value);
        }
        finally
        {
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task NegotiatedBoundRejectsOversizedSendAndRequestBeforeServerDispatch()
    {
        var port = ReservePort();
        await using var server = CreateServer(port, maximumMessageBytes: 512);
        await using var client = CreateClient(port, maximumMessageBytes: 4096);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();

        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            await WaitUntilAsync(
                () => clientRuntime.GetClientServerClientRuntime("work").ReadyCount == 1,
                TimeSpan.FromSeconds(5));
            var payload = new string('x', 1024);

            var sendError = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                client.GetRequiredService<IZLinkRouteClient>()
                    .SendToChannel("work", new EchoSend(payload))
                    .Async()
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, sendError.Kind);

            var requestError = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                client.GetRequiredService<IZLinkRouteClient>()
                    .RequestToChannel("work", new EchoRequest(payload))
                    .Timeout(TimeSpan.FromSeconds(2))
                    .Async<EchoReply>()
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, requestError.Kind);
            Assert.False(server.GetRequiredService<EchoProbe>().Received.Task.IsCompleted);
        }
        finally
        {
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task NegotiatedBoundConvertsOversizedServerReplyToCapacityExceeded()
    {
        var port = ReservePort();
        await using var server = CreateLargeReplyServer(port, maximumMessageBytes: 512);
        await using var client = CreateClient(port, maximumMessageBytes: 4096);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();

        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            await WaitUntilAsync(
                () => clientRuntime.GetClientServerClientRuntime("work").ReadyCount == 1,
                TimeSpan.FromSeconds(5));
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                client.GetRequiredService<IZLinkRouteClient>()
                    .RequestToChannel("work", new LargeReplyRequest("reply"))
                    .Timeout(TimeSpan.FromSeconds(2))
                    .Async<LargeReply>()
                    .AsTask());
            Assert.Equal(ZLinkFrameworkErrorKind.CapacityExceeded, error.Kind);
        }
        finally
        {
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ClientServerRequestPreservesInboundFlowIntoHandlerContext()
    {
        // Spec 27 §5: the ClientServer handler context preserves the caller's
        // flow pair, so downstream calls and the ambient-encoded reply reuse it.
        const string flowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
        var port = ReservePort();
        var services = new ServiceCollection();
        var probe = new FlowProbe();
        services.AddSingleton(probe);
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("work")
                .Server()
                .Listen(port)
                .AddRequestHandler<FlowEchoHandler, FlowEchoRequest, EchoReply>();
        });
        await using var server = services.BuildServiceProvider();
        await using var client = CreateClient(port);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();

        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            await WaitUntilAsync(
                () => clientRuntime.GetClientServerClientRuntime("work").ReadyCount == 1,
                TimeSpan.FromSeconds(5));

            EchoReply reply;
            using (ZLinkFlowContext.EnterExisting(flowId, ZLinkFlowOrigin.Application))
            {
                reply = await client.GetRequiredService<IZLinkRouteClient>()
                    .RequestToChannel("work", new FlowEchoRequest("flow"))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<EchoReply>();
            }

            Assert.Equal("flow", reply.Value);
            var observed = await probe.Handler.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(flowId, observed.FlowId);
            Assert.Equal(ZLinkFlowOrigin.Application, observed.Origin);
        }
        finally
        {
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public void CompleteMessageBoundAcceptsExactBoundaryAndRejectsOneByteAbove()
    {
        using var first = Message.From(new byte[200]);
        using var exact = Message.From(new byte[312]);
        using var above = Message.From(new byte[313]);

        Assert.True(ZLinkClientServerMessageBound.Fits([first, exact], 512));
        Assert.False(ZLinkClientServerMessageBound.Fits([first, above], 512));
    }

    [Fact]
    public async Task BlockingRequestHandler_DoesNotBlockClientServerLivenessControl()
    {
        var port = ReservePort();
        await using var server = CreateBlockingServer(port);
        await using var client = CreateClient(port);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var blocking = server.GetRequiredService<BlockingRequestProbe>();

        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            var clientTransport =
                clientRuntime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => clientTransport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));
            var serverState = await serverRuntime.EnsureStartedStateAsync(
                CancellationToken.None);
            var serverIdentity = GetServerBundle(serverState, "work")
                .ClientServerServer!;
            var request = client.GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new BlockingRequest("blocked"))
                .Timeout(TimeSpan.FromSeconds(15))
                .Async<EchoReply>()
                .AsTask();
            await blocking.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var baselineProbeCount = serverIdentity.LivenessProbeCount;

            await WaitUntilAsync(
                () => serverIdentity.LivenessProbeCount > baselineProbeCount,
                TimeSpan.FromSeconds(8));
            Assert.Equal(1, clientTransport.ReadyCount);

            blocking.Release.TrySetResult();
            Assert.Equal(
                "blocked",
                (await request.WaitAsync(TimeSpan.FromSeconds(5))).Value);
        }
        finally
        {
            blocking.Release.TrySetResult();
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task CancellationIgnoringRequestHandler_DoesNotBlockRuntimeStopOrReplyLate()
    {
        var port = ReservePort();
        await using var server = CreateBlockingServer(port);
        await using var client = CreateClient(port);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var blocking = server.GetRequiredService<BlockingRequestProbe>();
        Exception? runtimeFailure = null;
        serverRuntime.ErrorSink.UnhandledCallbackException += exception =>
            runtimeFailure = exception;

        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            var clientTransport =
                clientRuntime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => clientTransport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));
            var request = client.GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new BlockingRequest("late"))
                .Timeout(TimeSpan.FromSeconds(3))
                .Async<EchoReply>()
                .AsTask();
            await blocking.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));

            await serverRuntime.StopAsync(CancellationToken.None)
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(3));
            blocking.Release.TrySetResult();
            await blocking.Completed.Task.WaitAsync(TimeSpan.FromSeconds(2));
            await Assert.ThrowsAnyAsync<Exception>(async () => await request);
            Assert.Null(runtimeFailure);
        }
        finally
        {
            blocking.Release.TrySetResult();
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ServerPushedDrainingUpdate_RemovesManualClientFromReadySet()
    {
        var port = ReservePort();
        await using var server = CreateServer(port, maximumMessageBytes: 4096);
        await using var client = CreateClient(port, maximumMessageBytes: 512);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();

        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            var transport =
                clientRuntime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => transport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));
            var serverState = await serverRuntime.EnsureStartedStateAsync(
                CancellationToken.None);
            GetServerBundle(serverState, "work")
                .ClientServerServer!
                .MarkDraining();
            await WaitUntilAsync(
                () => transport.ReadyCount == 0,
                TimeSpan.FromSeconds(8));
            GetServerBundle(serverState, "work")
                .ClientServerServer!
                .MarkServing();
            await WaitUntilAsync(
                () => transport.ReadyCount == 1,
                TimeSpan.FromSeconds(8));
        }
        finally
        {
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task LocalOnlyClientAndServer_UseDealerRouterTransportWithoutLocationStore()
    {
        await using var provider = CreateLocalClientAndServer();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();

        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var transport = runtime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => transport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));

            Assert.Equal(1, transport.PhysicalConnectionCount);
            var reply = await provider.GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new EchoRequest("local"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<EchoReply>();
            Assert.Equal("local:local", reply.Value);

            var state = await runtime.EnsureStartedStateAsync(
                CancellationToken.None);
            GetServerBundle(state, "work")
                .ClientServerServer!
                .MarkDraining();
            await WaitUntilAsync(
                () => transport.ReadyCount == 0,
                TimeSpan.FromSeconds(2));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task ExactRuntimeProjectsLiveLocalServerAndPublishesDrainingEvent()
    {
        await using var provider = CreateLocalClientAndServer();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var monitoring =
            provider.GetRequiredService<IZLinkClientServerRuntime>();
        var hostLifecycle =
            provider.GetRequiredService<ZLinkFrameworkHostLifecycleState>();
        hostLifecycle.TransitionTo(ZLinkFrameworkRuntimeState.Serving);

        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var transport = runtime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => transport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));

            var ready = monitoring.GetStatus("work");
            Assert.Equal(
                Zlink.Framework.Contracts.Configuration
                    .ZLinkClientServerRole.ClientAndServer,
                ready.LocalRole);
            Assert.True(ready.IsReady);
            var readyServer = Assert.Single(ready.Targets);
            Assert.Equal(
                ZLinkPeerState.Ready,
                readyServer.State);

            hostLifecycle.TransitionTo(ZLinkFrameworkRuntimeState.Relocating);
            var relocating = monitoring.GetStatus("work");
            Assert.False(relocating.IsReady);
            Assert.Equal(ZLinkTopologyState.Stopping, relocating.State);
            Assert.Equal(1, relocating.ReadyTargetCount);
            hostLifecycle.TransitionTo(ZLinkFrameworkRuntimeState.Serving);

            using var timeout = new CancellationTokenSource(
                TimeSpan.FromSeconds(8));
            await using var events = monitoring.ObserveAsync(
                    "work",
                    timeout.Token)
                .GetAsyncEnumerator(timeout.Token);
            await using var secondEvents = monitoring.ObserveAsync(
                    "work",
                    timeout.Token)
                .GetAsyncEnumerator(timeout.Token);
            var firstChange = WaitForTargetStateAsync(
                events,
                ZLinkPeerState.Draining);
            var secondChange = WaitForTargetStateAsync(
                secondEvents,
                ZLinkPeerState.Draining);
            var state = await runtime.EnsureStartedStateAsync(
                CancellationToken.None);
            GetServerBundle(state, "work")
                .ClientServerServer!
                .MarkDraining();

            Assert.True(await firstChange);
            Assert.True(await secondChange);
            Assert.Equal(
                ZLinkPeerState.Draining,
                Assert.Single(events.Current.Status.Targets).State);
            Assert.Equal(
                ZLinkPeerState.Draining,
                Assert.Single(secondEvents.Current.Status.Targets).State);
            Assert.False(events.Current.Status.IsReady);
            Assert.False(monitoring.GetStatus("work").IsReady);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static async Task<bool> WaitForTargetStateAsync(
        IAsyncEnumerator<ZLinkObservedStatus<ZLinkClientServerStatus>> observer,
        ZLinkPeerState expected)
    {
        while (await observer.MoveNextAsync())
            if (observer.Current.Status.Targets.Any(target =>
                    target.State == expected))
                return true;
        return false;
    }

    [Fact]
    public async Task LocalOnlyClient_DoesNotSelectZeroWeightServer()
    {
        await using var provider = CreateLocalClientAndServer(weight: 0);
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();

        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var transport = runtime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => transport.AdmissionCompletedCount == 1,
                TimeSpan.FromSeconds(5));
            Assert.Equal(0, transport.ReadyCount);
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await provider.GetRequiredService<IZLinkRouteClient>()
                    .SendToChannel("work", new EchoSend("excluded"))
                    .Async());
            Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task AutomaticClient_UsesDedicatedDescriptorAndActualBoundEndpoint()
    {
        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(locationProvider);
        await using var server = CreateAutomaticServer(locationProvider, "only");
        await using var client = CreateAutomaticClient(locationProvider);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var serverLocations = server.GetRequiredService<ZLinkLocationRuntime>();
        var clientLocations = client.GetRequiredService<ZLinkLocationRuntime>();
        var serverDiscovery = server.GetRequiredService<ZLinkLocationAutoConnectHost>();
        var clientDiscovery = client.GetRequiredService<ZLinkLocationAutoConnectHost>();

        await serverLocations.StartAsync(RoutingId.From("server-owner"));
        await clientLocations.StartAsync(RoutingId.From("client-owner"));
        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            await serverDiscovery.StartAsync(
                await serverRuntime.EnsureStartedStateAsync(CancellationToken.None));
            await clientDiscovery.StartAsync(
                await clientRuntime.EnsureStartedStateAsync(CancellationToken.None));

            var descriptorPage = await store.ListClientServersAsync(
                "work",
                new ZLinkPageRequest(16));
            var descriptor = Assert.Single(descriptorPage.Items);
            Assert.NotEqual(0UL, descriptor.LifecycleGeneration);
            Assert.NotEqual(0UL, descriptor.DescriptorRevision);
            Assert.NotEqual(0, descriptor.ServerRid.Size);
            Assert.DoesNotContain(":0", descriptor.Endpoint, StringComparison.Ordinal);

            var reply = await client.GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new EchoRequest("automatic"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<EchoReply>();
            Assert.Equal("only:automatic", reply.Value);

            Assert.True(await serverDiscovery.MarkDrainingAsync());
            descriptorPage = await store.ListClientServersAsync(
                "work",
                new ZLinkPageRequest(16));
            descriptor = Assert.Single(descriptorPage.Items);
            Assert.Equal(ZLinkFrameworkRuntimeState.Draining, descriptor.State);
            Assert.Equal(0, descriptor.Weight);
            Assert.Equal(2UL, descriptor.DescriptorRevision);
        }
        finally
        {
            await clientDiscovery.StopAsync();
            await serverDiscovery.StopAsync();
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
            await clientLocations.StopAsync();
            await serverLocations.StopAsync();
        }

        Assert.Empty((await store.ListClientServersAsync(
            "work",
            new ZLinkPageRequest(16))).Items);
    }

    [Fact]
    public async Task AutomaticClient_SelectsAcrossPositiveWeightReadyServers()
    {
        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(locationProvider);
        await using var first = CreateAutomaticServer(locationProvider, "first", weight: 100);
        await using var second = CreateAutomaticServer(locationProvider, "second", weight: 300);
        await using var excluded = CreateAutomaticServer(locationProvider, "excluded", weight: 0);
        await using var client = CreateAutomaticClient(locationProvider);
        var providers = new[] { first, second, excluded, client };
        foreach (var provider in providers)
            await provider.GetRequiredService<ZLinkLocationRuntime>()
                .StartAsync(RoutingId.From(Guid.NewGuid().ToString("N")));
        foreach (var provider in providers)
            await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                .StartAsync(CancellationToken.None);
        try
        {
            foreach (var provider in providers)
                await provider.GetRequiredService<ZLinkLocationAutoConnectHost>()
                    .StartAsync(await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                        .EnsureStartedStateAsync(CancellationToken.None));

            var clientTransport =
                client.GetRequiredService<ZLinkFrameworkRuntime>()
                    .GetClientServerClientRuntime("work");
            try
            {
                await WaitUntilAsync(
                    () => clientTransport.ReadyCount == 2
                        && clientTransport.AdmissionCompletedCount == 3,
                    TimeSpan.FromSeconds(10));
            }
            catch (TimeoutException exception)
            {
                throw new TimeoutException(
                    clientTransport.AdmissionDiagnostics,
                    exception);
            }
            var route = client.GetRequiredService<IZLinkRouteClient>();
            var selected = new Dictionary<string, int>(StringComparer.Ordinal);
            for (var index = 0; index < 12; index++)
            {
                var reply = await route.RequestToChannel(
                        "work",
                        new EchoRequest(index.ToString()))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<EchoReply>();
                var server = reply.Value.Split(':', 2)[0];
                selected[server] = selected.GetValueOrDefault(server) + 1;
            }

            Assert.Equal(
                new[] { "first", "second" },
                selected.Keys.Order(StringComparer.Ordinal));
            Assert.Equal(3, selected["first"]);
            Assert.Equal(9, selected["second"]);

            var runtimeOptions =
                first.GetRequiredService<IZLinkRouteMeshRuntimeOptions>();
            runtimeOptions.Channel("work").Weight = 0;
            Assert.Throws<ZLinkConfigurationException>(
                () => runtimeOptions.Channel("work").Weight = 10_001);
            await WaitUntilAsync(
                () => clientTransport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));
            for (var index = 0; index < 4; index++)
            {
                var reply = await route.RequestToChannel(
                        "work",
                        new EchoRequest($"after-{index}"))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<EchoReply>();
                Assert.StartsWith("second:", reply.Value);
            }
            var ownerId = first.GetRequiredService<ZLinkLocationRuntime>()
                .OwnerId;
            ZLinkClientServerServerDescriptor? updated = null;
            var updateDeadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
            while (DateTime.UtcNow < updateDeadline)
            {
                updated = (await store.ListClientServersAsync(
                        "work",
                        new ZLinkPageRequest(16))).Items
                    .Single(row => row.OwnerId == ownerId);
                if (updated.Weight == 0)
                    break;
                await Task.Delay(10);
            }
            Assert.Equal(0, updated?.Weight);
        }
        finally
        {
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkLocationAutoConnectHost>().StopAsync();
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                    .StopAsync(CancellationToken.None);
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkLocationRuntime>().StopAsync();
        }
    }

    [Fact]
    public async Task ManualAndAutomaticIntents_ShareAdmittedServerIdentity()
    {
        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(locationProvider);
        await using var server = CreateAutomaticServer(locationProvider, "shared");
        await using var client = CreateAutomaticClient(locationProvider);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var serverLocations = server.GetRequiredService<ZLinkLocationRuntime>();
        var clientLocations = client.GetRequiredService<ZLinkLocationRuntime>();
        var serverDiscovery = server.GetRequiredService<ZLinkLocationAutoConnectHost>();
        var clientDiscovery = client.GetRequiredService<ZLinkLocationAutoConnectHost>();

        await serverLocations.StartAsync(RoutingId.From("shared-server"));
        await clientLocations.StartAsync(RoutingId.From("shared-client"));
        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            await serverDiscovery.StartAsync(
                await serverRuntime.EnsureStartedStateAsync(CancellationToken.None));
            await clientDiscovery.StartAsync(
                await clientRuntime.EnsureStartedStateAsync(CancellationToken.None));

            var descriptor = Assert.Single((await store.ListClientServersAsync(
                "work",
                new ZLinkPageRequest(16))).Items);
            var transport =
                clientRuntime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => transport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));

            transport.AddManual(descriptor.Endpoint);
            await WaitUntilAsync(
                () => transport.ConnectionIntentCount == 2
                    && transport.PhysicalConnectionCount == 1,
                TimeSpan.FromSeconds(5));

            transport.RemoveManual(descriptor.Endpoint);
            Assert.Equal(1, transport.ConnectionIntentCount);
            Assert.Equal(1, transport.PhysicalConnectionCount);
            var reply = await client.GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new EchoRequest("deduped"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<EchoReply>();
            Assert.Equal("shared:deduped", reply.Value);
        }
        finally
        {
            await clientDiscovery.StopAsync();
            await serverDiscovery.StopAsync();
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
            await clientLocations.StopAsync();
            await serverLocations.StopAsync();
        }
    }

    [Fact]
    public async Task ServerRestart_ReusesRidAndPublishesNewLifecycleIntent()
    {
        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(locationProvider);
        var port = ReservePort();
        await using var server = CreateAutomaticServer(locationProvider, "restart", port: port);
        await using var client = CreateAutomaticClient(locationProvider);
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var serverLocations = server.GetRequiredService<ZLinkLocationRuntime>();
        var clientLocations = client.GetRequiredService<ZLinkLocationRuntime>();
        var serverDiscovery = server.GetRequiredService<ZLinkLocationAutoConnectHost>();
        var clientDiscovery = client.GetRequiredService<ZLinkLocationAutoConnectHost>();

        await serverLocations.StartAsync(RoutingId.From("restart-owner"));
        await clientLocations.StartAsync(RoutingId.From("restart-client"));
        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            await serverDiscovery.StartAsync(
                await serverRuntime.EnsureStartedStateAsync(CancellationToken.None));
            await clientDiscovery.StartAsync(
                await clientRuntime.EnsureStartedStateAsync(CancellationToken.None));
            var first = Assert.Single((await store.ListClientServersAsync(
                "work",
                new ZLinkPageRequest(16))).Items);
            var clientTransport =
                clientRuntime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => clientTransport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));

            await serverDiscovery.StopAsync();
            await serverRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StartAsync(CancellationToken.None);
            await serverDiscovery.StartAsync(
                await serverRuntime.EnsureStartedStateAsync(CancellationToken.None));

            var second = Assert.Single((await store.ListClientServersAsync(
                "work",
                new ZLinkPageRequest(16))).Items);
            Assert.Equal(first.ServerRid, second.ServerRid);
            Assert.NotEqual(first.LifecycleGeneration, second.LifecycleGeneration);
            Assert.Equal(first.Endpoint, second.Endpoint);
            Assert.Equal(1UL, second.DescriptorRevision);

            await Task.Delay(TimeSpan.FromSeconds(2));
            await WaitUntilAsync(
                () => clientTransport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));
            var reply = await client.GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new EchoRequest("after-restart"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<EchoReply>();
            Assert.Equal("restart:after-restart", reply.Value);
        }
        finally
        {
            await clientDiscovery.StopAsync();
            await serverDiscovery.StopAsync();
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
            await clientLocations.StopAsync();
            await serverLocations.StopAsync();
        }
    }

    private static async Task WaitUntilAsync(
        Func<bool> condition,
        TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (!condition())
        {
            if (DateTime.UtcNow >= deadline)
                throw new TimeoutException("ClientServer condition was not reached.");
            await Task.Delay(10);
        }
    }

    private static ZLinkChannelRuntimeBundle GetServerBundle(
        ZLinkFrameworkComponentState state,
        string channelName) =>
        state.ClientServerServerBundles[ZLinkChannelName.FromBoundary(
            channelName,
            nameof(channelName))];

    [Fact]
    public void ClientServerRegistration_AllowsEachRoleOnceAndRejectsDuplicateRole()
    {
        var unselected = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(
                options => options.AddClientServerChannel("work")));
        Assert.Contains("at least once", unselected.Message, StringComparison.Ordinal);

        var services = new ServiceCollection();
        services.AddSingleton<EchoProbe>();
        services.AddSingleton(new ServerIdentity("local"));
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("work").Client().Connect("tcp://127.0.0.1:7001");
            options.AddClientServerChannel("work")
                .Server()
                .Listen(0)
                .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
        });

        var duplicateClient = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
            {
                options.AddClientServerChannel("work").Client().Connect("tcp://127.0.0.1:7001");
                options.AddClientServerChannel("work").Client().Connect("tcp://127.0.0.1:7002");
            }));
        Assert.Contains("role 'Client' is already registered", duplicateClient.Message, StringComparison.Ordinal);

        var duplicateServer = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
            {
                options.AddClientServerChannel("work")
                    .Server()
                    .Listen(0)
                    .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
                options.AddClientServerChannel("work").Server();
            }));
        Assert.Contains("role 'Server' is already registered", duplicateServer.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ClientServerRegistration_AllowsDifferentNamesAndRejectsRouteMeshCollision()
    {
        var services = new ServiceCollection();
        services.AddSingleton<EchoProbe>();
        services.AddSingleton(new ServerIdentity("local"));
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("orders").Client().Connect("tcp://127.0.0.1:7001");
            options.AddClientServerChannel("billing").Client().Connect("tcp://127.0.0.1:7002");
            options.AddClientServerChannel("orders-server")
                .Server()
                .Listen(0)
                .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
            options.AddClientServerChannel("billing-server")
                .Server()
                .Listen(0)
                .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
        });

        var collision = Assert.Throws<ZLinkConfigurationException>(() =>
            new ServiceCollection().AddZLinkFramework(options =>
            {
                options.AddRouteMesh("mesh")
                    .Listen("tcp://127.0.0.1:0")
                    .Channel("orders")
                    .Server();
                options.AddClientServerChannel("orders")
                    .Client()
                    .Connect("tcp://127.0.0.1:7001");
            }));
        Assert.Contains(
            "registered on both RouteMesh and ClientServer physical paths",
            collision.Message,
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task AutomaticClient_IncludesSameProcessServerWithoutLocalPreferenceOrBypass()
    {
        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(locationProvider);
        await using var local = CreateAutomaticClientAndServer(locationProvider, "local");
        await using var remote = CreateAutomaticServer(locationProvider, "remote");
        var providers = new[] { local, remote };

        foreach (var provider in providers)
            await provider.GetRequiredService<ZLinkLocationRuntime>()
                .StartAsync(RoutingId.From(Guid.NewGuid().ToString("N")));
        foreach (var provider in providers)
            await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                .StartAsync(CancellationToken.None);

        try
        {
            foreach (var provider in providers)
                await provider.GetRequiredService<ZLinkLocationAutoConnectHost>()
                    .StartAsync(await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                        .EnsureStartedStateAsync(CancellationToken.None));

            var transport = local.GetRequiredService<ZLinkFrameworkRuntime>()
                .GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => transport.ReadyCount == 2,
                TimeSpan.FromSeconds(10));

            var route = local.GetRequiredService<IZLinkRouteClient>();
            var selected = new HashSet<string>(StringComparer.Ordinal);
            for (var index = 0; index < 12; index++)
            {
                var reply = await route.RequestToChannel(
                        "work",
                        new EchoRequest(index.ToString()))
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<EchoReply>();
                selected.Add(reply.Value.Split(':', 2)[0]);
            }

            Assert.Equal(
                new[] { "local", "remote" },
                selected.OrderBy(static value => value, StringComparer.Ordinal));

            Assert.True(
                await local.GetRequiredService<ZLinkLocationAutoConnectHost>()
                    .MarkDrainingAsync());
            await WaitUntilAsync(
                () => transport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));
            var afterDrain = await route.RequestToChannel(
                    "work",
                    new EchoRequest("after-drain"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<EchoReply>();
            Assert.Equal("remote:after-drain", afterDrain.Value);
        }
        finally
        {
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkLocationAutoConnectHost>().StopAsync();
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                    .StopAsync(CancellationToken.None);
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkLocationRuntime>().StopAsync();
        }
    }

    [Fact]
    public async Task AutomaticClient_ExcludesSameProcessZeroWeightServer()
    {
        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(locationProvider);
        await using var local = CreateAutomaticClientAndServer(
            locationProvider,
            "local",
            weight: 0);
        await using var remote = CreateAutomaticServer(locationProvider, "remote");
        var providers = new[] { local, remote };

        foreach (var provider in providers)
            await provider.GetRequiredService<ZLinkLocationRuntime>()
                .StartAsync(RoutingId.From(Guid.NewGuid().ToString("N")));
        foreach (var provider in providers)
            await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                .StartAsync(CancellationToken.None);

        try
        {
            foreach (var provider in providers)
                await provider.GetRequiredService<ZLinkLocationAutoConnectHost>()
                    .StartAsync(await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                        .EnsureStartedStateAsync(CancellationToken.None));

            var transport = local.GetRequiredService<ZLinkFrameworkRuntime>()
                .GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => transport.ReadyCount == 1
                    && transport.AdmissionCompletedCount == 2,
                TimeSpan.FromSeconds(10));

            var reply = await local.GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new EchoRequest("weighted"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<EchoReply>();
            Assert.Equal("remote:weighted", reply.Value);
        }
        finally
        {
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkLocationAutoConnectHost>().StopAsync();
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkFrameworkRuntime>()
                    .StopAsync(CancellationToken.None);
            foreach (var provider in providers.Reverse())
                await provider.GetRequiredService<ZLinkLocationRuntime>().StopAsync();
        }
    }

    [Fact]
    public void ClientServerControlProtocol_UsesExactBinaryAdmissionRecords()
    {
        using var hello = ZLinkClientServerControlProtocol.EncodeHello(
            new ZLinkClientServerControlProtocol.Hello(
                "work",
                "plaintext",
                4096));
        Assert.Equal(
            "5A4D010100020000001701001404776F726B0109706C61696E7465787400001000",
            Convert.ToHexString(hello.AsReadOnlyMemory().Span));
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeHello(
                [hello],
                out var decodedHello));
        Assert.Equal("work", decodedHello!.ChannelName);
        Assert.Equal("plaintext", decodedHello.SecurityIdentity);
        Assert.Equal(4096U, decodedHello.NormalizedEffectiveMaxMessageBytes);

        using var admit = ZLinkClientServerControlProtocol.EncodeAdmission(
            new ZLinkClientServerControlProtocol.Admission(
                "work",
                RoutingId.From("server-a"),
                3,
                7,
                75,
                ZLinkFrameworkRuntimeState.Serving,
                "plaintext",
                4096,
                "tcp://127.0.0.1:7002"));
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeAdmission(
                [admit],
                out var decodedAdmission));
        Assert.Equal(RoutingId.From("server-a"), decodedAdmission!.ServerRid);
        Assert.Equal(3UL, decodedAdmission.LifecycleGeneration);
        Assert.Equal(7UL, decodedAdmission.DescriptorRevision);
        Assert.Equal(75, decodedAdmission.Weight);
        Assert.Equal("tcp://127.0.0.1:7002", decodedAdmission.AdvertisedEndpoint);

        using var update = ZLinkClientServerControlProtocol.EncodeUpdate(
            decodedAdmission with
            {
                DescriptorRevision = 8,
                Weight = 0,
                State = ZLinkFrameworkRuntimeState.Draining
            });
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeUpdate(
                [update],
                out var decodedUpdate));
        Assert.Equal(8UL, decodedUpdate!.DescriptorRevision);
        Assert.Equal(0, decodedUpdate.Weight);
        Assert.Equal(
            ZLinkFrameworkRuntimeState.Draining,
            decodedUpdate.State);

        using var probe =
            ZLinkClientServerControlProtocol.EncodeLivenessProbe(11);
        Assert.Equal(
            "5A4D010500000000000000000B",
            Convert.ToHexString(probe.AsReadOnlyMemory().Span));
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(
                [probe],
                out var probeId));
        Assert.Equal(11UL, probeId);

        using var ack =
            ZLinkClientServerControlProtocol.EncodeLivenessAck(11);
        Assert.Equal(
            "5A4D010600000000000000000B",
            Convert.ToHexString(ack.AsReadOnlyMemory().Span));
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeLivenessAck(
                [ack],
                out var ackId));
        Assert.Equal(11UL, ackId);

        using var reject = ZLinkClientServerControlProtocol.EncodeReject(1);
        Assert.Equal(
            "5A4D01030000000001",
            Convert.ToHexString(reject.AsReadOnlyMemory().Span));
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeReject(
                [reject],
                out var reason));
        Assert.Equal(1U, reason);
    }

    [Fact]
    public void ClientServerControlProtocol_AdmissionDecode_NormalizesAdvertisedEndpoint()
    {
        // The advertised endpoint is accepted from the wire (a peer,
        // potentially another language or an older build). Per
        // doc/plan/endpoint-notation-policy.ko.md §2.3, notation is
        // normalized once at the point it is accepted so downstream Ordinal
        // comparisons (e.g. against the client's expected endpoint from the
        // location store) cannot fail on notation alone.
        using var admit = ZLinkClientServerControlProtocol.EncodeAdmission(
            new ZLinkClientServerControlProtocol.Admission(
                "work",
                RoutingId.From("server-notation"),
                1,
                1,
                100,
                ZLinkFrameworkRuntimeState.Serving,
                "plaintext",
                4096,
                "TCP://Host.Example.com:0443/"));

        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeAdmission(
                [admit],
                out var decoded));
        Assert.Equal(
            "tcp://host.example.com:443",
            decoded!.AdvertisedEndpoint);
    }

    [Fact]
    public async Task AutomaticClient_AdmitsDespiteAdvertisedEndpointNotationDifferingFromExpected()
    {
        // Pin for ZLinkClientServerClientRuntime.ApplyAdmission's hard-reject
        // branch: it does a raw Ordinal compare between admission
        // .AdvertisedEndpoint (decoded off the wire) and expected.Endpoint
        // (the automatic descriptor, e.g. sourced from the location store).
        // Drive the wire directly with a raw router so the two values can be
        // given different (but equivalent) notation while every other
        // admission field matches exactly -- isolating endpoint notation as
        // the only variable. Before ZLinkClientServerControlProtocol's
        // decode-side normalization existed, this made the client set
        // _rejected = true forever (a hard connection failure, not a silent
        // no-op).
        var port = ReservePort();
        var endpoint = $"tcp://127.0.0.1:{port}";
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        router.Bind(endpoint);

        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        await using var client = CreateAutomaticClient(locationProvider);
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            var transport = clientRuntime.GetClientServerClientRuntime("work");
            var serverRid = RoutingId.From("notation-server");
            var expected = new ZLinkClientServerServerDescriptor(
                "work",
                serverRid,
                LifecycleGeneration: 1,
                DescriptorRevision: 1,
                endpoint, // canonical, dialable -- what the client connects to
                Weight: 100,
                ZLinkFrameworkRuntimeState.Serving,
                SecurityIdentity: "plaintext",
                OwnerId: "test-owner",
                LeaseGeneration: 1,
                UpdatedAt: default);
            transport.ReplaceAutomatic([expected]);

            using var hello = await PollReceivedAsync(
                storage => TryReceive(router, storage),
                TimeSpan.FromSeconds(5));
            Assert.True(
                ZLinkClientServerControlProtocol.TryDecodeHello(
                    hello.Parts,
                    out _));

            // Same physical target as `endpoint`, but different (equivalent)
            // notation: uppercase scheme, leading-zero port, trailing slash.
            var mismatchedNotation = $"TCP://127.0.0.1:0{port}/";
            var admission = ZLinkClientServerControlProtocol.EncodeAdmission(
                new ZLinkClientServerControlProtocol.Admission(
                    "work",
                    serverRid,
                    1,
                    1,
                    100,
                    ZLinkFrameworkRuntimeState.Serving,
                    "plaintext",
                    1024 * 1024,
                    mismatchedNotation));
            router.Reply(
                    hello.RoutingId
                        ?? throw new InvalidOperationException(
                            "missing client routing id"),
                    hello.RequestSeq
                        ?? throw new InvalidOperationException(
                            "missing request sequence"))
                .Message(admission)
                .Submit();

            try
            {
                await WaitUntilAsync(
                    () => transport.ReadyCount == 1,
                    TimeSpan.FromSeconds(5));
            }
            catch (TimeoutException exception)
            {
                throw new TimeoutException(
                    transport.AdmissionDiagnostics,
                    exception);
            }
            Assert.Equal(1, transport.ReadyCount);
        }
        finally
        {
            await clientRuntime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task AutomaticClient_AdmitsDespiteLocationStoreEndpointNotationDifference()
    {
        // Pin for doc/plan/endpoint-notation-policy.ko.md §2.3 row-intake
        // normalization (ZLinkClientServerDiscovery.ClientLoop.ListAllAsync):
        // a Location Store row written with different (but equivalent)
        // endpoint notation than the server's own canonical value must still
        // be usable as a dial target and match the server's live wire
        // admission once normalized on read.
        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(locationProvider);
        await using var server = CreateAutomaticServer(locationProvider, "notation");
        await using var client = CreateAutomaticClient(locationProvider);
        var serverLocations = server.GetRequiredService<ZLinkLocationRuntime>();
        var clientLocations = client.GetRequiredService<ZLinkLocationRuntime>();
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var serverDiscovery = server.GetRequiredService<ZLinkLocationAutoConnectHost>();
        var clientDiscovery = client.GetRequiredService<ZLinkLocationAutoConnectHost>();

        await serverLocations.StartAsync(RoutingId.From("notation-owner"));
        await clientLocations.StartAsync(RoutingId.From("notation-client"));
        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            await serverDiscovery.StartAsync(
                await serverRuntime.EnsureStartedStateAsync(CancellationToken.None));
            var actual = Assert.Single(
                (await store.ListClientServersAsync(
                    "work",
                    new ZLinkPageRequest(16))).Items);
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                await store.RemoveClientServerAsync(
                    new ZLinkClientServerServerDescriptorKey(
                        actual.ChannelName,
                        actual.ServerRid),
                    serverLocations.OwnerToken));

            // Same physical target, different (but equivalent) notation:
            // uppercase scheme, leading-zero port, trailing slash. The
            // server still binds and admits using its own canonical value.
            var mismatchedNotation =
                "TCP://" + new Uri(actual.Endpoint, UriKind.Absolute).Host
                + ":0" + new Uri(actual.Endpoint, UriKind.Absolute).Port + "/";
            var forged = actual with { Endpoint = mismatchedNotation };
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (await store.UpdateClientServerAsync(
                    forged,
                    ZLinkLocationWriteIntent.NewClaim)).Status);

            await clientDiscovery.StartAsync(
                await clientRuntime.EnsureStartedStateAsync(CancellationToken.None));
            var transport = clientRuntime.GetClientServerClientRuntime("work");
            try
            {
                await WaitUntilAsync(
                    () => transport.ReadyCount == 1,
                    TimeSpan.FromSeconds(5));
            }
            catch (TimeoutException exception)
            {
                throw new TimeoutException(
                    $"forged={forged.Endpoint} {transport.AdmissionDiagnostics}",
                    exception);
            }
            Assert.Equal(1, transport.ReadyCount);

            var reply = await client.GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new EchoRequest("notation"))
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<EchoReply>();
            Assert.Equal("notation:notation", reply.Value);
        }
        finally
        {
            await clientDiscovery.StopAsync();
            await serverDiscovery.StopAsync();
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
            await clientLocations.StopAsync();
            await serverLocations.StopAsync();
        }
    }

    [Fact]
    public async Task BindingSockets_DeliverUnsolicitedLivenessProbe()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var router = context.CreateRouterSocket();
        await using var dealer = context.CreateDealerSocket();
        var endpoint = $"inproc://liveness-probe-{Guid.NewGuid():N}";
        dealer.SetRoutingId(RoutingId.From("probe-client"));
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        var hello = ZLinkClientServerControlProtocol.EncodeHello(
            new ZLinkClientServerControlProtocol.Hello(
                "work",
                "plaintext",
                4096));
        var admissionTask = dealer.Request()
            .Message(hello)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async(CancellationToken.None);
        using var inbound = await PollReceivedAsync(
            storage => router.Recv(storage, RecvFlags.DontWait),
            TimeSpan.FromSeconds(2));
        var sourceRid = Assert.IsType<RoutingId>(inbound.RoutingId);
        var requestSeq = Assert.IsType<ulong>(inbound.RequestSeq);
        var admit = ZLinkClientServerControlProtocol.EncodeAdmission(
            new ZLinkClientServerControlProtocol.Admission(
                "work",
                RoutingId.From("probe-server"),
                1,
                1,
                100,
                ZLinkFrameworkRuntimeState.Serving,
                "plaintext",
                4096,
                endpoint));
        router.Reply(sourceRid, requestSeq).Message(admit).Submit();
        ZLinkMessageParts.DisposeAll(await admissionTask);

        var probe =
            ZLinkClientServerControlProtocol.EncodeLivenessProbe(17);
        await router.Send(sourceRid)
            .Message(probe)
            .Async(CancellationToken.None);
        using var delivered = await PollReceivedAsync(
            storage => dealer.Recv(storage, RecvFlags.DontWait),
            TimeSpan.FromSeconds(2));
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(
                delivered.Parts,
                out var probeId));
        Assert.Equal(17UL, probeId);
    }

    private static async Task<Received> PollReceivedAsync(
        Func<Received, bool> receive,
        TimeSpan timeout)
    {
        var received = Received.Create();
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (receive(received))
                return received;
            await Task.Delay(5);
        }
        received.Dispose();
        throw new TimeoutException("Expected raw record was not received.");
    }

    [Fact]
    public async Task AutomaticClient_RejectsDescriptorWhoseAdmissionIdentityDoesNotMatch()
    {
        var locationProvider = new ZLinkInMemoryProviderLocationStore();
        var store = new ZLinkProviderLocationRepository(locationProvider);
        await using var server = CreateAutomaticServer(locationProvider, "identity");
        await using var client = CreateAutomaticClient(locationProvider);
        var serverLocations = server.GetRequiredService<ZLinkLocationRuntime>();
        var clientLocations = client.GetRequiredService<ZLinkLocationRuntime>();
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        var serverDiscovery =
            server.GetRequiredService<ZLinkLocationAutoConnectHost>();
        var clientDiscovery =
            client.GetRequiredService<ZLinkLocationAutoConnectHost>();

        await serverLocations.StartAsync(RoutingId.From("identity-owner"));
        await clientLocations.StartAsync(RoutingId.From("identity-client"));
        await serverRuntime.StartAsync(CancellationToken.None);
        await clientRuntime.StartAsync(CancellationToken.None);
        try
        {
            await serverDiscovery.StartAsync(
                await serverRuntime.EnsureStartedStateAsync(
                    CancellationToken.None));
            var actual = Assert.Single(
                (await store.ListClientServersAsync(
                    "work",
                    new ZLinkPageRequest(16))).Items);
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                await store.RemoveClientServerAsync(
                    new ZLinkClientServerServerDescriptorKey(
                        actual.ChannelName,
                        actual.ServerRid),
                    serverLocations.OwnerToken));
            var forged = actual with
            {
                LifecycleGeneration = actual.LifecycleGeneration + 1,
                DescriptorRevision = 1,
                SecurityIdentity = "forged-security"
            };
            Assert.Equal(
                ZLinkLocationWriteStatus.Stored,
                (await store.UpdateClientServerAsync(
                    forged,
                    ZLinkLocationWriteIntent.NewClaim)).Status);

            await clientDiscovery.StartAsync(
                await clientRuntime.EnsureStartedStateAsync(
                    CancellationToken.None));
            var transport =
                clientRuntime.GetClientServerClientRuntime("work");
            await WaitUntilAsync(
                () => transport.AdmissionCompletedCount == 1,
                TimeSpan.FromSeconds(5));
            Assert.Equal(0, transport.ReadyCount);
        }
        finally
        {
            await clientDiscovery.StopAsync();
            await serverDiscovery.StopAsync();
            await clientRuntime.StopAsync(CancellationToken.None);
            await serverRuntime.StopAsync(CancellationToken.None);
            await clientLocations.StopAsync();
            await serverLocations.StopAsync();
        }
    }

    [Fact]
    public async Task MalformedPushedControl_ReconnectsAndReadmits()
    {
        var port = ReservePort();
        var endpoint = $"tcp://127.0.0.1:{port}";
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        router.Bind(endpoint);
        await using var client = CreateClient(port);
        var runtime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var transport = runtime.GetClientServerClientRuntime("work");
            using var firstHello = await PollReceivedAsync(
                storage => TryReceive(router, storage),
                TimeSpan.FromSeconds(5));
            Assert.True(
                ZLinkClientServerControlProtocol.TryDecodeHello(
                    firstHello.Parts,
                    out _));
            ReplyAdmission(router, firstHello, endpoint);
            await WaitUntilAsync(
                () => transport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));

            var malformed = Message.From(
                new byte[] { 0x5a, 0x4d, 0x01, 0xff, 0x00 });
            await router.Send(
                    firstHello.RoutingId
                        ?? throw new InvalidOperationException(
                            "missing client routing id"))
                    .Message(malformed)
                    .Async(CancellationToken.None);

            Received secondHello;
            try
            {
                secondHello = await PollReceivedAsync(
                    storage => TryReceive(router, storage),
                    TimeSpan.FromSeconds(5));
            }
            catch (TimeoutException exception)
            {
                throw new TimeoutException(
                    $"{exception.Message} diagnostics={transport.AdmissionDiagnostics}; "
                    + $"physical={transport.PhysicalConnectionCount}; "
                    + $"ready={transport.ReadyCount}",
                    exception);
            }
            using (secondHello)
            {
                Assert.True(
                    ZLinkClientServerControlProtocol.TryDecodeHello(
                        secondHello.Parts,
                        out _));
                ReplyAdmission(router, secondHello, endpoint);
            }
            await WaitUntilAsync(
                () => transport.ReadyCount == 1,
                TimeSpan.FromSeconds(5));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static bool TryReceive(IRouterSocket router, Received storage)
    {
        return router.Recv(storage, RecvFlags.DontWait);
    }

    private static void ReplyAdmission(
        IRouterSocket router,
        Received hello,
        string endpoint)
    {
        var admission = ZLinkClientServerControlProtocol.EncodeAdmission(
            new ZLinkClientServerControlProtocol.Admission(
                "work",
                RoutingId.From("manual-server"),
                1,
                1,
                100,
                ZLinkFrameworkRuntimeState.Serving,
                "plaintext",
                1024 * 1024,
                endpoint));
        router.Reply(
                hello.RoutingId
                ?? throw new InvalidOperationException(
                    "missing client routing id"),
                hello.RequestSeq
                ?? throw new InvalidOperationException(
                    "missing request sequence"))
            .Message(admission)
            .Submit();
    }

    [Fact]
    public async Task MalformedReservedControlFrame_DoesNotReachApplicationHandler()
    {
        var port = ReservePort();
        await using var server = CreateServer(port);
        var runtime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            using var context = Systems.Zlink.Zlink.CreateContext();
            using var dealer = context.CreateDealerSocket();
            dealer.SetRoutingId(RoutingId.From("malformed-client"));
            dealer.Connect($"tcp://127.0.0.1:{port}");
            await Task.Delay(100);
            var malformed = Message.From(
                new byte[] { 0x5a, 0x4d, 0x01, 0xff, 0x00 });
            await dealer.Send()
                .Message(malformed)
                .Async(CancellationToken.None);
            await Task.Delay(250);
            Assert.False(
                server.GetRequiredService<EchoProbe>().Received.Task.IsCompleted);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private static ServiceProvider CreateServer(
        int port,
        long maximumMessageBytes = 16L * 1024L * 1024L)
    {
        var services = new ServiceCollection();
        services.AddSingleton<EchoProbe>();
        services.AddSingleton(new ServerIdentity(string.Empty));
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("work")
                .Server()
                .Listen(port)
                .AddSendHandler<EchoSendHandler, EchoSend>()
                .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
        });
        var provider = services.BuildServiceProvider();
        provider.GetRequiredService<ZLinkFrameworkRegistration>()
            .Channels["work"].Server!.SocketConfig.MaxMessageSize =
            maximumMessageBytes;
        return provider;
    }

    private static ServiceProvider CreateLargeReplyServer(
        int port,
        long maximumMessageBytes)
    {
        var services = new ServiceCollection();
        services.AddSingleton(new ServerIdentity(string.Empty));
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("work")
                .Server()
                .Listen(port)
                .AddRequestHandler<LargeReplyHandler, LargeReplyRequest, LargeReply>();
        });
        var provider = services.BuildServiceProvider();
        provider.GetRequiredService<ZLinkFrameworkRegistration>()
            .Channels["work"].Server!.SocketConfig.MaxMessageSize =
            maximumMessageBytes;
        return provider;
    }

    private static ServiceProvider CreateBlockingServer(int port)
    {
        var services = new ServiceCollection();
        services.AddSingleton<BlockingRequestProbe>();
        services.AddSingleton(new ServerIdentity(string.Empty));
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("work")
                .Server()
                .Listen(port)
                .AddRequestHandler<BlockingRequestHandler, BlockingRequest, EchoReply>();
        });
        return services.BuildServiceProvider();
    }

    private static ServiceProvider CreateClient(
        int port,
        long maximumMessageBytes = 16L * 1024L * 1024L)
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("work")
                .Client()
                .Connect($"tcp://127.0.0.1:{port}");
        });
        var provider = services.BuildServiceProvider();
        provider.GetRequiredService<ZLinkFrameworkRegistration>()
            .Channels["work"].Client!.SocketConfig.MaxMessageSize =
            maximumMessageBytes;
        return provider;
    }

    private static ServiceProvider CreateAutomaticServer(
        IZLinkLocationStore store,
        string name,
        int weight = 100,
        int port = 0)
    {
        var services = new ServiceCollection();
        services.AddSingleton<EchoProbe>();
        services.AddSingleton(new ServerIdentity(name));
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddClientServerChannel("work")
                .Server()
                .Listen(port)
                .SetWeight(weight)
                .AddSendHandler<EchoSendHandler, EchoSend>()
                .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
        });
        return services.BuildServiceProvider();
    }

    private static ServiceProvider CreateAutomaticClient(
        IZLinkLocationStore store)
    {
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddClientServerChannel("work").Client();
        });
        return services.BuildServiceProvider();
    }

    private static ServiceProvider CreateAutomaticClientAndServer(
        IZLinkLocationStore store,
        string name,
        int weight = 100)
    {
        var services = new ServiceCollection();
        services.AddSingleton<EchoProbe>();
        services.AddSingleton(new ServerIdentity(name));
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddClientServerChannel("work").Client();
            options.AddClientServerChannel("work")
                .Server()
                .Listen(0)
                .SetWeight(weight)
                .AddSendHandler<EchoSendHandler, EchoSend>()
                .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
        });
        return services.BuildServiceProvider();
    }

    private static ServiceProvider CreateLocalClientAndServer(int weight = 100)
    {
        var services = new ServiceCollection();
        services.AddSingleton<EchoProbe>();
        services.AddSingleton(new ServerIdentity("local"));
        services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("work").Client();
            options.AddClientServerChannel("work")
                .Server()
                .Listen(0)
                .SetWeight(weight)
                .AddSendHandler<EchoSendHandler, EchoSend>()
                .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
        });
        return services.BuildServiceProvider();
    }

    private static int ReservePort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    private sealed class SingleAccessMessageParts : IReadOnlyList<Message>
    {
        private readonly int[] _accessCounts = new int[2];
        private readonly Message[] _parts =
        [
            Message.From(new byte[] { 1, 2, 3 }),
            Message.From(new byte[] { 4, 5, 6 })
        ];

        public int Count => _parts.Length;

        public Message this[int index]
        {
            get
            {
                if (Interlocked.Increment(ref _accessCounts[index]) != 1)
                    throw new InvalidOperationException(
                        $"Message part {index} was consumed more than once.");
                return _parts[index];
            }
        }

        public void AssertDisposedOnce()
        {
            Assert.All(_accessCounts, count => Assert.Equal(1, count));
            Assert.All(_parts, part =>
                Assert.Throws<ObjectDisposedException>(() => _ = part.Size));
        }

        public void DisposeRemaining()
        {
            foreach (var part in _parts)
            {
                try
                {
                    _ = part.Size;
                    part.Dispose();
                }
                catch (ObjectDisposedException)
                {
                }
            }
        }

        public IEnumerator<Message> GetEnumerator()
        {
            for (var index = 0; index < Count; index++)
                yield return this[index];
        }

        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    }

    private sealed class MetadataFailureSpotActivation : IZLinkCurrentSpotActivation
    {
        public void EnsureOperationAllowed()
        {
        }

        public string ChannelName => "entry";

        public string SpotId => "entry";

        public ZLinkUserSpotExecutionMode ExecutionMode => default;

        public TimeSpan DefaultRequestTimeout => TimeSpan.FromSeconds(1);

        public ZLinkCodecRegistryBuilder Codecs { get; } = new();

        public ZLinkMessageFlowTracer Flow => null!;

        public IZLinkRuntimeFailureReporter ErrorSink => null!;

        public IZLinkSpotOutbound Outbound => null!;

        public ZLinkSpotOutboundEndpoint OutboundEndpoint { get; set; } = null!;
    }

    private sealed record EchoRequest(string Value);

    private sealed record EchoReply(string Value);

    private sealed record EchoSend(string Value);

    private sealed record LargeReplyRequest(string Value);

    private sealed record LargeReply(string Value);

    private sealed record BlockingRequest(string Value);

    private sealed record ServerIdentity(string Name);

    private sealed class EchoProbe
    {
        public TaskCompletionSource<string> Received { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed record FlowEchoRequest(string Value);

    private sealed class FlowProbe
    {
        public TaskCompletionSource<(string? FlowId, ZLinkFlowOrigin? Origin)> Handler { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class FlowEchoHandler(FlowProbe probe)
        : IZLinkRequestHandler<FlowEchoRequest, EchoReply>
    {
        public ValueTask<EchoReply> HandleAsync(
            FlowEchoRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var flow = ZLinkFlowContext.Current;
            probe.Handler.TrySetResult((flow?.FlowId, flow?.Origin));
            return ValueTask.FromResult(new EchoReply(request.Value));
        }
    }

    private sealed class BlockingRequestProbe
    {
        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Completed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }

    private sealed class BlockingRequestHandler(BlockingRequestProbe probe)
        : IZLinkRequestHandler<BlockingRequest, EchoReply>
    {
        public async ValueTask<EchoReply> HandleAsync(
            BlockingRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            Assert.Equal("work", context.ChannelName);
            probe.Entered.TrySetResult();
            await probe.Release.Task;
            try
            {
                return new EchoReply(request.Value);
            }
            finally
            {
                probe.Completed.TrySetResult();
            }
        }
    }

    private sealed class EchoSendHandler(EchoProbe probe) : IZLinkSendHandler<EchoSend>
    {
        public ValueTask HandleAsync(
            EchoSend message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal("work", context.ChannelName);
            probe.Received.TrySetResult(message.Value);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class EchoHandler(ServerIdentity identity)
        : IZLinkRequestHandler<EchoRequest, EchoReply>
    {
        public ValueTask<EchoReply> HandleAsync(
            EchoRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal("work", context.ChannelName);
            return ValueTask.FromResult(new EchoReply(
                string.IsNullOrEmpty(identity.Name)
                    ? request.Value
                    : $"{identity.Name}:{request.Value}"));
        }
    }

    private sealed class LargeReplyHandler
        : IZLinkRequestHandler<LargeReplyRequest, LargeReply>
    {
        public ValueTask<LargeReply> HandleAsync(
            LargeReplyRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Assert.Equal("work", context.ChannelName);
            return ValueTask.FromResult(new LargeReply(
                request.Value + new string('y', 2048)));
        }
    }
}
