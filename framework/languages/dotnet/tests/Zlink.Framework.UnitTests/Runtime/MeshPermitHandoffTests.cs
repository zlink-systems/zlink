using System.Reflection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshPermitHandoffTests
{
    private const nuint ReceiveSocketSlot = 1;

    [Fact]
    public async Task RawIngress_StopsReceivingAtApplicationPermitCapacity()
    {
        const int capacity = 2;
        const int suppliedRecords = 32;
        const ulong sourceGeneration = 17;
        var sourceRid = RoutingId.From("permit-source");

        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var dealer = context.CreateDealerSocket();
        await using var socket = context.CreateRouterSocket();
        var endpoint = $"inproc://permit-ingress-{Guid.NewGuid():N}";
        dealer.SetRoutingId(sourceRid);
        socket.Bind(endpoint);
        dealer.Connect(endpoint);

        // The blocking handshake establishes the inproc route without a timed
        // retry before the records under test are submitted.
        using (var handshake = Message.From("handshake"))
            dealer.Send().Message(handshake).Submit();
        ulong routeGeneration;
        using (var received = Received.Create())
        {
            Assert.True(socket.Recv(received));
            routeGeneration = received.RouteGeneration;
        }

        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, capacity, 1, capacity)
        );
        await using var node = new ZLinkManagedMeshNode(
            context,
            "permit-ingress",
            applicationJobQueue: queue
        );
        using var stop = new CancellationTokenSource();
        AddAdmittedPeer(node, sourceRid, sourceGeneration, routeGeneration);
        SetField(node, "_socket", socket);
        using var poller = CreateReceivePoller(socket);
        SetField(node, "_poller", poller);

        try
        {
            for (var index = 0; index < suppliedRecords; index++)
            {
                using var header = Message.From(
                    ZLinkServiceWireCodec.EncodeApplication(
                        ServiceWireConstants.Command.NodeSend,
                        0,
                        null,
                        false
                    )
                );
                using var payload = Message.From(
                    ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                        new ReadOnlyMemory<byte>[] { new byte[] { checked((byte)index) } }
                    )
                );
                var submission = dealer.Send().Message(header).Message(payload).Async();
                Assert.Equal(SubmitResult.Ok, submission.Result);
                Assert.True(submission.EnsureAcceptedAsync().IsCompletedSuccessfully);
            }

            FillIngress();
            var atCapacity = queue.GetStatus();
            Assert.Equal((ulong)capacity, atCapacity.QueuedApplicationJobs);
            Assert.Equal((ulong)capacity, atCapacity.PermitsInUse);
            Assert.Equal((ulong)capacity, atCapacity.PeakPermitsInUse);
            Assert.Equal((uint)capacity, node.Status().PendingApplicationMessages);

            // With both permits owned by queued records, the next
            // drain registers its one waiter and returns before Recv. The next
            // ordinary record is therefore still on the ROUTER socket.
            Assert.True(DrainWhenReadable(node, poller, stop.Token));
            Assert.False(DrainWhenReadable(node, poller, stop.Token));
            Assert.False(DrainWhenReadable(node, poller, stop.Token));
            Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);
            Assert.Equal((uint)capacity, node.Status().PendingApplicationMessages);
            using var retained = Received.Create();
            Assert.True(socket.Recv(retained, RecvFlags.DontWait));
            Assert.Equal(ReceivedMessageType.Raw, retained.MessageType);
            Assert.Equal(
                ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                    new ReadOnlyMemory<byte>[] { new byte[] { capacity } }
                ),
                retained.Parts[1].ToArray()
            );

            // Releasing the claimed records frees both queued leases. The
            // second wave must be admitted from the remaining socket records.
            stop.Cancel();
            DrainAndDisposeApplicationRecords(node, capacity);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);

            FillIngress();
            var secondWave = queue.GetStatus();
            Assert.Equal((ulong)capacity, secondWave.QueuedApplicationJobs);
            Assert.Equal((ulong)capacity, secondWave.PermitsInUse);
            Assert.Equal((uint)capacity, node.Status().PendingApplicationMessages);
            DrainAndDisposeApplicationRecords(node, capacity);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
        }
        finally
        {
            stop.Cancel();
            SetField(node, "_socket", null);
            SetField(node, "_poller", null);
        }

        void FillIngress()
        {
            // A receive turn may stop at its time budget before exhausting
            // permits. All records are already supplied; at most two such
            // turns are needed to fill this queue, with no polling or delay.
            for (var turn = 0; turn < capacity; turn++)
            {
                Drain(node, CancellationToken.None);
                if (queue.GetStatus().PermitsInUse == capacity)
                    return;
            }
        }
    }

    [Fact]
    public async Task RawIngress_ReservedPermitConsumptionAllowsSuccessorWaiter()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var socket = context.CreateRouterSocket();
        socket.Bind($"inproc://permit-handoff-{Guid.NewGuid():N}");
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1)
        );
        await using var node = new ZLinkManagedMeshNode(
            context,
            "permit-handoff",
            applicationJobQueue: queue
        );
        using var stop = new CancellationTokenSource();
        SetField(node, "_socket", socket);
        using var poller = CreateReceivePoller(socket);
        SetField(node, "_poller", poller);

        // Arrange the producer's published reservation. The actual lease is
        // the handoff fact, including capacity for a successor waiter.
        var reservation = await queue.AcquireAsync(stop.Token, ZLinkApplicationJobOrigin.Remote);
        SetField(node, "_reservedRawApplicationAdmission", reservation);
        ZLinkApplicationJobQueueLease? held = null;
        try
        {
            Drain(node, stop.Token);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
            held = await queue.AcquireAsync(stop.Token, ZLinkApplicationJobOrigin.Remote);

            Drain(node, stop.Token);
            Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);
        }
        finally
        {
            stop.Cancel();
            held?.Dispose();
            SetField(node, "_socket", null);
            SetField(node, "_poller", null);
        }
    }

    [Fact]
    public async Task RawIngress_LeasesOnlyClaimedRecordsAndReturnsTheUnclaimedReservation()
    {
        const int capacity = 8;
        const int suppliedRecords = 3;
        await using var fixture = await RawIngressFixture.CreateAsync(capacity, "claimed-only");
        for (var index = 0; index < suppliedRecords; index++)
            fixture.SupplyNodeSend(index);

        // A rotation reserves every free permit, claims records until the socket
        // is empty or its 1 ms budget ends, and returns the unclaimed permits
        // from its single return point. Only the claimed records keep a permit.
        for (var rotation = 0; rotation < suppliedRecords; rotation++)
        {
            Drain(fixture.Node, CancellationToken.None);
            var status = fixture.Queue.GetStatus();
            Assert.Equal(0UL, status.ReservedSupplyPermits);
            Assert.Equal(status.QueuedApplicationJobs, status.PermitsInUse);
            Assert.Equal(0UL, status.CapacityWaiters);
            if (status.QueuedApplicationJobs == suppliedRecords)
                break;
        }
        Assert.Equal((ulong)suppliedRecords, fixture.Queue.GetStatus().QueuedApplicationJobs);
        Assert.Equal((ulong)capacity, fixture.Queue.GetStatus().PeakPermitsInUse);

        DrainAndDisposeApplicationRecords(fixture.Node, suppliedRecords);
        Assert.Equal(0UL, fixture.Queue.GetStatus().PermitsInUse);
    }

    [Fact]
    public async Task RawIngress_FailureDuringClaimReturnsTheHeldAndReservedPermits()
    {
        await using var fixture = await RawIngressFixture.CreateAsync(4, "claim-failure");
        // The producer's handed-over permit and the rotation's reservation are
        // both outstanding when Recv fails on a socket closed by shutdown.
        var handedOver = await fixture.Queue.AcquireAsync(
            CancellationToken.None,
            ZLinkApplicationJobOrigin.Remote
        );
        SetField(fixture.Node, "_reservedRawApplicationAdmission", handedOver);
        var waiting = fixture
            .Queue.AcquireAsync(CancellationToken.None, ZLinkApplicationJobOrigin.Remote)
            .AsTask();
        await using (var closed = fixture.Context.CreateRouterSocket())
            SetField(fixture.Node, "_socket", closed);
        // DisposeAsync has completed: the poller still holds the open socket.
        var failure = Assert.Throws<TargetInvocationException>(() =>
            Drain(fixture.Node, CancellationToken.None)
        );
        Assert.IsAssignableFrom<Exception>(failure.InnerException);

        // The held lease returned and admitted the older waiter; the counted
        // reservation was empty because that waiter was queued first.
        using var admitted = await waiting.WaitAsync(TimeSpan.FromSeconds(5));
        var status = fixture.Queue.GetStatus();
        Assert.Equal(1UL, status.PermitsInUse);
        Assert.Equal(0UL, status.CapacityWaiters);
        Assert.Null(
            Field<ZLinkApplicationJobQueueLease?>(fixture.Node, "_reservedRawApplicationAdmission")
        );
        admitted.Dispose();

        // Without a waiter the same failure returns a counted reservation.
        Assert.Throws<TargetInvocationException>(() => Drain(fixture.Node, CancellationToken.None));
        Assert.Equal(0UL, fixture.Queue.GetStatus().PermitsInUse);
        Assert.Equal(4UL, fixture.Queue.GetStatus().PeakPermitsInUse);
    }

    [Fact]
    public async Task RawIngress_FanoutTakesOnePermitPerChildAndKeepsTheRestForTheNextPermit()
    {
        const int capacity = 2;
        await using var fixture = await RawIngressFixture.CreateAsync(capacity, "fanout");
        var spots = Enumerable
            .Range(0, 3)
            .Select(_ => fixture.Node.CreateSpot())
            .Cast<ZLinkManagedSpot>()
            .ToArray();
        foreach (var spot in spots)
            spot.SetSubscription("fanout-channel", "fanout-topic");
        fixture.Supply(
            ZLinkServiceWireCodec.EncodeLogicalMulticast(
                "fanout-channel",
                "fanout-topic",
                "fanout-source",
                false
            ),
            1
        );
        using var stop = new CancellationTokenSource();

        // The record's permit admits the first child and the next permit the
        // second. A rotation that ends at its time budget hands the next free
        // permit to the source at once, so at most two rotations post both.
        for (var rotation = 0; rotation < capacity; rotation++)
        {
            Drain(fixture.Node, stop.Token);
            if (fixture.Queue.GetStatus().QueuedApplicationJobs == capacity)
                break;
        }
        // The third child waits for a later permit as this source's one waiter.
        var first = fixture.Queue.GetStatus();
        Assert.Equal(2UL, first.QueuedApplicationJobs);
        Assert.Equal(0UL, first.ReservedSupplyPermits);
        Assert.Equal(1UL, first.CapacityWaiters);
        Assert.NotNull(Field<object?>(fixture.Node, "_pendingRawMulticast"));
        Assert.Null(
            Field<ZLinkApplicationJobQueueLease?>(fixture.Node, "_reservedRawApplicationAdmission")
        );

        stop.Cancel();
        Assert.Equal(0UL, fixture.Queue.GetStatus().CapacityWaiters);
        Assert.Equal(2, DrainAndDisposeAllApplicationRecords(fixture.Node));
        Assert.Equal(0UL, fixture.Queue.GetStatus().PermitsInUse);

        // The handed-over permit admits the last child. The rotation tops its
        // budget up to two; the unclaimed permit returns when the socket is empty.
        SetField(
            fixture.Node,
            "_reservedRawApplicationAdmission",
            await fixture.Queue.AcquireAsync(
                CancellationToken.None,
                ZLinkApplicationJobOrigin.Remote
            )
        );
        Drain(fixture.Node, CancellationToken.None);
        var last = fixture.Queue.GetStatus();
        Assert.Equal(1UL, last.QueuedApplicationJobs);
        Assert.Equal(0UL, last.ReservedSupplyPermits);
        Assert.Null(Field<object?>(fixture.Node, "_pendingRawMulticast"));
        Assert.Equal(1, DrainAndDisposeAllApplicationRecords(fixture.Node));
        Assert.Equal(0UL, fixture.Queue.GetStatus().PermitsInUse);
    }

    [Fact]
    public async Task RawIngress_SourceAuthorityAdmitsOnlyTheCurrentRouteAndRecordsItsLiveness()
    {
        await using var fixture = await RawIngressFixture.CreateAsync(4, "source-authority");
        var peer = fixture.Peer;
        peer.Liveness = new ZLinkServiceLiveness(0);
        var admittedDeadline = peer.Liveness.DeadlineTimestamp;

        // A record from a route generation the node did not observe is not
        // admitted, is not delivered and records no liveness.
        var current = peer.RouteGeneration;
        peer.RouteGeneration = current + 1;
        fixture.SupplyNodeSend(0);
        Drain(fixture.Node, CancellationToken.None);
        Assert.Equal(0UL, fixture.Queue.GetStatus().PermitsInUse);
        Assert.Equal(0U, fixture.Node.Status().PendingApplicationMessages);
        Assert.Equal(admittedDeadline, peer.Liveness.DeadlineTimestamp);

        // A peer that no longer requires a connection is not a current source.
        peer.RouteGeneration = current;
        peer.State = MeshPeerState.NotRequired;
        fixture.SupplyNodeSend(1);
        Drain(fixture.Node, CancellationToken.None);
        Assert.Equal(0U, fixture.Node.Status().PendingApplicationMessages);
        Assert.Equal(admittedDeadline, peer.Liveness.DeadlineTimestamp);

        // The current admitted route is delivered and its receipt extends the
        // peer's liveness deadline in the same node turn.
        peer.State = MeshPeerState.Admitted;
        fixture.SupplyNodeSend(2);
        Drain(fixture.Node, CancellationToken.None);
        Assert.Equal(1U, fixture.Node.Status().PendingApplicationMessages);
        Assert.True(peer.Liveness.DeadlineTimestamp > admittedDeadline);
        Assert.Equal(1, DrainAndDisposeAllApplicationRecords(fixture.Node));
    }

    [Fact]
    public async Task RawIngress_OnlyARequestCarriesItsOneShotNativeReply()
    {
        await using var fixture = await RawIngressFixture.CreateAsync(4, "reply-capture");
        fixture.SupplyNodeSend(0);
        var request = fixture.SupplyNodeRequest(7);
        Drain(fixture.Node, CancellationToken.None);
        if (fixture.Node.Status().PendingApplicationMessages < 2)
            Drain(fixture.Node, CancellationToken.None);

        using var ready = new MeshReadyBatch();
        fixture.Node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        Assert.Equal(1, ready.Count);
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(2, received.Count);
        Assert.Equal(MeshRecordKind.NodeSend, received[0].Kind);
        Assert.Null(received[0].CaptureReplyRoute());
        Assert.Equal(MeshRecordKind.NodeRequest, received[1].Kind);
        var reply = received[1].CaptureReplyRoute();
        Assert.NotNull(reply);

        // The capability submits one terminal on the native reply and then
        // refuses any second use with the original InvalidState result.
        using var replyPart = Message.From("reply-7");
        Assert.Equal(SubmitResult.Ok, reply([replyPart]));
        Assert.Equal(SubmitResult.InvalidState, reply([replyPart]));
        fixture.AwaitDealerCompletion();
        var replied = await request.Reply.WaitAsync(TimeSpan.FromSeconds(5));
        try
        {
            Assert.True(
                ZLinkServiceWireCodec.TryDecodeReply(
                    replied[0].AsReadOnlySpan(),
                    out var terminal,
                    out _
                )
            );
            Assert.Equal(7UL, terminal.Correlation);
        }
        finally
        {
            foreach (var part in replied)
                part.Dispose();
        }
    }

    private static int DrainAndDisposeAllApplicationRecords(ZLinkManagedMeshNode node)
    {
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        var records = 0;
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            records += received.Count;
        }
        return records;
    }

    // A started-free node whose raw ingress reads one inproc ROUTER fed by an
    // admitted dealer peer. Every receive turn is driven by the test.
    private sealed class RawIngressFixture : IAsyncDisposable
    {
        private const ulong SourceGeneration = 17;
        private IDealerSocket _dealer = null!;
        private IRouterSocket _socket = null!;
        private IPoller _poller = null!;
        private IPoller? _completionPoller;

        internal Systems.Zlink.IContext Context { get; private init; } = null!;
        internal ZLinkMeshPeer Peer { get; private init; } = null!;
        internal ZLinkApplicationJobQueue Queue { get; private init; } = null!;
        internal ZLinkManagedMeshNode Node { get; private init; } = null!;

        internal static async Task<RawIngressFixture> CreateAsync(int capacity, string name)
        {
            var sourceRid = RoutingId.From($"{name}-source");
            var context = Systems.Zlink.Zlink.CreateContext();
            var dealer = context.CreateDealerSocket();
            var socket = context.CreateRouterSocket();
            var endpoint = $"inproc://{name}-{Guid.NewGuid():N}";
            dealer.SetRoutingId(sourceRid);
            socket.Bind(endpoint);
            dealer.Connect(endpoint);
            using (var handshake = Message.From("handshake"))
                dealer.Send().Message(handshake).Submit();
            ulong routeGeneration;
            using (var received = Received.Create())
            {
                Assert.True(socket.Recv(received));
                routeGeneration = received.RouteGeneration;
            }
            var queue = new ZLinkApplicationJobQueue(
                new(ZLinkApplicationJobQueueProfile.Balanced, (ulong)capacity, 1, (ulong)capacity)
            );
            var node = new ZLinkManagedMeshNode(context, name, applicationJobQueue: queue);
            AddAdmittedPeer(node, sourceRid, SourceGeneration, routeGeneration);
            var peer = Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid")[sourceRid];
            SetField(node, "_socket", socket);
            var poller = CreateReceivePoller(socket);
            SetField(node, "_poller", poller);
            await Task.CompletedTask;
            return new RawIngressFixture
            {
                Context = context,
                Peer = peer,
                Queue = queue,
                Node = node,
                _dealer = dealer,
                _socket = socket,
                _poller = poller,
            };
        }

        internal void SupplyNodeSend(int index) =>
            Supply(
                ZLinkServiceWireCodec.EncodeApplication(
                    ServiceWireConstants.Command.NodeSend,
                    0,
                    null,
                    false
                ),
                index
            );

        internal RequestSubmission SupplyNodeRequest(int correlation)
        {
            using var header = Message.From(
                ZLinkServiceWireCodec.EncodeApplication(
                    ServiceWireConstants.Command.NodeRequest,
                    checked((ulong)correlation),
                    null,
                    false
                )
            );
            using var payload = Message.From(
                ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                    new ReadOnlyMemory<byte>[] { new byte[] { checked((byte)correlation) } }
                )
            );
            // A dealer request needs a completion owner; the test waits on it.
            if (_completionPoller is null)
            {
                _completionPoller = Systems.Zlink.Zlink.CreatePoller();
                _completionPoller.Add(_dealer, PollEventFlags.PollCompletion, 1);
            }
            var submission = _dealer
                .Request()
                .Message(header)
                .Message(payload)
                .Timeout(TimeSpan.FromSeconds(30))
                .Async();
            Assert.Equal(SubmitResult.Ok, submission.Result);
            return submission;
        }

        // Blocks until the dealer's completion owner reports a ready completion.
        internal void AwaitDealerCompletion()
        {
            var events = new PollEvent[1];
            Assert.Equal(1, _completionPoller!.Wait(events, TimeSpan.FromSeconds(5)));
        }

        internal void Supply(byte[] head, int index)
        {
            using var header = Message.From(head);
            using var payload = Message.From(
                ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                    new ReadOnlyMemory<byte>[] { new byte[] { checked((byte)index) } }
                )
            );
            var submission = _dealer.Send().Message(header).Message(payload).Async();
            Assert.Equal(SubmitResult.Ok, submission.Result);
            Assert.True(submission.EnsureAcceptedAsync().IsCompletedSuccessfully);
        }

        public async ValueTask DisposeAsync()
        {
            SetField(Node, "_socket", null);
            SetField(Node, "_poller", null);
            await Node.DisposeAsync();
            Queue.Dispose();
            _completionPoller?.Dispose();
            _poller.Dispose();
            await _socket.DisposeAsync();
            await _dealer.DisposeAsync();
            await Context.DisposeAsync();
        }
    }

    private static IPoller CreateReceivePoller(IRouterSocket socket)
    {
        var poller = Systems.Zlink.Zlink.CreatePoller();
        poller.Add(
            socket,
            PollEventFlags.PollIn | PollEventFlags.PollCompletion | PollEventFlags.PollErr,
            ReceiveSocketSlot
        );
        return poller;
    }

    private static bool DrainWhenReadable(
        ZLinkManagedMeshNode node,
        IPoller poller,
        CancellationToken stop
    )
    {
        var events = new PollEvent[1];
        if (
            poller.Wait(events, TimeSpan.Zero) == 0
            || (events[0].Revents & PollEventFlags.PollIn) == 0
        )
            return false;
        Drain(node, stop);
        return true;
    }

    private static void Drain(ZLinkManagedMeshNode node, CancellationToken stop) =>
        node.GetType()
            .GetMethod("DrainRawSocket", BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(node, [stop]);

    private static void SetField(object instance, string name, object? value) =>
        instance
            .GetType()
            .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(instance, value);

    private static T Field<T>(ZLinkManagedMeshNode node, string name) =>
        (T)
            typeof(ZLinkManagedMeshNode)
                .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(node)!;

    private static void AddAdmittedPeer(
        ZLinkManagedMeshNode node,
        RoutingId routingId,
        ulong lifecycleGeneration,
        ulong routeGeneration
    )
    {
        var peer = new ZLinkMeshPeer(1)
        {
            RoutingId = routingId,
            PhysicalRoutingId = routingId,
            LifecycleGeneration = lifecycleGeneration,
            RouteGeneration = routeGeneration,
            Admission = new ZLinkServiceWireCodec.AdmissionRecord(
                "permit-ingress",
                ZLinkServiceSecurityIdentity.Plaintext,
                "inproc://permit-source",
                lifecycleGeneration,
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
            State = MeshPeerState.Admitted,
            Admitted = true,
        };
        Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid").Add(routingId, peer);
    }

    private static void DrainAndDisposeApplicationRecords(
        ZLinkManagedMeshNode node,
        int expectedRecords
    )
    {
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        Assert.Equal(1, ready.Count);
        var claim = ready.TakeClaim(0);
        using (claim)
        using (var received = new MeshReceiveBatch())
        {
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            Assert.Equal(expectedRecords, received.Count);
            Assert.All(
                Enumerable.Range(0, received.Count),
                index => Assert.Equal(MeshRecordKind.NodeSend, received[index].Kind)
            );
        }
    }
}
