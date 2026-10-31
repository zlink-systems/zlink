using System.Diagnostics;
using System.Reflection;
using System.Runtime.ExceptionServices;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed partial class StatefulServiceRuntimeTests
{
    [Fact]
    public Task ControlSendNotConnected_KeepsAdmissionUntilSelectedRouteEnds() =>
        VerifyControlSendFailureAsync(
            ZlinkSubmitException.ErrorCode.NotConnected,
            nameof(ControlSendNotConnected_KeepsAdmissionUntilSelectedRouteEnds)
        );

    [Fact]
    public Task ControlSendNotFound_KeepsAdmissionUntilSelectedRouteEnds() =>
        VerifyControlSendFailureAsync(
            ZlinkSubmitException.ErrorCode.NotFound,
            nameof(ControlSendNotFound_KeepsAdmissionUntilSelectedRouteEnds)
        );

    [Fact]
    public Task ControlSendTerminated_KeepsAdmissionUntilSelectedRouteEnds() =>
        VerifyControlSendFailureAsync(
            ZlinkSubmitException.ErrorCode.Terminated,
            nameof(ControlSendTerminated_KeepsAdmissionUntilSelectedRouteEnds)
        );

    private static async Task VerifyControlSendFailureAsync(
        ZlinkSubmitException.ErrorCode result,
        string methodName
    )
    {
        if (
            await IsolatedTestProcess.RunIsolatedAsync(
                typeof(StatefulServiceRuntimeTests),
                methodName
            )
        )
            return;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        Exception? failure = null;
        var failed = 0;
        var scheduler = new ControlFailureTaskScheduler();
        var captured = new CapturingMeshSocketContext(context)
        {
            BeforeSend = () =>
            {
                if (Interlocked.Exchange(ref failure, null) is not { } error)
                    return;
                Interlocked.Increment(ref failed);
                scheduler.MarkFailedTurn();
                throw error;
            },
        };
        await using var owner = new ZLinkManagedMeshNode(
            captured,
            "mesh",
            routedSubmitScheduler: scheduler,
            receivePollerFactory: () =>
                new MeshNodeShutdownSealTests.CanonicalRequestPoller(
                    Systems.Zlink.Zlink.CreatePoller(),
                    socket =>
                        socket is CapturingMeshSocketContext.CapturingRouterSocketProxy proxy
                            ? proxy.Native
                            : socket
                )
        );
        owner.SetRoutingId(RoutingId.From("control-failure-owner"));
        owner.SetBind("tcp://127.0.0.1:0");
        owner.Start();
        using var peer = context.CreateDealerSocket();
        using var completions = new TestCompletionPollerDriver(peer);
        var rid = RoutingId.From("control-failure-peer");
        const string peerEndpoint = "inproc://control-failure-peer";
        peer.SetRoutingId(rid);
        peer.Connect(Assert.IsType<string>(owner.Status().LocalEndpoint));
        using (
            var hello = Message.From(
                ZLinkServiceWireCodec.EncodeRouteAdmission(
                    ServiceWireConstants.Command.Hello,
                    "mesh",
                    peerEndpoint,
                    7,
                    1,
                    new Dictionary<string, uint>(),
                    objectRole: (byte)ZLinkMeshNodeObjectRole.Server
                )
            )
        )
            await peer.Send().Message(hello).Async().EnsureAcceptedAsync();
        using var admission = await RawMeshPeerAdmission.CompleteAsync(
            peer,
            "mesh",
            peerEndpoint,
            7
        );
        await ObserveAsync(() => owner.Status().AdmittedPeerCount == 1);
        var generation = owner.ObservePeerRoute(rid).ReadyGeneration;
        Assert.NotEqual(0UL, generation);
        Interlocked.Exchange(ref failure, new ZlinkSubmitException(result));
        using (
            var probe = Message.From(
                ZLinkServiceWireCodec.EncodeLiveness(ServiceWireConstants.Command.LivenessProbe, 1)
            )
        )
            await peer.Send().Message(probe).Async().EnsureAcceptedAsync();
        // The scheduler signals after the throwing send and its synchronous catch finish.
        await scheduler.FailedTurn.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, Volatile.Read(ref failed));
        Assert.Equal(1U, owner.Status().AdmittedPeerCount);
        Assert.Equal(generation, owner.ObservePeerRoute(rid).ReadyGeneration);
        Assert.Equal(MeshPeerState.Admitted, Assert.Single(owner.Peers()).State);

        // Physical route removal is independently observed by the existing receive owner.
        captured.Router!.DisconnectRid(rid);
        await ObserveAsync(() => owner.Status().AdmittedPeerCount == 0);
        Assert.Equal(0UL, owner.ObservePeerRoute(rid).ReadyGeneration);
        Assert.NotEqual(MeshNodeState.Error, owner.Status().State);
    }

    private sealed class ControlFailureTaskScheduler : TaskScheduler
    {
        private readonly TaskCompletionSource _completed = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private Task? _executingTask;
        private Task? _failedTask;

        internal Task FailedTurn => _completed.Task;

        internal void MarkFailedTurn() =>
            Volatile.Write(ref _failedTask, Volatile.Read(ref _executingTask));

        protected override IEnumerable<Task> GetScheduledTasks() => [];

        protected override bool TryExecuteTaskInline(Task task, bool taskWasPreviouslyQueued) =>
            false;

        protected override void QueueTask(Task task) =>
            ThreadPool.QueueUserWorkItem(_ =>
            {
                Volatile.Write(ref _executingTask, task);
                TryExecuteTask(task);
                if (ReferenceEquals(Volatile.Read(ref _failedTask), task))
                    _completed.TrySetResult();
            });
    }

    [Fact]
    public async Task LivenessDisconnect_NonNotFoundBindingFailureIsExposed()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var captured = new CapturingMeshSocketContext(context);
        try
        {
            var rid = RoutingId.From("disconnect-error-peer");
            foreach (
                var result in Enum.GetValues<ZlinkConnectException.ErrorCode>()
                    .Where(result => result != ZlinkConnectException.ErrorCode.Ok)
            )
            {
                var failure = new ZlinkConnectException(result);
                captured.DisconnectFailure = failure;
                using var socket = captured.CreateRouterSocket();
                var before = captured.DisconnectRidCalls;
                if (result == ZlinkConnectException.ErrorCode.NotFound)
                    ZLinkManagedMeshNode.DisconnectPhysicalPeer(socket, rid);
                else
                {
                    var error = Assert.Throws<ZlinkConnectException>(() =>
                        ZLinkManagedMeshNode.DisconnectPhysicalPeer(socket, rid)
                    );
                    Assert.Equal(failure.Result, error.Result);
                    Assert.Same(failure, error);
                }
                Assert.Equal(before + 1, captured.DisconnectRidCalls);
            }
            using var native = context.CreateRouterSocket();
            var proxy = DispatchProxy.Create<
                IRouterSocket,
                CapturingMeshSocketContext.CapturingRouterSocketProxy
            >();
            var unrelated = new InvalidOperationException("unrelated disconnect failure");
            var calls = 0;
            ((CapturingMeshSocketContext.CapturingRouterSocketProxy)(object)proxy).Initialize(
                native,
                () =>
                {
                    calls++;
                    throw unrelated;
                }
            );
            Assert.Same(
                unrelated,
                Assert.Throws<InvalidOperationException>(() =>
                    ZLinkManagedMeshNode.DisconnectPhysicalPeer(proxy, rid)
                )
            );
            Assert.Equal(1, calls);
        }
        finally
        {
            captured.DisposeCapturedRouter();
        }
    }

    [Fact]
    public async Task LivenessDisconnect_AlreadyMissingPhysicalPeerExpiresLogicalAdmission()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var socket = context.CreateRouterSocket();
        var scheduler = new GatedTaskScheduler();
        var peers = new Dictionary<RoutingId, ZLinkMeshPeer>();
        await using var owner = new ZLinkManagedMeshNode(
            context,
            "mesh",
            routedSubmitScheduler: scheduler,
            peersByRid: peers,
            directRequestSocket: socket
        );
        owner.SetRoutingId(RoutingId.From("liveness-missing-owner"));
        owner.SetBind("tcp://127.0.0.1:0");
        using (var physicalPeer = context.CreateDealerSocket())
        {
            var rid = RoutingId.From("liveness-missing-peer");
            physicalPeer.SetRoutingId(rid);
            var endpoint = $"inproc://liveness-missing-{Guid.NewGuid():N}";
            socket.Bind(endpoint);
            physicalPeer.Connect(endpoint);
            await ObserveAsync(() => socket.RoutesSnapshot().Count == 1);
            var route = Assert.Single(socket.RoutesSnapshot());
            var peer = new ZLinkMeshPeer(1)
            {
                RoutingId = rid,
                PhysicalRoutingId = rid,
                RouteGeneration = route.RouteGeneration,
                Admitted = true,
                State = MeshPeerState.Admitted,
                Liveness = new ZLinkServiceLiveness(0),
            };
            peers.Add(rid, peer);
            try
            {
                // No receive loop runs. Keep the accepted probe on its existing scheduler.
                TickLiveness(owner, 0);
                await scheduler.Queued.WaitAsync(TimeSpan.FromSeconds(5));
                socket.DisconnectRid(rid);
                physicalPeer.Disconnect(endpoint);
                var missing = Assert.Throws<ZlinkConnectException>(() => socket.DisconnectRid(rid));
                Assert.Equal(ZlinkConnectException.ErrorCode.NotFound, missing.Result);
                Assert.Equal(1U, owner.Status().AdmittedPeerCount);
                Assert.Single(owner.Peers(), candidate => candidate.RoutingId == rid);
                TickLiveness(owner, 15 * Stopwatch.Frequency);
                Assert.Equal(0U, owner.Status().AdmittedPeerCount);
                Assert.NotEqual(MeshNodeState.Error, owner.Status().State);
                Assert.DoesNotContain(owner.Peers(), candidate => candidate.RoutingId == rid);
            }
            finally
            {
                await scheduler.ReleaseAsync();
            }
        }
    }

    [Fact]
    public async Task LivenessDisconnect_AlreadyMissingPhysicalPeerKeepsReceiveOwnerRunning()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var captured = new CapturingMeshSocketContext(context);
        await using var owner = NewNode(captured, "liveness-running-owner");
        owner.SetBind("tcp://127.0.0.1:0");
        owner.Start();
        await using (var peer = NewNode(context, "liveness-departing-peer"))
        {
            peer.Start();
            peer.ConnectPeer(Assert.IsType<string>(owner.Status().LocalEndpoint), owner.RoutingId);
            await ObserveAsync(() =>
                owner.Status().AdmittedPeerCount == 1 && peer.Status().AdmittedPeerCount == 1
            );
            await VerifyLivenessDeliveryAsync(owner, peer, "before-disconnect");
            captured.Router!.DisconnectRid(peer.RoutingId);
            peer.DisconnectPeer(owner.RoutingId);
            // Liveness uses Stopwatch, independently of the operation deadline clock.
            // Keep the original 15-second liveness interval and five-second observation bound.
            await Task.Delay(ZLinkServiceLiveness.PeerTimeout);
            await ObserveAsync(() =>
                !owner.Peers().Any(candidate => candidate.RoutingId == peer.RoutingId)
            );
            Assert.Equal(0U, owner.Status().AdmittedPeerCount);
            Assert.NotEqual(MeshNodeState.Error, owner.Status().State);
        }
        await using var healthy = NewNode(context, "liveness-after-cleanup");
        healthy.Start();
        healthy.ConnectPeer(Assert.IsType<string>(owner.Status().LocalEndpoint), owner.RoutingId);
        await ObserveAsync(() =>
            owner.Status().AdmittedPeerCount == 1 && healthy.Status().AdmittedPeerCount == 1
        );
        await VerifyLivenessDeliveryAsync(healthy, owner, "after-cleanup");
        await VerifyLivenessDeliveryAsync(owner, healthy, "reply-after-cleanup");
        Assert.NotEqual(MeshNodeState.Error, owner.Status().State);
    }

    [Fact]
    public async Task LivenessDisconnect_ExplicitNativeDisconnectFailureKeepsPeer()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var peers = new Dictionary<RoutingId, ZLinkMeshPeer>();
        await using var owner = new ZLinkManagedMeshNode(context, "mesh", peersByRid: peers);
        owner.SetRoutingId(RoutingId.From("native-disconnect-failure"));
        using var socket = context.CreateRouterSocket();
        using var physicalPeer = context.CreateDealerSocket();
        var rid = RoutingId.From("native-disconnect-failure-peer");
        physicalPeer.SetRoutingId(rid);
        var endpoint = $"inproc://native-disconnect-failure-{Guid.NewGuid():N}";
        socket.Bind(endpoint);
        physicalPeer.Connect(endpoint);
        await ObserveAsync(() => socket.RoutesSnapshot().Count == 1);
        var route = Assert.Single(socket.RoutesSnapshot());
        var peer = new ZLinkMeshPeer(1)
        {
            RoutingId = rid,
            PhysicalRoutingId = rid,
            RouteGeneration = route.RouteGeneration,
            Admitted = true,
            State = MeshPeerState.Admitted,
        };
        peers.Add(rid, peer);
        SetLivenessField(owner, "_socket", socket);
        var delivered = 0;
        typeof(ZLinkManagedMeshNode)
            .GetEvent(
                "PeerConnectionIntentRemoved",
                BindingFlags.Instance | BindingFlags.NonPublic
            )!
            .GetAddMethod(nonPublic: true)!
            .Invoke(
                owner,
                [
                    new Func<RoutingId, Action?>(removed =>
                    {
                        Assert.Equal(rid, removed);
                        return () => delivered++;
                    }),
                ]
            );
        try
        {
            // A closed concrete socket exercises failure before logical removal.
            // It is never registered with the process receive pool.
            socket.Dispose();
            owner.DisconnectPeer(rid, checked(peer.LifecycleGeneration + 1));
            Assert.Equal(0, delivered);
            Assert.Equal(1U, owner.Status().AdmittedPeerCount);
            Assert.Throws<ObjectDisposedException>(() => owner.DisconnectPeer(rid));
            Assert.Equal(1, delivered);
            Assert.Single(owner.Peers(), candidate => candidate.RoutingId == rid);
            Assert.Equal(1U, owner.Status().AdmittedPeerCount);
        }
        finally
        {
            SetLivenessField(owner, "_socket", null);
        }
    }

    private static async Task VerifyLivenessDeliveryAsync(
        ZLinkManagedMeshNode sender,
        ZLinkManagedMeshNode receiver,
        string payload
    )
    {
        Assert.Equal(
            SubmitResult.Ok,
            sender.SendToNode(
                receiver.RoutingId,
                [Message.From(System.Text.Encoding.UTF8.GetBytes(payload))]
            )
        );
        await ObserveAsync(() => receiver.Status().PendingApplicationMessages != 0);
        using var ready = new MeshReadyBatch();
        receiver.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
        Assert.True(ready.Count > 0);
        var delivered = false;
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            while (claim.Receive(received, RecvFlags.DontWait))
            {
                for (var record = 0; record < received.Count; record++)
                    foreach (var part in received.RetainMessage(record))
                    {
                        delivered |= part.AsReadOnlySpan()
                            .SequenceEqual(System.Text.Encoding.UTF8.GetBytes(payload));
                        part.Dispose();
                    }
                received.Reset();
            }
        }
        Assert.True(delivered);
    }

    private static void TickLiveness(ZLinkManagedMeshNode node, long now)
    {
        try
        {
            typeof(ZLinkManagedMeshNode)
                .GetMethod("ProcessInfrastructure", BindingFlags.Instance | BindingFlags.NonPublic)!
                .Invoke(node, [now]);
        }
        catch (TargetInvocationException error) when (error.InnerException is not null)
        {
            ExceptionDispatchInfo.Capture(error.InnerException).Throw();
            throw;
        }
    }

    private static void SetLivenessField(object owner, string name, object? value) =>
        owner
            .GetType()
            .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(owner, value);

    private static async Task ObserveAsync(Func<bool> condition)
    {
        var deadline = Stopwatch.GetTimestamp() + 5 * Stopwatch.Frequency;
        while (!condition())
        {
            Assert.True(
                Stopwatch.GetTimestamp() < deadline,
                "Expected public state was not observed."
            );
            await Task.Yield();
        }
    }
}
