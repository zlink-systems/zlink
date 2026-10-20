using System.Diagnostics;
using System.Diagnostics.Tracing;
using System.Reflection;
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
        await using var node = new ZLinkManagedMeshNode(context, "direct-request-waits");
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

        // Only Framework state is injected. The socket and request builders
        // use the public binding surface, including its completion poller.
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var nodeType = typeof(ZLinkManagedMeshNode);
        var socketField = nodeType.GetField("_socket", flags)!;
        socketField.SetValue(node, client);
        nodeType
            .GetField("_activeSocketGeneration", flags)!
            .SetValue(node, nodeType.GetField("_lifecycleGeneration", flags)!.GetValue(node));
        var request = nodeType.GetMethod(
            "RequestDirectWireAsync",
            flags,
            [
                typeof(RoutingId),
                encodedWire
                    ? typeof(IReadOnlyList<ReadOnlyMemory<byte>>)
                    : typeof(IReadOnlyList<Message>),
                typeof(TimeSpan),
                typeof(CancellationToken),
                typeof(ZLinkServiceLiveness),
                typeof(ZLinkRequestAdmission),
            ]
        )!;
        using var waits = new SubmissionWaitListener();
        try
        {
            for (var index = 0; index < RequestCount; index++)
            {
                using var payload = Message.From(BitConverter.GetBytes(index));
                ValueTask<IReadOnlyList<Message>> pending;
                waits.Begin();
                try
                {
                    pending =
                        (ValueTask<IReadOnlyList<Message>>)
                            request.Invoke(
                                node,
                                [
                                    target,
                                    encodedWire
                                        ? new ReadOnlyMemory<byte>[]
                                        {
                                            BitConverter.GetBytes(index),
                                        }
                                        : new[] { payload },
                                    TestGuard,
                                    default(CancellationToken),
                                    null,
                                    null,
                                ]
                            )!;
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
                    Assert.Equal(
                        index,
                        BitConverter.ToInt32(Assert.Single(parts).AsReadOnlySpan())
                    );
                }
                finally
                {
                    ZLinkMessageParts.DisposeAll(parts);
                }
            }
        }
        finally
        {
            socketField.SetValue(node, null);
        }
        Assert.Equal(RequestCount, waits.ReplyObservations);
        Assert.Equal(0, waits.ForwardingWaits);
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
