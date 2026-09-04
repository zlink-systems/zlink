using System.Runtime.CompilerServices;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_completion_lifecycle_regressions
{
    [Fact]
    public void poller_accepts_sub_pollin_without_completion_support()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var subscriber = context.CreateSubSocket();
        using var poller = Zlink.CreatePoller();

        poller.Add(subscriber, PollEventFlags.PollIn, 7);

        Assert.Equal(1, poller.Size);
        Assert.True(poller.Remove(subscriber));
        Assert.Equal(0, poller.Size);
    }

    [Fact]
    public async Task duplicate_add_keeps_existing_completion_ownership()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "duplicate-completion-owner");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        using (Message handshake = Message.From("ready"))
            dealer.Send().Message(handshake).Submit();
        using (Received received = Receive(router))
            Assert.Equal("ready", received.SinglePartOrThrow().GetString());

        poller.Add(dealer, PollEventFlags.PollCompletion, 83);
        Assert.Throws<ZlinkConfigException>(() =>
            poller.Add(dealer, PollEventFlags.PollCompletion, 84));

        using Message request = Message.From("request");
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(request).Timeout(TimeSpan.FromSeconds(2)).Async();
        using (Received received = Receive(router))
        using (Message reply = Message.From("reply"))
            router.Reply(received.RoutingId!.Value, received.ReplyToken!)
                .Message(reply).Submit();

        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(2)));
        Assert.Equal((nuint)83, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollCompletion);

        IReadOnlyList<Message> parts = await pending;
        Assert.Equal("reply", Assert.Single(parts).GetString());
        Zlink.MultipartClose(parts);
    }

    [Fact]
    public void closed_registered_socket_can_be_removed()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var socket = context.CreatePairSocket();
        using var poller = Zlink.CreatePoller();
        poller.Add(socket, PollEventFlags.PollIn, 31);

        socket.Close();

        Assert.True(poller.Remove(socket));
        Assert.Equal(0, poller.Size);
    }

    [Fact]
    public void socket_keeps_its_context_alive()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        (IPairSocket socket, WeakReference<IContext> weakContext) =
            CreateSocketWithoutContextRoot();
        ForceFinalizers();

        Assert.True(weakContext.TryGetTarget(out IContext? context));
        try
        {
            using IPairSocket sibling = context!.CreatePairSocket();
        }
        finally
        {
            socket.Dispose();
            context?.Dispose();
        }
    }

    [Fact]
    public async Task abandoned_socket_settles_pending_request()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "abandoned-request");
        router.Bind(endpoint);

        (Task<IReadOnlyList<Message>> pending,
            WeakReference<IDealerSocket> weakDealer) =
            CreatePendingRequestWithoutSocketRoot(context, endpoint);
        using (Received received = Receive(router))
            Assert.Equal("ready", received.SinglePartOrThrow().GetString());

        ForceFinalizers();

        Assert.False(weakDealer.TryGetTarget(out _));
        ZlinkRequestException error = await Assert.ThrowsAsync<ZlinkRequestException>(
            () => pending.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(ZlinkRequestException.ErrorCode.Terminated, error.Result);
    }

    [Fact]
    public async Task context_shutdown_settles_runtime_owned_request_as_terminated()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "runtime-context-shutdown");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        EstablishConnection(dealer, router);

        using Message request = Message.From("pending");
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(request).Timeout(TimeSpan.FromSeconds(30)).Async();

        context.Shutdown();

        ZlinkRequestException error = await Assert.ThrowsAsync<ZlinkRequestException>(
            () => pending.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(ZlinkRequestException.ErrorCode.Terminated, error.Result);
    }

    [Fact]
    public async Task context_shutdown_settles_public_owned_request_as_terminated()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "public-context-shutdown");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        EstablishConnection(dealer, router);
        poller.Add(dealer, PollEventFlags.PollCompletion, 91);

        using Message request = Message.From("pending");
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(request).Timeout(TimeSpan.FromSeconds(30)).Async();

        context.Shutdown();
        var events = new PollEvent[1];
        try
        {
            _ = poller.Wait(events, TimeSpan.FromSeconds(2));
        }
        catch (ZlinkException)
        {
            // A terminated completion queue may surface through PollErr or as
            // the typed receive error; either path must settle the operation.
        }

        ZlinkRequestException error = await Assert.ThrowsAsync<ZlinkRequestException>(
            () => pending.WaitAsync(TimeSpan.FromSeconds(2)));
        Assert.Equal(ZlinkRequestException.ErrorCode.Terminated, error.Result);
    }

    private static void EstablishConnection(IDealerSocket dealer,
        IRouterSocket router)
    {
        using Message handshake = Message.From("ready");
        dealer.Send().Message(handshake).Submit();
        using Received received = Receive(router);
        Assert.Equal("ready", received.SinglePartOrThrow().GetString());
    }

    private static Received Receive(IReceivingMessageSocket socket)
    {
        var received = Received.Create();
        if (socket.Recv(received))
            return received;
        received.Dispose();
        throw new InvalidOperationException("blocking receive returned no data");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (IPairSocket Socket, WeakReference<IContext> Context)
        CreateSocketWithoutContextRoot()
    {
        IContext context = Zlink.CreateContext();
        IPairSocket socket = context.CreatePairSocket();
        var weakContext = new WeakReference<IContext>(context);
        GC.KeepAlive(context);
        return (socket, weakContext);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (Task<IReadOnlyList<Message>> Pending,
        WeakReference<IDealerSocket> Dealer)
        CreatePendingRequestWithoutSocketRoot(IContext context,
            string endpoint)
    {
        IDealerSocket dealer = context.CreateDealerSocket();
        dealer.Connect(endpoint);
        using (Message handshake = Message.From("ready"))
            dealer.Send().Message(handshake).Submit();
        using Message request = Message.From("pending");
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(request).Timeout(TimeSpan.FromSeconds(30)).Async();
        var weakDealer = new WeakReference<IDealerSocket>(dealer);
        GC.KeepAlive(dealer);
        return (pending, weakDealer);
    }

    private static void ForceFinalizers()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }
}
