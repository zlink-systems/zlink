using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RequestReplyBoundaryTests
{
    private static readonly TimeSpan Guard = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task NativeReply_ReachesFinalReceiverWithoutForwardingTask()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var server = context.CreateRouterSocket();
        using var native = context.CreateRouterSocket();
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var socket = new CapturingRouter(native);
        await using var node = new ZLinkManagedMeshNode(
            context,
            "reply-boundary",
            directRequestSocket: socket
        );
        var target = RoutingId.From("reply-boundary-target");
        server.SetRoutingId(target);
        native.SetRoutingId(RoutingId.From("reply-boundary-source"));
        server.Options.ReceiveTimeout = Guard;
        var endpoint = $"inproc://reply-boundary-{Guid.NewGuid():N}";
        server.Bind(endpoint);
        poller.Add(native, PollEventFlags.PollRoute | PollEventFlags.PollCompletion, 1);
        native.Connect(endpoint);
        var events = new PollEvent[1];
        while (native.RoutesSnapshot().Count == 0)
            Assert.True(poller.Wait(events, Guard) > 0);
        poller.Modify(native, PollEventFlags.PollCompletion);
        using var payload = Message.From(new byte[] { 7 });
        var reply = node.SubmitNativeWireAsync(target, [payload], Guard, out var admitted, default)
            .AsTask();
        Assert.Equal(1, socket.Submissions);
        Assert.Same(socket.NativeReply, reply);
        Assert.False(reply.IsCompleted);
        await admitted;
        // Replay only the public immutable submission to isolate Framework
        // forwarding allocations from the binding's own operation storage.
        socket.Replay = new ReplayRequest(socket.Submission);
        using var replayPayload = Message.From(new byte[] { 0 });
        IReadOnlyList<Message> replayParts = [replayPayload];
        _ = node.SubmitNativeWireAsync(target, replayParts, Guard, out _, default).AsTask();
        var sameTask = true;
        var allocated = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < 32; index++)
            sameTask &= ReferenceEquals(
                reply,
                node.SubmitNativeWireAsync(target, replayParts, Guard, out _, default).AsTask()
            );
        allocated = GC.GetAllocatedBytesForCurrentThread() - allocated;
        Assert.True(sameTask);
        Assert.Equal(0, allocated);
        using var received = Received.Create();
        Assert.True(server.Recv(received));
        using var response = Message.From(new byte[] { 9 });
        received.Reply().Message(response).Submit();
        Assert.True(poller.Wait(events, Guard) > 0);
        var parts = await reply.WaitAsync(Guard);
        try
        {
            Assert.Equal(9, Assert.Single(parts).AsReadOnlySpan()[0]);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private sealed class CapturingRouter(IRouterSocket inner)
        : PressureSocket<IRouterSocket>(inner),
            IRouterSocket
    {
        internal int Submissions;
        internal RequestSubmission Submission;
        internal RequestOperation? Replay;
        internal Task<IReadOnlyList<Message>>? NativeReply;
        public new RouterSocketOptions Options => InnerSocket.Options;

        public void SetRoutingId(RoutingId rid) => InnerSocket.SetRoutingId(rid);

        public RoutingId GetRoutingId() => InnerSocket.GetRoutingId();

        public SendOperation Send(RoutingId rid) => InnerSocket.Send(rid);

        public RequestOperation Request(RoutingId rid) =>
            Replay ?? new CaptureRequest(InnerSocket.Request(rid), this);

        public ReplyOperation Reply(RoutingId rid, ReplyToken token) =>
            InnerSocket.Reply(rid, token);

        public IReadOnlyList<RouterRoute> RoutesSnapshot() => InnerSocket.RoutesSnapshot();
    }

    private sealed class ReplayRequest(RequestSubmission submission)
        : RequestOperation,
            RequestSubmitOperation
    {
        public RequestSubmitOperation Message(Message message) => this;

        public RequestSubmitOperation Timeout(TimeSpan timeout) => this;

        public IReadOnlyList<Message> Submit() => throw new NotSupportedException();

        public RequestSubmission Async(CancellationToken token = default) => submission;
    }

    private sealed class CaptureRequest(RequestOperation operation, CapturingRouter owner)
        : RequestOperation
    {
        public RequestSubmitOperation Message(Message message) =>
            new CaptureSubmit(operation.Message(message), owner);
    }

    private sealed class CaptureSubmit(RequestSubmitOperation operation, CapturingRouter owner)
        : RequestSubmitOperation
    {
        public RequestSubmitOperation Message(Message message)
        {
            operation.Message(message);
            return this;
        }

        public RequestSubmitOperation Timeout(TimeSpan timeout)
        {
            operation.Timeout(timeout);
            return this;
        }

        public IReadOnlyList<Message> Submit() => operation.Submit();

        public RequestSubmission Async(CancellationToken token = default)
        {
            var submission = operation.Async(token);
            owner.Submissions++;
            owner.Submission = submission;
            owner.NativeReply = submission.Reply;
            return submission;
        }
    }
}
