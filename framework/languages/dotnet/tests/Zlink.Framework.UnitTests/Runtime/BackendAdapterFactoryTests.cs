using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests;

public sealed class BackendAdapterFactoryTests
{
    [Fact]
    public async Task BackendFactory_Creates_Channel_Spot_And_Stream_Wrappers()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        var channelAdapter = factory.CreateChannelAdapter();
        var spotAdapter = factory.CreateSpotAdapter();
        var streamAdapter = factory.CreateStreamAdapter();

        await using var context = channelAdapter.CreateContext();
        await using var dealer = channelAdapter.CreateDealerSocket(context);
        await using var router = channelAdapter.CreateRouterSocket(context);
        await using var publisher = channelAdapter.CreatePublisherSocket(context);
        await using var subscriber = channelAdapter.CreateSubscriberSocket(context);

        Assert.IsType<ZLinkBackendContextWrapper>(context);
        Assert.IsType<ZLinkBackendDealerSocketWrapper>(dealer);
        Assert.IsType<ZLinkBackendRouterSocketWrapper>(router);
        Assert.IsType<ZLinkBackendPublisherSocketWrapper>(publisher);
        Assert.IsType<ZLinkBackendSubscriberSocketWrapper>(subscriber);
        await using var monitor = factory.CreateMonitoringAdapter().OpenSocketMonitor(dealer);
        await AssertSpotBackendAsync(channelAdapter, spotAdapter);
        await AssertStreamBackendAsync(channelAdapter, streamAdapter);
    }

    [Theory]
    [InlineData(ZLinkApplicationHwmProfile.Compact, AutoHwmProfile.Compact)]
    [InlineData(ZLinkApplicationHwmProfile.LowLatency, AutoHwmProfile.LowLatency)]
    [InlineData(ZLinkApplicationHwmProfile.Balanced, AutoHwmProfile.Balanced)]
    [InlineData(ZLinkApplicationHwmProfile.Throughput, AutoHwmProfile.Throughput)]
    public async Task Framework_Profile_Is_Applied_To_The_Binding_Context(
        ZLinkApplicationHwmProfile frameworkProfile,
        AutoHwmProfile bindingProfile)
    {
        var adapter = new ZLinkDotNetBackendAdapterFactory().CreateChannelAdapter();
        await using var context = adapter.CreateContext();

        adapter.ConfigureAutoHwm(context, frameworkProfile);

        var native = Assert.IsType<ZLinkBackendContextWrapper>(context).NativeContext;
        Assert.True(native.Options.AutoHwmEnabled);
        Assert.Equal(bindingProfile, native.Options.AutoHwmProfile);
    }

    [Fact]
    public async Task Dealer_Request_Completes_Through_Binding_Progress_Without_Framework_Poll_Worker()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        var channelAdapter = factory.CreateChannelAdapter();
        await using var context = channelAdapter.CreateContext();
        await using var dealer = channelAdapter.CreateDealerSocket(context);
        await using var router = channelAdapter.CreateRouterSocket(context);
        var endpoint = $"inproc://binding-progress-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        var completion = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);

        using (var request = Message.From("request"))
        {
            Assert.True(dealer.Request(
                request,
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
                },
                SendFlags.None,
                TimeSpan.FromSeconds(2)));
        }

        using var received = await ReceiveAsync(router, TimeSpan.FromSeconds(2));
        using (var reply = Message.From("reply"))
            router.Reply(
                Assert.IsType<RoutingId>(received.RoutingId),
                Assert.IsType<ulong>(received.RequestSeq),
                reply);

        Assert.Equal("reply", await completion.Task.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Null(typeof(ZLinkFrameworkRuntime).Assembly.GetType(
            "Zlink.Framework.Runtime.Messaging.ZLinkRequestCompletionPump"));
    }

    private static async Task AssertSpotBackendAsync(
        IZLinkChannelBackendAdapter channelAdapter,
        IZLinkSpotBackendAdapter spotAdapter)
    {
        await using var context = channelAdapter.CreateContext();
        await using var spotNode = spotAdapter.CreateSpotNode(context, "test-mesh");

        Assert.IsType<ZLinkBackendSpotNodeWrapper>(spotNode);
    }

    [Fact]
    public async Task SpotNode_Router_Send_Config_RoundTrips_Through_Binding()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        var channelAdapter = factory.CreateChannelAdapter();
        var spotAdapter = factory.CreateSpotAdapter();
        await using var context = channelAdapter.CreateContext();
        await using var backend = spotAdapter.CreateSpotNode(context, "router-config-mesh");
        var spotNode = Assert.IsType<ZLinkBackendSpotNodeWrapper>(backend);

        var byteHwm = (ulong)int.MaxValue + 1UL;
        spotNode.SetRouterHighWaterMark(byteHwm);
        spotNode.SetRouterSendTimeout(TimeSpan.FromMilliseconds(37));

        Assert.Equal(byteHwm, spotNode.NativeNode.RouterHighWaterMark);
        Assert.Equal(TimeSpan.FromMilliseconds(37), spotNode.NativeNode.SendTimeout);
    }

    private static async Task AssertStreamBackendAsync(
        IZLinkChannelBackendAdapter channelAdapter,
        IZLinkStreamBackendAdapter streamAdapter)
    {
        await using var context = channelAdapter.CreateContext();
        await using var streamSocket = streamAdapter.CreateStreamSocket(context, "test-mesh");

        Assert.IsType<ZLinkBackendStreamSocketWrapper>(streamSocket);
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
        var channelAdapter = factory.CreateChannelAdapter();
        await using var context = channelAdapter.CreateContext();
        await using var dealer = channelAdapter.CreateDealerSocket(context);

        Assert.Equal(ZLinkSocketConfig.DefaultPeerWeight, dealer.GetPeerWeight());
        dealer.SetPeerWeight(0);
        Assert.Equal(0, dealer.GetPeerWeight());
        dealer.SetPeerWeight(ZLinkSocketConfig.DefaultPeerWeight);
        Assert.Equal(ZLinkSocketConfig.DefaultPeerWeight, dealer.GetPeerWeight());
    }

    [Fact]
    public async Task SpotNode_EntrySpot_IsSingleton_UnderConcurrentFirstAccess()
    {
        var factory = new ZLinkDotNetBackendAdapterFactory();
        var channelAdapter = factory.CreateChannelAdapter();
        var spotAdapter = factory.CreateSpotAdapter();
        await using var context = channelAdapter.CreateContext();
        await using var spotNode = spotAdapter.CreateSpotNode(context, "test-mesh");

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
        IZLinkBackendRouterSocket router,
        TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            var received = router.Recv(RecvFlags.DontWait);
            if (received is not null) return received;
            await Task.Delay(5);
        }

        throw new TimeoutException("Router did not receive the request.");
    }
}
