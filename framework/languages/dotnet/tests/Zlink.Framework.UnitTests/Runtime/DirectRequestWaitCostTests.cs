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

    [Fact]
    public async Task Direct_request_preserves_synchronous_cancellation_status()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var client = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(context, "direct-request-cancellation");
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var nodeType = typeof(ZLinkManagedMeshNode);
        var socketField = nodeType.GetField("_socket", flags)!;
        socketField.SetValue(node, client);
        nodeType
            .GetField("_activeSocketGeneration", flags)!
            .SetValue(node, nodeType.GetField("_lifecycleGeneration", flags)!.GetValue(node));
        var signature = new[]
        {
            typeof(RoutingId),
            typeof(IReadOnlyList<Message>),
            typeof(TimeSpan),
            typeof(CancellationToken),
            typeof(ZLinkServiceLiveness),
            typeof(ZLinkRequestAdmission),
        };
        var forwarding = nodeType.GetMethod("RequestDirectWireAsync", flags, signature)!;
        var submitting = nodeType.GetMethod(
            "RequestDirectWireAsync",
            flags,
            signature
                .Take(3)
                .Concat(new[] { typeof(Task).MakeByRefType() })
                .Concat(signature.Skip(3))
                .ToArray()
        )!;
        object?[] Arguments(bool includeAdmission)
        {
            var arguments = new List<object?>
            {
                RoutingId.From("direct-request-cancellation-target"),
                new[] { Message.From("cancelled") },
                TestGuard,
            };
            if (includeAdmission)
                arguments.Add(null);
            arguments.Add(cancellation.Token);
            arguments.Add(null);
            arguments.Add(null);
            return arguments.ToArray();
        }
        try
        {
            // Prove submission throws synchronously, rather than returning a
            // canceled reply that would bypass the forwarding catch entirely.
            var synchronous = Assert.Throws<TargetInvocationException>(() =>
                submitting.Invoke(node, Arguments(true))
            );
            Assert.IsAssignableFrom<OperationCanceledException>(synchronous.InnerException);
            var pending =
                (ValueTask<IReadOnlyList<Message>>)forwarding.Invoke(node, Arguments(false))!;
            Assert.True(pending.IsCanceled);
            var task = pending.AsTask();
            Assert.Equal(TaskStatus.Canceled, task.Status);
            var failure = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => task);
            Assert.Equal(cancellation.Token, failure.CancellationToken);
        }
        finally
        {
            socketField.SetValue(node, null);
        }
    }

    [Fact]
    public async Task Direct_request_preserves_synchronous_disposal_failure_status()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "direct-request-disposed");
        var forwarding = typeof(ZLinkManagedMeshNode).GetMethod(
            "RequestDirectWireAsync",
            BindingFlags.Instance | BindingFlags.NonPublic,
            [
                typeof(RoutingId),
                typeof(IReadOnlyList<Message>),
                typeof(TimeSpan),
                typeof(CancellationToken),
                typeof(ZLinkServiceLiveness),
                typeof(ZLinkRequestAdmission),
            ]
        )!;
        var pending =
            (ValueTask<IReadOnlyList<Message>>)
                forwarding.Invoke(
                    node,
                    [
                        RoutingId.From("direct-request-disposed-target"),
                        new[] { Message.From("disposed") },
                        TestGuard,
                        CancellationToken.None,
                        null,
                        null,
                    ]
                )!;
        var task = pending.AsTask();
        Assert.Equal(TaskStatus.Faulted, task.Status);
        await Assert.ThrowsAsync<ObjectDisposedException>(() => task);
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
