using System.Collections;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_blocking_submit_concurrency
{
    private static readonly TimeSpan Watchdog = TimeSpan.FromSeconds(3);

    [Theory]
    [InlineData("Request", false)]
    [InlineData("Request", true)]
    [InlineData("Send", false)]
    [InlineData("Send", true)]
    public async Task blocking_admission_does_not_serialize_other_target(
        string blockingOperation, bool competingReply)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        using var blockedPeer = context.CreateRouterSocket();
        using var readyPeer = context.CreateRouterSocket();
        RoutingId socketRid = CoreTestSupport.RoutingIdUtf8("sender");
        RoutingId blockedRid = CoreTestSupport.RoutingIdUtf8("blocked");
        RoutingId readyRid = CoreTestSupport.RoutingIdUtf8("ready");
        socket.SetRoutingId(socketRid);
        Connect(socket, blockedPeer, blockedRid);
        Connect(socket, readyPeer, readyRid);

        using var readyInbound = Received.Create();
        Task<IReadOnlyList<Message>>? readyOrigin = null;
        if (competingReply)
            readyOrigin = BeginInbound(readyPeer, socket, socketRid,
                readyInbound);

        using var monitor = socket.MonitorOpen(SocketEvent.SendFlowPaused);
        blockedPeer.SetReceiveFlowState(ReceiveFlowState.Paused);
        using (var flow = Zlink.CreatePoller())
        {
            flow.Add(monitor, PollEventFlags.PollIn, 1);
            Assert.Equal(1, flow.Wait(new PollEvent[1], Watchdog));
            Assert.Equal(MonitorEventType.SendFlowPaused,
                monitor.Recv(RecvFlags.DontWait)!.Event);
        }

        using Message blockedPart = Message.From("blocked-operation");
        using var parts = new SubmitBarrierParts(blockedPart);
        object owner = CompletionOwnerTestAccess.Owner(socket);
        Task blocked = Start(() =>
        {
            object? result = blockingOperation switch
            {
                "Request" => CompletionOwnerTestAccess.Invoke(owner, "Request",
                    blockedRid, parts, 3000u),
                "Send" => CompletionOwnerTestAccess.Invoke(owner, "Send",
                    blockedRid, parts),
                _ => throw new ArgumentOutOfRangeException(nameof(blockingOperation))
            };
            if (result is IReadOnlyList<Message> reply)
                Zlink.MultipartClose(reply);
        });
        Task? competing = null;
        try
        {
            // This barrier is inside staging after registration. In the old
            // implementation it is reached with the socket-wide submit lock held.
            Assert.True(parts.StagingEntered.Wait(Watchdog));
            competing = Start(() =>
            {
                using Message part = Message.From("independent-operation");
                if (competingReply)
                    socket.Reply(readyRid, readyInbound.ReplyToken!)
                        .Message(part).Submit();
                else
                    Zlink.MultipartClose(socket.Request(readyRid).Message(part)
                        .Timeout(Watchdog).Submit());
            });
            if (!competingReply)
            {
                using Received request = Receive(readyPeer);
                Assert.Equal("independent-operation",
                    request.SinglePartOrThrow().GetString());
                using Message response = Message.From("independent-reply");
                request.Reply().Message(response).Submit();
            }
            await competing.WaitAsync(Watchdog);
            Assert.False(blocked.IsCompleted,
                "Another target must finish while the first admission is blocked.");
            if (readyOrigin is not null)
            {
                IReadOnlyList<Message> reply = await readyOrigin.WaitAsync(Watchdog);
                Assert.Equal("independent-operation", Assert.Single(reply).GetString());
                Zlink.MultipartClose(reply);
            }
        }
        finally
        {
            context.Shutdown();
            // Observe all terminals even when an assertion fails, then dispose
            // sockets only after native submissions have returned.
            _ = await Record.ExceptionAsync(() => blocked.WaitAsync(Watchdog));
            if (competing is not null)
                _ = await Record.ExceptionAsync(() => competing.WaitAsync(Watchdog));
            if (readyOrigin is not null)
                _ = await Record.ExceptionAsync(() => readyOrigin.WaitAsync(Watchdog));
        }
    }

    [Fact]
    public async Task prepublication_reply_joins_blocking_request_once()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        using var completions = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "request-publication");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        completions.Add(dealer, PollEventFlags.PollCompletion, 9);
        using Message requestPart = Message.From("before-publication");
        using var parts = new SubmitBarrierParts(requestPart, holdPublication: true);
        object owner = CompletionOwnerTestAccess.Owner(dealer);
        IReadOnlyList<Message>? reply = null;
        Task request = Start(() => reply = (IReadOnlyList<Message>)
            CompletionOwnerTestAccess.Invoke(owner, "Request", null, parts, 3000u)!);
        Task? drain = null;
        try
        {
            Assert.True(parts.Admitted.Wait(Watchdog));
            using Received received = Receive(router);
            using Message response = Message.From("early-reply");
            received.Reply().Message(response).Submit();

            object submitSync = CompletionOwnerTestAccess.Field(owner, "_submitSync");
            Assert.True(Monitor.TryEnter(submitSync),
                "Native submit and caller-message consumption must leave the owner lock free.");
            Monitor.Exit(submitSync);
            drain = Start(() =>
            {
                var events = new PollEvent[1];
                Assert.Equal(1, completions.Wait(events, Watchdog));
                Assert.NotEqual(PollEventFlags.None,
                    events[0].Revents & PollEventFlags.PollCompletion);
            });
            Assert.True(SpinWait.SpinUntil(() =>
            {
                if (!Monitor.TryEnter(submitSync))
                    return true;
                Monitor.Exit(submitSync);
                return drain.IsCompleted;
            }, Watchdog));
            Assert.False(drain.IsCompleted);
            Assert.False(request.IsCompleted);
            parts.ReleasePublication.Set();
            await Task.WhenAll(request, drain).WaitAsync(Watchdog);
            Assert.Equal("early-reply", Assert.Single(reply!).GetString());
            Assert.Empty(CompletionOwnerTestAccess.Entries(owner));
            Assert.Equal(0, completions.Wait(new PollEvent[1], TimeSpan.Zero));
        }
        finally
        {
            parts.ReleasePublication.Set();
            _ = await Record.ExceptionAsync(() => request.WaitAsync(Watchdog));
            if (drain is not null)
                _ = await Record.ExceptionAsync(() => drain.WaitAsync(Watchdog));
            if (reply is not null)
                Zlink.MultipartClose(reply);
        }
    }

    private static Task Start(Action action) => Task.Factory.StartNew(action,
        CancellationToken.None, TaskCreationOptions.LongRunning,
        TaskScheduler.Default);

    private static void Connect(IRouterSocket socket, IRouterSocket peer, RoutingId rid)
    {
        peer.SetRoutingId(rid);
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "parallel-submit");
        peer.Bind(endpoint);
        socket.Options.SetConnectRoutingId(rid);
        socket.Connect(endpoint);
        using Message handshake = Message.From("ready");
        socket.Send(rid).Message(handshake).Submit();
        using Received received = Receive(peer);
        Assert.Equal("ready", received.SinglePartOrThrow().GetString());
    }

    private static Task<IReadOnlyList<Message>> BeginInbound(IRouterSocket peer,
        IRouterSocket socket, RoutingId socketRid, Received received)
    {
        using Message part = Message.From("reply-token");
        Task<IReadOnlyList<Message>> pending = peer.Request(socketRid).Message(part)
            .Timeout(Watchdog).Async();
        using var poller = Zlink.CreatePoller();
        poller.Add(socket, PollEventFlags.PollIn, 1);
        Assert.Equal(1, poller.Wait(new PollEvent[1], Watchdog));
        Assert.True(socket.Recv(received, RecvFlags.DontWait));
        Assert.Equal(ReceivedMessageType.Request, received.MessageType);
        return pending;
    }

    private static Received Receive(IRouterSocket socket)
    {
        using var poller = Zlink.CreatePoller();
        poller.Add(socket, PollEventFlags.PollIn, 1);
        Assert.Equal(1, poller.Wait(new PollEvent[1], Watchdog));
        var received = Received.Create();
        Assert.True(socket.Recv(received, RecvFlags.DontWait));
        return received;
    }

    // Inject scheduling only at the existing caller-payload boundary. The
    // production submission still executes the real Core API with flags NONE.
    private sealed class SubmitBarrierParts(Message part, bool holdPublication = false)
        : IReadOnlyList<Message>, IDisposable
    {
        private int _reads;
        internal readonly ManualResetEventSlim StagingEntered = new();
        internal readonly ManualResetEventSlim Admitted = new();
        internal readonly ManualResetEventSlim ReleasePublication = new();
        public int Count => 1;
        public Message this[int index]
        {
            get
            {
                Assert.Equal(0, index);
                if (Interlocked.Increment(ref _reads) == 1)
                    StagingEntered.Set();
                else if (holdPublication)
                {
                    Admitted.Set();
                    Assert.True(ReleasePublication.Wait(Watchdog));
                }
                return part;
            }
        }
        public IEnumerator<Message> GetEnumerator() =>
            ((IEnumerable<Message>)new[] { part }).GetEnumerator();
        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
        public void Dispose()
        {
            StagingEntered.Dispose();
            Admitted.Dispose();
            ReleasePublication.Dispose();
        }
    }
}
