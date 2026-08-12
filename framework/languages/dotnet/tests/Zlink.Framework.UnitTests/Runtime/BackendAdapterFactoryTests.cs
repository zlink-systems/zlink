using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed class BackendAdapterFactoryTests
{
    [Fact]
    public async Task BackendFactory_Returns_Binding_Sockets_Through_Runtime_Context()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        await using var context = factory.CreateRuntimeContext();
        await using var dealer = context.CreateDealerSocket();
        await using var router = context.CreateRouterSocket();
        await using var publisher = context.CreatePublisherSocket();
        await using var subscriber = context.CreateSubscriberSocket();

        Assert.IsAssignableFrom<IZLinkBackendRuntimeContext>(context);
        Assert.IsAssignableFrom<IDealerSocket>(dealer);
        Assert.IsAssignableFrom<IRouterSocket>(router);
        Assert.IsAssignableFrom<IPubSocket>(publisher);
        Assert.IsAssignableFrom<ISubSocket>(subscriber);
        await using var monitor = factory.CreateMonitoringAdapter().OpenSocketMonitor(dealer);
        await AssertSpotBackendAsync(context);
        await AssertStreamBackendAsync(context);
    }

    [Fact]
    public void BackendAssembly_DoesNotRetain_PassThroughSocketPortsOrWrappers()
    {
        var assembly = typeof(ZLinkFrameworkRuntime).Assembly;
        string[] removedTypes =
        [
            "Zlink.Framework.Runtime.Backend.Contracts.IZLinkBackendDealerSocket",
            "Zlink.Framework.Runtime.Backend.Contracts.IZLinkBackendRouterSocket",
            "Zlink.Framework.Runtime.Backend.Contracts.IZLinkBackendPublisherSocket",
            "Zlink.Framework.Runtime.Backend.Contracts.IZLinkBackendSubscriberSocket",
            "Zlink.Framework.Runtime.Backend.DotNet.Wrappers.ZLinkBackendDealerSocketWrapper",
            "Zlink.Framework.Runtime.Backend.DotNet.Wrappers.ZLinkBackendRouterSocketWrapper",
            "Zlink.Framework.Runtime.Backend.DotNet.Wrappers.ZLinkBackendPublisherSocketWrapper",
            "Zlink.Framework.Runtime.Backend.DotNet.Wrappers.ZLinkBackendSubscriberSocketWrapper"
        ];

        Assert.All(removedTypes, typeName => Assert.Null(assembly.GetType(typeName)));
    }

    [Theory]
    [InlineData(ZLinkApplicationHwmProfile.Compact)]
    [InlineData(ZLinkApplicationHwmProfile.LowLatency)]
    [InlineData(ZLinkApplicationHwmProfile.Balanced)]
    [InlineData(ZLinkApplicationHwmProfile.Throughput)]
    public async Task Framework_Profile_Is_Applied_To_The_Binding_Context(
        ZLinkApplicationHwmProfile frameworkProfile)
    {
        await using var context = new ZLinkDotNetBackendAdapterFactory()
            .CreateRuntimeContext();

        context.ConfigureAutoHwm(frameworkProfile);

        // The port owns the binding option mapping. The binding enum is not
        // exposed through the semantic runtime contract.
    }

    [Fact]
    public async Task Dealer_Request_Completes_Through_Binding_Progress_Without_Framework_Poll_Worker()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        await using var context = factory.CreateRuntimeContext();
        await using var dealer = context.CreateDealerSocket();
        await using var router = context.CreateRouterSocket();
        var endpoint = $"inproc://binding-progress-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        var completion = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);

        using (var request = Message.From("request"))
        {
            Assert.True(dealer.Request()
                .Message(request)
                .Timeout(TimeSpan.FromSeconds(2))
                .Submit(
                    (result, parts) =>
                    {
                        try
                        {
                            if (result != RequestResult.Ok)
                            {
                                completion.TrySetException(new InvalidOperationException($"Request failed: {result}."));
                                return;
                            }

                            completion.TrySetResult(Assert.Single(parts).GetString());
                        }
                        finally
                        {
                            ZLinkMessageParts.DisposeAll(parts);
                        }
                    }));
        }

        using var received = await ReceiveAsync(router, TimeSpan.FromSeconds(2));
        using (var reply = Message.From("reply"))
            router.Reply(
                    Assert.IsType<RoutingId>(received.RoutingId),
                    Assert.IsType<ulong>(received.RequestSeq))
                .Message(reply)
                .Submit();

        Assert.Equal("reply", await completion.Task.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Null(typeof(ZLinkFrameworkRuntime).Assembly.GetType(
            "Zlink.Framework.Runtime.Messaging.ZLinkRequestCompletionPump"));
    }

    private static async Task AssertSpotBackendAsync(
        IZLinkBackendRuntimeContext context)
    {
        await using var spotNode = context.CreateSpotNode("test-mesh");

        Assert.IsType<ZLinkBackendSpotNodeWrapper>(spotNode);
    }

    [Fact]
    public async Task SpotNode_Router_Send_Config_RoundTrips_Through_Binding()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        await using var context = factory.CreateRuntimeContext();
        await using var backend = context.CreateSpotNode("router-config-mesh");
        var spotNode = Assert.IsType<ZLinkBackendSpotNodeWrapper>(backend);

        var byteHwm = (ulong)int.MaxValue + 1UL;
        spotNode.SetRouterHighWaterMark(byteHwm);
        spotNode.SetRouterReceiveHighWaterMark(byteHwm - 1);
        spotNode.SetRouterReceiveTimeout(TimeSpan.FromMilliseconds(29));
        spotNode.SetRouterSendTimeout(TimeSpan.FromMilliseconds(37));

        Assert.Equal(byteHwm, spotNode.NativeNode.RouterHighWaterMark);
        Assert.Equal(byteHwm - 1, spotNode.NativeNode.RouterReceiveHighWaterMark);
        Assert.Equal(TimeSpan.FromMilliseconds(29), spotNode.NativeNode.ReceiveTimeout);
        Assert.Equal(TimeSpan.FromMilliseconds(37), spotNode.NativeNode.SendTimeout);
    }

    private static async Task AssertStreamBackendAsync(
        IZLinkBackendRuntimeContext context)
    {
        await using var streamSocket = context.CreateStreamSocket("test-mesh");

        var wrapper = Assert.IsType<ZLinkBackendStreamSocketWrapper>(streamSocket);
        var endpoint = $"inproc://stream-backend-{Guid.NewGuid():N}";
        wrapper.Bind(endpoint);
        Assert.Equal(endpoint, wrapper.GetLastEndpoint());
    }

    [Fact]
    public void BackendFactory_Creates_MonitoringAdapter()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();

        Assert.NotNull(factory.CreateMonitoringAdapter());
    }

    [Fact]
    public async Task Dealer_PeerWeight_RoundTrips_Through_The_Binding_Option()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        await using var context = factory.CreateRuntimeContext();
        await using var dealer = context.CreateDealerSocket();

        Assert.Equal(ZLinkSocketConfig.DefaultPeerWeight, dealer.Options.PeerWeight);
        dealer.Options.PeerWeight = 0;
        Assert.Equal(0, dealer.Options.PeerWeight);
        dealer.Options.PeerWeight = ZLinkSocketConfig.DefaultPeerWeight;
        Assert.Equal(ZLinkSocketConfig.DefaultPeerWeight, dealer.Options.PeerWeight);
    }

    [Fact]
    public async Task SpotNode_EntrySpot_IsSingleton_UnderConcurrentFirstAccess()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        await using var context = factory.CreateRuntimeContext();
        await using var spotNode = context.CreateSpotNode("test-mesh");

        // Core's zlink_mesh_node_start requires routing-id + bind + channel before the
        // node leaves the CREATED state (matches ZLinkSpotNodeInitializer's non-lazy
        // startup); configure them so the wrapper's lazy EnsureStarted can succeed.
        spotNode.SetRoutingId(RoutingId.From("entry-singleton-node"));
        spotNode.SetRouterBind("inproc://entry-singleton-node");
        spotNode.AddChannel("test-mesh");

        var accesses = Enumerable.Range(0, 32)
            .Select(_ => Task.Run(spotNode.EntrySpot))
            .ToArray();
        var entrySpots = await Task.WhenAll(accesses);

        Assert.All(entrySpots, entrySpot => Assert.Same(entrySpots[0], entrySpot));
    }

    private static async Task<Received> ReceiveAsync(
        IRouterSocket router,
        TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            var received = Received.Create();
            if (router.Recv(received, RecvFlags.DontWait)) return received;
            received.Dispose();
            await Task.Delay(5);
        }

        throw new TimeoutException("Router did not receive the request.");
    }
}
