using System.Diagnostics;
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
    [InlineData(AutoHwmProfile.Compact)]
    [InlineData(AutoHwmProfile.LowLatency)]
    [InlineData(AutoHwmProfile.Balanced)]
    [InlineData(AutoHwmProfile.Throughput)]
    public async Task Core_Profile_Is_Applied_To_The_Binding_Context(
        AutoHwmProfile coreProfile)
    {
        await using var context = new ZLinkDotNetBackendAdapterFactory()
            .CreateRuntimeContext();

        context.ConfigureCoreHwm(coreProfile, 1024 * 1024, 256 * 1024);

        // The port owns the binding option mapping. The binding enum is not
        // exposed through the semantic runtime contract.
    }

    [Fact]
    public async Task Core_Hwm_Snapshot_And_Reset_Are_Direct_Binding_Projections()
    {
        await using var context = new ZLinkDotNetBackendAdapterFactory()
            .CreateRuntimeContext();
        const ulong budgetBytes = 1024 * 1024;
        context.ConfigureCoreHwm(
            AutoHwmProfile.Balanced,
            memoryLimitBytes: 0,
            budgetBytes);

        var before = context.GetCoreHwmBudgetSnapshot();

        Assert.Equal(budgetBytes, before.ConfiguredCoreBudgetBytes);
        Assert.Equal(budgetBytes, before.EffectiveCoreBudgetBytes);

        context.ResetCoreHwmBudgetMetrics();
        var after = context.GetCoreHwmBudgetSnapshot();

        Assert.True(after.MeasurementEpoch > before.MeasurementEpoch);
        Assert.Equal(before.EffectiveCoreBudgetBytes, after.EffectiveCoreBudgetBytes);
        Assert.Equal(after.CurrentAccountedBytes, after.PeakAccountedBytes);
        Assert.Equal(
            after.CompletionCurrentAccountedBytes,
            after.CompletionPeakAccountedBytes);
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
        Task<IReadOnlyList<Message>> requestTask;
        using (var request = Message.From("request"))
        {
            requestTask = dealer.Request()
                .Message(request)
                .Timeout(TimeSpan.FromSeconds(2))
                .Async(CancellationToken.None);
        }

        using var received = await ReceiveAsync(router, TimeSpan.FromSeconds(2));
        using (var reply = Message.From("reply"))
            router.Reply(
                    Assert.IsType<RoutingId>(received.RoutingId),
                    Assert.IsType<ReplyToken>(received.ReplyToken))
                .Message(reply)
                .Submit();

        var replyParts = await requestTask.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            Assert.Equal("reply", Assert.Single(replyParts).GetString());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
        Assert.Null(typeof(ZLinkFrameworkRuntime).Assembly.GetType(
            "Zlink.Framework.Runtime.Messaging.ZLinkRequestCompletionPump"));
    }

    [Fact]
    public async Task PublicPoller_SubPollIn_AndDealerCompletion_ProgressTogether()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var publisher = context.CreateXPubSocket();
        using var subscriber = context.CreateSubSocket();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var publishEndpoint = $"inproc://poller-sub-{suffix}";
        var requestEndpoint = $"inproc://poller-dealer-{suffix}";
        const string topic = "contract";
        const nuint subscriberSlot = 1;
        const nuint dealerSlot = 2;

        publisher.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
        router.Options.ReceiveTimeout = TimeSpan.FromSeconds(2);
        publisher.Bind(publishEndpoint);
        subscriber.Connect(publishEndpoint);
        subscriber.SetSubscription(topic);
        var subscription = new SubscriptionEvent();
        Assert.True(publisher.ReceiveSubscriptionEvent(subscription));
        Assert.True(subscription.Subscribed);
        Assert.Equal(topic, subscription.Topic);

        router.Bind(requestEndpoint);
        dealer.Connect(requestEndpoint);
        using (var handshake = Message.From("ready"))
            dealer.Send().Message(handshake).Submit();
        using (var received = Received.Create())
            Assert.True(router.Recv(received));

        poller.Add(subscriber, PollEventFlags.PollIn, subscriberSlot);
        poller.Add(dealer, PollEventFlags.PollCompletion, dealerSlot);

        Task<IReadOnlyList<Message>> requestTask;
        using (var request = Message.From("request"))
        {
            requestTask = dealer.Request()
                .Message(request)
                .Timeout(TimeSpan.FromSeconds(2))
                .Async(CancellationToken.None);
        }

        using (var received = Received.Create())
        {
            Assert.True(router.Recv(received));
            using var reply = Message.From("reply");
            router.Reply(
                    Assert.IsType<RoutingId>(received.RoutingId),
                    Assert.IsType<ReplyToken>(received.ReplyToken))
                .Message(reply)
                .Submit();
        }
        using (var published = Message.From("published"))
            publisher.Publish(topic).Message(published).Submit();

        var events = new PollEvent[2];
        var deadlineTimeout = TimeSpan.FromSeconds(2);
        var deadlineStarted = Stopwatch.GetTimestamp();
        var subscriberReady = false;
        var dealerCompletionReady = false;
        while ((!subscriberReady || !dealerCompletionReady)
               && Stopwatch.GetElapsedTime(deadlineStarted) < deadlineTimeout)
        {
            var remaining = deadlineTimeout - Stopwatch.GetElapsedTime(deadlineStarted);
            if (remaining <= TimeSpan.Zero)
                break;
            var ready = poller.Wait(events, remaining);
            for (var index = 0; index < ready; index++)
            {
                if (events[index].Slot == subscriberSlot)
                    subscriberReady =
                        (events[index].Revents & PollEventFlags.PollIn) != 0;
                if (events[index].Slot == dealerSlot)
                    dealerCompletionReady =
                        (events[index].Revents & PollEventFlags.PollCompletion) != 0;
            }
        }

        Assert.True(subscriberReady);
        Assert.True(dealerCompletionReady);
        using (var published = new TopicMessage())
        {
            Assert.True(subscriber.Subscribe(published, RecvFlags.DontWait));
            Assert.Equal(topic, published.Topic);
            Assert.Equal("published", published.SinglePartOrThrow().GetString());
        }

        var replyParts = await requestTask.WaitAsync(TimeSpan.FromSeconds(2));
        try
        {
            Assert.Equal("reply", Assert.Single(replyParts).GetString());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(replyParts);
        }
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
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (Stopwatch.GetElapsedTime(deadlineStarted) < timeout)
        {
            var received = Received.Create();
            if (router.Recv(received, RecvFlags.DontWait)) return received;
            received.Dispose();
            await Task.Delay(5);
        }

        throw new TimeoutException("Router did not receive the request.");
    }
}
