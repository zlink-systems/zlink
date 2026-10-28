using System.Diagnostics;
using System.Diagnostics.Tracing;
using System.Runtime.CompilerServices;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class DirectRequestWaitCostTests
{
    private const int RequestCount = 1000;
    private static readonly TimeSpan TestGuard = TimeSpan.FromSeconds(5);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Direct_request_only_awaits_for_reply_observation(bool encodedWire)
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var server = context.CreateRouterSocket();
        using var client = context.CreateRouterSocket();
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "direct-request-waits",
            directRequestSocket: client
        );
        var target = RoutingId.From("direct-request-waits-target");
        server.SetRoutingId(target);
        client.SetRoutingId(RoutingId.From("direct-request-waits-source"));
        server.Options.ReceiveTimeout = TestGuard;
        var endpoint = $"inproc://direct-request-waits-{Guid.NewGuid():N}";
        server.Bind(endpoint);
        poller.Add(client, PollEventFlags.PollRoute | PollEventFlags.PollCompletion, 1);
        client.Connect(endpoint);
        var events = new PollEvent[1];
        while (client.RoutesSnapshot().Count == 0)
            Assert.True(poller.Wait(events, TestGuard) > 0);
        poller.Modify(client, PollEventFlags.PollCompletion);

        using var waits = new SubmissionWaitListener();
        for (var index = 0; index < RequestCount; index++)
        {
            using var payload = Message.From(BitConverter.GetBytes(index));
            ValueTask<IReadOnlyList<Message>> pending;
            waits.Begin();
            try
            {
                pending = encodedWire
                    ? node.RequestDirectWireAsync(
                        target,
                        new ReadOnlyMemory<byte>[] { BitConverter.GetBytes(index) },
                        TestGuard,
                        CancellationToken.None
                    )
                    : node.RequestDirectWireAsync(
                        target,
                        new[] { payload },
                        TestGuard,
                        CancellationToken.None
                    );
            }
            finally
            {
                waits.End();
            }
            // The server cannot reply until submission returns and its
            // pending awaits are registered, independent of scheduling.
            Assert.False(pending.IsCompleted);
            using var received = Received.Create();
            Assert.True(server.Recv(received));
            Assert.Equal(
                index,
                BitConverter.ToInt32(received.SinglePartOrThrow().AsReadOnlySpan())
            );
            using var reply = Message.From(BitConverter.GetBytes(index));
            received.Reply().Message(reply).Submit();
            Assert.True(poller.Wait(events, TestGuard) > 0);
            var parts = await pending;
            try
            {
                Assert.Equal(index, BitConverter.ToInt32(Assert.Single(parts).AsReadOnlySpan()));
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(parts);
            }
        }
        Assert.Equal(RequestCount, waits.ReplyObservations);
        Assert.Equal(0, waits.ForwardingWaits);
    }

    [Fact]
    public async Task Direct_request_preserves_synchronous_cancellation_status()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var client = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "direct-request-cancellation",
            directRequestSocket: client
        );
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        var target = RoutingId.From("direct-request-cancellation-target");
        // Submission must throw synchronously before forwarding converts its status.
        Assert.ThrowsAny<OperationCanceledException>(() =>
            node.SubmitNativeWireAsync(
                target,
                new[] { Message.From("cancelled") },
                TestGuard,
                out _,
                cancellation.Token
            )
        );
        var pending = node.RequestDirectWireAsync(
            target,
            new[] { Message.From("cancelled") },
            TestGuard,
            cancellation.Token
        );
        // §10.1 requires dispatch through the logical entry. Terminal status
        // is checked after delivery, rather than before its dispatcher turn.
        var task = pending.AsTask();
        var failure = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);
        Assert.Equal(TaskStatus.Canceled, task.Status);
        Assert.Equal(cancellation.Token, failure.CancellationToken);
    }

    [Fact]
    public async Task Direct_request_preserves_synchronous_disposal_failure_status()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "direct-request-disposed");
        var pending = node.RequestDirectWireAsync(
            RoutingId.From("direct-request-disposed-target"),
            new[] { Message.From("disposed") },
            TestGuard,
            CancellationToken.None
        );
        var task = pending.AsTask();
        await Assert.ThrowsAsync<ObjectDisposedException>(() => task);
        Assert.Equal(TaskStatus.Faulted, task.Status);
    }

    private sealed class SubmissionWaitListener : EventListener
    {
        private int _submissionThread;
        internal int ForwardingWaits { get; private set; }
        internal int ReplyObservations { get; private set; }

        internal SubmissionWaitListener()
        {
            foreach (var source in EventSource.GetSources())
                Enable(source);
        }

        protected override void OnEventSourceCreated(EventSource source) => Enable(source);

        private void Enable(EventSource source)
        {
            if (source.Name == "System.Threading.Tasks.TplEventSource")
                EnableEvents(source, EventLevel.Verbose, (EventKeywords)3);
        }

        internal void Begin() =>
            Volatile.Write(ref _submissionThread, Environment.CurrentManagedThreadId);

        internal void End() => Volatile.Write(ref _submissionThread, 0);

        protected override void OnEventWritten(EventWrittenEventArgs data)
        {
            // No stack collection on other tests or after the synchronous
            // submission call returns, even if they run in parallel.
            if (
                data.EventName != "TaskWaitBegin"
                || Volatile.Read(ref _submissionThread) != Environment.CurrentManagedThreadId
            )
                return;
            var waiter = new StackTrace()
                .GetFrames()
                .Select(frame => frame.GetMethod()?.DeclaringType)
                .FirstOrDefault(type =>
                    type is not null && typeof(IAsyncStateMachine).IsAssignableFrom(type)
                );
            if (waiter?.DeclaringType != typeof(ZLinkManagedMeshNode))
                return;
            if (waiter.Name.StartsWith("<RequestDirectWireAsync>", StringComparison.Ordinal))
                ForwardingWaits++;
            if (waiter.Name.StartsWith("<ObserveDirectWireReplyAsync>", StringComparison.Ordinal))
                ReplyObservations++;
        }
    }
}
