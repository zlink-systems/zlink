using System.Collections;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_submit_close_admission
{
    private static readonly TimeSpan Watchdog = TimeSpan.FromSeconds(5);

    [Theory]
    [InlineData("Send")]
    [InlineData("Request")]
    [InlineData("Reply")]
    public async Task native_preparation_orders_close(string operation)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        for (var iteration = 0; iteration < 1000; iteration++)
        {
            using var router = context.CreateRouterSocket();
            using var dealer = context.CreateRouterSocket();
            using var poller = Zlink.CreatePoller();
            var endpoint = CoreTestSupport.NewEndpoint("inproc", "submit-close");
            router.SetRoutingId(CoreTestSupport.RoutingIdUtf8("server"));
            dealer.SetRoutingId(CoreTestSupport.RoutingIdUtf8("peer"));
            router.Bind(endpoint);
            dealer.Connect(endpoint);
            poller.Add(dealer, PollEventFlags.PollCompletion, 1);
            using var seed = Message.From("request");
            var pending = dealer.Request(CoreTestSupport.RoutingIdUtf8("server"))
                .Message(seed).Timeout(Watchdog).Async().Reply;
            using var received = Receive(router);
            using var message = Message.From("payload");
            using var parts = new StagingParts(message);
            var owner = CompletionOwnerTestAccess.Owner(router);
            var submit = Task.Factory.StartNew(() => Record.Exception(() =>
            {
                object? attempt = operation switch
                {
                    "Send" => CompletionOwnerTestAccess.Invoke(owner, "SubmitSend",
                        received.RoutingId, parts, 1, IntPtr.Zero),
                    "Request" => CompletionOwnerTestAccess.Invoke(owner, "SubmitRequest",
                        received.RoutingId, parts, 5000u, 1, IntPtr.Zero),
                    "Reply" => CompletionOwnerTestAccess.Invoke(owner, "Reply",
                        received.RoutingId!.Value, received.ReplyToken, parts),
                    _ => throw new ArgumentOutOfRangeException(nameof(operation))
                };
                if (attempt is not null && CompletionOwnerTestAccess.Property(attempt, "Failure")
                        is Exception failure)
                    throw failure;
            }), CancellationToken.None, TaskCreationOptions.LongRunning, TaskScheduler.Default);
            Exception? closeFailure;
            try
            {
                Assert.True(parts.Entered.Wait(Watchdog));
                closeFailure = Record.Exception(router.Dispose);
            }
            finally
            {
                parts.Continue.Set();
            }
            var failure = await submit.WaitAsync(Watchdog);
            Assert.NotNull(closeFailure);
            if (closeFailure is not null)
            {
                var busy = Assert.IsType<ZlinkCloseException>(closeFailure);
                Assert.Equal(ZlinkCloseException.ErrorCode.Busy, busy.Result);
                Assert.Equal(CoreTestSupport.Errno("EBUSY"), busy.NativeErrno);
            }
            if (failure is not null)
                Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated,
                    Assert.IsType<ZlinkSubmitException>(failure).Result);
            router.Dispose();
            poller.Remove(dealer);
            dealer.Dispose();
            _ = await Record.ExceptionAsync(async () =>
            {
                var reply = await pending;
                Zlink.MultipartClose(reply);
            });
        }
    }

    [Theory]
    [InlineData("Send")]
    [InlineData("Request")]
    [InlineData("Reply")]
    public void submit_after_close_is_terminated(string operation)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateRouterSocket();
        using var poller = Zlink.CreatePoller();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "closed-submit");
        router.SetRoutingId(CoreTestSupport.RoutingIdUtf8("server"));
        dealer.SetRoutingId(CoreTestSupport.RoutingIdUtf8("peer"));
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        poller.Add(dealer, PollEventFlags.PollCompletion, 1);
        using var seed = Message.From("request");
        var pending = dealer.Request(CoreTestSupport.RoutingIdUtf8("server"))
            .Message(seed).Timeout(Watchdog).Async().Reply;
        using var received = Receive(router);
        using var message = Message.From("payload");
        Action submit = operation switch
        {
            "Send" => () => router.Send(received.RoutingId!.Value).Message(message).Submit(),
            "Request" => () => router.Request(received.RoutingId!.Value).Message(message)
                .Timeout(Watchdog).Submit(),
            "Reply" => () => received.Reply().Message(message).Submit(),
            _ => throw new ArgumentOutOfRangeException(nameof(operation))
        };
        router.Dispose();
        var failure = Assert.Throws<ZlinkSubmitException>(submit);
        Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, failure.Result);
        Assert.Equal(CoreTestSupport.Errno("ESHUTDOWN"), failure.NativeErrno);
        poller.Remove(dealer);
        dealer.Dispose();
        _ = pending.Exception;
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

    [Theory]
    [InlineData("Send")]
    [InlineData("Request")]
    [InlineData("Reply")]
    public void preparation_failure_releases_native_reservation(string operation)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        var owner = CompletionOwnerTestAccess.Owner(socket);
        var target = CoreTestSupport.RoutingIdUtf8("target");
        var exception = Record.Exception(() =>
        {
            object? attempt = operation switch
            {
                "Send" => CompletionOwnerTestAccess.Invoke(owner, "SubmitSend",
                    target, new FailingParts(), 1, IntPtr.Zero),
                "Request" => CompletionOwnerTestAccess.Invoke(owner, "SubmitRequest",
                    target, new FailingParts(), 5000u, 1, IntPtr.Zero),
                "Reply" => CompletionOwnerTestAccess.Invoke(owner, "Reply", target,
                    CompletionOwnerTestAccess.Create(typeof(ReplyToken), new object(), 1ul),
                    new FailingParts()),
                _ => throw new ArgumentOutOfRangeException(nameof(operation))
            };
            if (attempt is not null && CompletionOwnerTestAccess.Property(attempt, "Failure")
                    is Exception failure)
                throw failure;
        });
        Assert.IsType<ArgumentException>(exception);
        socket.Dispose();
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    public void publish_after_close_is_terminated(int partCount)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var socket = context.CreatePubSocket();
        using var first = Message.From("first");
        using var second = Message.From("second");
        var operation = socket.Publish("topic").Message(first);
        if (partCount == 2)
            operation.Message(second);
        socket.Dispose();
        var failure = Assert.Throws<ZlinkSubmitException>(operation.Submit);
        Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, failure.Result);
        Assert.Equal(CoreTestSupport.Errno("ESHUTDOWN"), failure.NativeErrno);
    }

    [Fact]
    public void receive_after_close_reports_socket_shutdown()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        using var received = Received.Create();
        socket.Dispose();
        var failure = Assert.Throws<ZlinkRecvException>(() =>
            socket.Recv(received, RecvFlags.DontWait));
        Assert.Equal(ZlinkRecvException.ErrorCode.InvalidState, failure.Result);
        Assert.Equal(CoreTestSupport.Errno("ESHUTDOWN"), failure.NativeErrno);
    }

    [Fact]
    public async Task shutdown_close_settles_pending_request_before_throwing()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        using var poller = Zlink.CreatePoller();
        var endpoint = CoreTestSupport.NewEndpoint("inproc", "shutdown-close");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        poller.Add(dealer, PollEventFlags.PollCompletion, 1);
        using var message = Message.From("pending");
        var pending = dealer.Request().Message(message).Timeout(Watchdog).Async().Reply;
        using var received = Receive(router);
        Assert.True(poller.Remove(dealer));

        var owner = CompletionOwnerTestAccess.Owner(dealer);
        var handle = CompletionOwnerTestAccess.Field(owner, "_handle");
        var nativeHandle = CompletionOwnerTestAccess.Property(handle, "NativeHandle");
        var native = CompletionOwnerTestAccess.RuntimeType("Systems.Zlink.Runtime.Native.NativeMethods");
        Assert.Equal(0, (int)CompletionOwnerTestAccess.InvokeStatic(native,
            "zlink_close", nativeHandle)!);

        var close = Assert.Throws<ZlinkCloseException>(dealer.Close);
        Assert.Equal(ZlinkCloseException.ErrorCode.Shutdown, close.Result);
        Assert.Equal(CoreTestSupport.Errno("ESHUTDOWN"), close.NativeErrno);
        Assert.True(pending.IsCompleted);
        var failure = await Assert.ThrowsAsync<ZlinkRequestException>(() => pending);
        Assert.Equal(ZlinkRequestException.ErrorCode.Terminated, failure.Result);
        Assert.Empty(CompletionOwnerTestAccess.Entries(owner));
    }

    private sealed class FailingParts : IReadOnlyList<Message>
    {
        public int Count => 1;
        public Message this[int index] => throw new ArgumentException("Invalid payload.");
        public IEnumerator<Message> GetEnumerator() => throw new NotSupportedException();
        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
    }

    private sealed class StagingParts(Message message) : IReadOnlyList<Message>, IDisposable
    {
        internal readonly ManualResetEventSlim Entered = new();
        internal readonly ManualResetEventSlim Continue = new();
        public int Count => 1;
        public Message this[int index]
        {
            get
            {
                Entered.Set();
                Assert.True(Continue.Wait(Watchdog));
                return message;
            }
        }
        public IEnumerator<Message> GetEnumerator() =>
            new[] { message }.AsEnumerable().GetEnumerator();
        IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
        public void Dispose()
        {
            Entered.Dispose();
            Continue.Dispose();
        }
    }
}
