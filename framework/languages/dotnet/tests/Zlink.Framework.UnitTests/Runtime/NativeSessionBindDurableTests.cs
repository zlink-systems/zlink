using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task NativeSessionBind_UsesIngressLifecycleWhenPeerChanges(bool retirePeer)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "bind-ingress-target");
        var replyToken = await CaptureCompletedBindRequestTokenAsync(context);
        var sourceRid = RoutingId.From("bind-ingress-source");
        const ulong ingressGeneration = 17;
        const ulong routeGeneration = 23;
        var peer = new ZLinkMeshPeer(1)
        {
            RoutingId = sourceRid,
            LifecycleGeneration = ingressGeneration,
            RouteGeneration = routeGeneration,
            State = MeshPeerState.Admitted,
            Admitted = true,
            Admission = new ZLinkServiceWireCodec.AdmissionRecord(
                "mesh",
                ZLinkServiceSecurityIdentity.Plaintext,
                "inproc://bind-ingress",
                ingressGeneration,
                1,
                new Dictionary<string, uint>(),
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                0,
                new Dictionary<byte, byte[]>(),
                Array.Empty<byte>()
            ),
        };
        var peers =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                typeof(ZLinkManagedMeshNode)
                    .GetField("_peersByRid", BindingFlags.Instance | BindingFlags.NonPublic)!
                    .GetValue(node)!;
        peers.Add(sourceRid, peer);
        object?[] ingress = [sourceRid, routeGeneration, 0UL];
        Assert.Equal(
            true,
            typeof(ZLinkManagedMeshNode)
                .GetMethod("HasCurrentSource", BindingFlags.Instance | BindingFlags.NonPublic)!
                .Invoke(node, ingress)
        );
        Assert.Equal(ingressGeneration, ingress[2]);
        node.SetBoundSessionBindHandler(
            (_, generation, _, reply, _) =>
            {
                if (generation != ingressGeneration)
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "The bind lost its ingress lifecycle."
                    );
                reply();
                return ValueTask.CompletedTask;
            }
        );
        var terminal = new CapturedBindReply();
        InvokeBindControl(
            node,
            sourceRid,
            (ulong)ingress[2]!,
            replyToken,
            terminal,
            () =>
            {
                if (retirePeer)
                    peers.Remove(sourceRid);
                else
                {
                    peer.LifecycleGeneration = 19;
                    peer.RouteGeneration = 29;
                    peer.Admission = peer.Admission!.Value with { LifecycleGeneration = 19 };
                }
            }
        );
        var reply = await terminal.Completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(
            (ServiceWireCodec.RequestTerminalResult.Ok, ServiceWireCodec.FrameworkErrorCode.None),
            (reply.TerminalResult, reply.FailureCode)
        );
    }

    [Theory]
    [InlineData(ZLinkDiagnosticsLevel.Normal)]
    [InlineData(ZLinkDiagnosticsLevel.Off)]
    public async Task NativeSessionBind_FailurePreservesOriginalExceptionInFlow(
        ZLinkDiagnosticsLevel level
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "bind-failure-target");
        var replyToken = await CaptureCompletedBindRequestTokenAsync(context);
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(level);
        node.SetFlowTracer(new ZLinkMessageFlowTracer(options));
        var events = new ConcurrentQueue<Activity>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = events.Enqueue,
        };
        ActivitySource.AddActivityListener(listener);
        using var flow = ZLinkFlowContext.EnterCurrentOrCreate(
            ZLinkFlowOrigin.Application,
            captureEnabled: true
        );
        var flowId = ZLinkFlowContext.Current!.Value.FlowId;
        var cause = new InvalidOperationException("original bound-session cause");
        node.SetBoundSessionBindHandler((_, _, _, _, _) => ValueTask.FromException(cause));
        var terminal = new CapturedBindReply();
        var sourceRid = RoutingId.From("bind-failure-source");
        InvokeBindControl(node, sourceRid, 17, replyToken, terminal);
        var reply = await terminal.Completed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ServiceWireCodec.RequestTerminalResult.InternalError, reply.TerminalResult);
        var failures = events.Where(item =>
            Equals(item.GetTagItem("event_id"), "zlink.message_flow")
            && Equals(item.GetTagItem("packet_name"), "BoundSessionBind")
        );
        if (level == ZLinkDiagnosticsLevel.Off)
        {
            Assert.Empty(failures);
            return;
        }
        var failure = Assert.Single(failures);
        Assert.Equal("replied", failure.GetTagItem("phase"));
        Assert.Equal("failed", failure.GetTagItem("outcome"));
        Assert.Equal("stream", failure.GetTagItem("surface"));
        Assert.Equal("control", failure.GetTagItem("message_kind"));
        Assert.Equal(nameof(InvalidOperationException), failure.GetTagItem("error_type"));
        Assert.Equal(cause.Message, failure.GetTagItem("error_message"));
        Assert.Equal("bind-actor", failure.GetTagItem("actor_id"));
        Assert.Equal(sourceRid.ToHex(), failure.GetTagItem("source_rid"));
        Assert.Equal(node.RoutingId.ToHex(), failure.GetTagItem("target_rid"));
        Assert.Equal(flowId, failure.GetTagItem("flow_id"));
    }

    [Theory]
    [InlineData(false, 17UL, true)]
    [InlineData(false, 19UL, false)]
    [InlineData(true, 17UL, true)]
    [InlineData(true, 19UL, false)]
    public async Task NativeSessionNotification_FenceUsesIngressLifecycle(
        bool replaced,
        ulong wireGeneration,
        bool accepted
    )
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = NewNode(context, "notification-ingress-target");
        var source = RoutingId.From("notification-ingress-source");
        var localGeneration = node.Status().LifecycleGeneration;
        var peers =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                typeof(ZLinkManagedMeshNode)
                    .GetField("_peersByRid", BindingFlags.Instance | BindingFlags.NonPublic)!
                    .GetValue(node)!;
        peers.Add(source, new ZLinkMeshPeer(1) { LifecycleGeneration = 19 });
        object record = replaced
            ? new ZLinkServiceWireCodec.BoundSessionReplacedRecord(
                new("bind-actor", 1, source, wireGeneration, 3, 5),
                new(
                    node.RoutingId,
                    localGeneration,
                    "bind-owner",
                    7,
                    RoutingId.From("bind-session"),
                    1
                )
            )
            : new ZLinkServiceWireCodec.MessageFollowRecord(
                new(1, "bind-actor", 1, source, wireGeneration, 3, 5),
                new(1, "bind-actor", 1, node.RoutingId, localGeneration, 4, 7),
                1,
                0,
                0,
                new MeshOperationId(1, 2),
                0
            );
        var method = replaced ? "IsValidBoundSessionReplacedSource" : "IsValidMessageFollowSource";
        Assert.Equal(
            accepted,
            typeof(ZLinkManagedMeshNode)
                .GetMethod(method, BindingFlags.Instance | BindingFlags.NonPublic)!
                .Invoke(node, [source, 17UL, record])
        );
    }

    private static void InvokeBindControl(
        ZLinkManagedMeshNode node,
        RoutingId source,
        ulong sourceGeneration,
        ReplyToken replyToken,
        CapturedBindReply reply,
        Action? beforeBind = null
    )
    {
        var request = CanonicalBindRequest(node.RoutingId, 7);
        var encoded = ServiceWireCodec.EncodeBoundSessionBind38(
            request,
            ServiceWireCodec.DecodeContext.Empty
        );
        using var head = Message.From(encoded[0]);
        var handled = typeof(ZLinkManagedMeshNode)
            .GetMethod(
                "ProcessInfrastructureControl",
                BindingFlags.Instance | BindingFlags.NonPublic
            )!
            .Invoke(
                node,
                [
                    source,
                    23UL,
                    sourceGeneration,
                    replyToken,
                    new[] { head },
                    encoded[0],
                    (Func<ReplyOperation>)(
                        () =>
                        {
                            beforeBind?.Invoke();
                            return reply;
                        }
                    ),
                    null,
                ]
            );
        Assert.Equal(true, handled);
    }

    private static async Task<ReplyToken> CaptureCompletedBindRequestTokenAsync(IContext context)
    {
        await using var router = context.CreateRouterSocket();
        await using var dealer = context.CreateDealerSocket();
        var endpoint = $"inproc://bind-control-token-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using var driver = new TestCompletionPollerDriver(dealer);
        using var request = Message.From("bind-token");
        var submission = dealer.Request().Message(request).Timeout(TimeSpan.FromSeconds(5)).Async();
        using var received = Received.Create();
        Assert.True(router.Recv(received));
        var token = Assert.IsType<ReplyToken>(received.ReplyToken);
        using var response = Message.From("bound");
        received.Reply().Message(response).Submit();
        var parts = await submission.Reply.WaitAsync(TimeSpan.FromSeconds(5));
        foreach (var part in parts)
            part.Dispose();
        return token;
    }

    private sealed class CapturedBindReply : ReplyOperation, ReplySubmitOperation
    {
        private readonly List<Message> _parts = [];
        internal TaskCompletionSource<ServiceWireCodec.Reply20> Completed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ReplySubmitOperation Message(Message message)
        {
            _parts.Add(message);
            return this;
        }

        public void Submit() =>
            Completed.TrySetResult(
                ServiceWireCodec.DecodeReply20(
                    _parts.Select(part => part.ToArray()).ToArray(),
                    ServiceWireCodec.DecodeContext.Empty with
                    {
                        OriginalOperationKind = ServiceWireCodec.MeshOperationKind.StreamBind,
                    }
                )
            );
    }

    [Fact]
    public async Task NativeSessionBind_IntentRemovalEndsWithoutWaitingForDeadline()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var deadlineTime = new ControllableTimeProvider();
        await using var source = NewNode(
            context,
            "bind-intent-source",
            deadlineTimeProvider: deadlineTime
        );
        var target = RoutingId.From("bind-intent-target");
        var endpoint = $"inproc://bind-intent-{Guid.NewGuid():N}";
        source.SetPeerExpectation(target, endpoint, ZLinkServiceSecurityIdentity.Plaintext, 7);
        source.Start();
        var request = source
            .BindBoundSessionAsync(
                CanonicalBindRequest(target, 7),
                TimeSpan.FromSeconds(2),
                CancellationToken.None
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        source.RemovePeerExpectation(target, endpoint);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            request.WaitAsync(TimeSpan.FromSeconds(1))
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
    }

    [Fact]
    public async Task NativeDurablePending_ImmediateIntentRemovalPublishesTerminal()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var deadlineTime = new ControllableTimeProvider();
        await using var source = NewNode(
            context,
            "bind-pending-source",
            deadlineTimeProvider: deadlineTime
        );
        var target = RoutingId.From("bind-pending-target");
        var endpoint = $"inproc://bind-pending-{Guid.NewGuid():N}";
        source.SetPeerExpectation(target, endpoint, ZLinkServiceSecurityIdentity.Plaintext, 7);
        source.Start();
        var reservation = new ObjectReservationFence(
            "bind-reservation",
            "bind-store",
            11,
            13,
            target,
            7,
            "bind-owner",
            17,
            1
        );
        // Intent retirement fixes the outcome immediately; the durable owner
        // publishes its record asynchronously. Keep the deadline clock frozen
        // and wait for that record's ready edge before consuming it.
        var completionReady = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        source.SetReadyHandler(domains =>
        {
            completionReady.TrySetResult();
            return domains;
        });
        var timeout = TimeSpan.FromSeconds(2);
        Assert.Equal(
            SubmitResult.Ok,
            source.CreateActorRemote(
                target,
                "pending-actor",
                "Sample.Actor",
                reservation,
                checked((ulong)deadlineTime.GetUtcNow().Add(timeout).ToUnixTimeMilliseconds()),
                out var operation,
                timeout
            )
        );
        source.RemovePeerExpectation(target, endpoint);
        await completionReady.Task.WaitAsync(timeout);
        var completion = Assert.Single(
            DrainRecords(source),
            record => record.OperationId == operation && record.Kind == MeshRecordKind.Completion
        );
        Assert.Equal((int)RequestResult.NotConnected, completion.TerminalResult);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task NativeSessionBind_WaitsForAdmissionAndConsumesOneTerminal(bool rejected)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "bind-source");
        await using var target = NewNode(context, "bind-target");
        var suffix = Guid.NewGuid().ToString("N");
        source.SetBind($"inproc://bind-source-{suffix}");
        var endpoint = $"inproc://bind-target-{suffix}";
        target.SetBind(endpoint);
        source.Start();
        target.Start();
        var attempts = 0;
        target.SetBoundSessionBindHandler(
            (sourceRid, generation, binding, reply, cancellationToken) =>
            {
                attempts++;
                Assert.Equal(source.RoutingId, sourceRid);
                Assert.NotEqual(0UL, generation);
                Assert.Equal("bind-actor", binding.Actor.Actor.ActorId.Value);
                if (rejected)
                    return ValueTask.FromException(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            "terminal rejection"
                        )
                    );
                reply();
                return ValueTask.CompletedTask;
            }
        );
        var request = source
            .BindBoundSessionAsync(
                CanonicalBindRequest(target.RoutingId, target.Status().LifecycleGeneration),
                TimeSpan.FromSeconds(2),
                CancellationToken.None
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        source.ConnectPeer(endpoint, target.RoutingId);
        if (rejected)
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => request);
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        }
        else
        {
            var accepted = await request;
            Assert.NotNull(accepted);
            Assert.Equal(1UL, accepted.BindingGeneration.Value);
            Assert.Equal(3UL, accepted.AuthorityOwnerGeneration.Value);
        }
        Assert.Equal(1, attempts);
        using var ready = new MeshReadyBatch();
        target.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        Assert.Equal(0, ready.Count);
    }

    private static ServiceWireCodec.BoundSessionBind38 CanonicalBindRequest(
        RoutingId target,
        ulong generation
    ) =>
        new(
            0,
            new(1),
            new(
                new(new("bind-actor"), new(1)),
                new(target.ToBytes().ToArray()),
                new(generation),
                new(3),
                new(5)
            ),
            new(RoutingId.From("bind-session").ToBytes().ToArray()),
            new ServiceWireCodec.BoundSessionBindingTransitionCase0(
                ServiceWireCodec.BoundSessionBindingState.Active,
                new(1)
            )
        );
}
