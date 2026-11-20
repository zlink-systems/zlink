using System.Diagnostics;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

public sealed class MeshNodeHelloSubmissionTests
{
    private const string MeshName = "hello-submission";
    private static readonly TimeSpan ObservationTimeout = TimeSpan.FromSeconds(2);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task AdmitBeforeFirstHello_RetainsSnapshotUntilRemoteAdmit(bool admitBeforeHello)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"snapshot-peer-{suffix}");
        var endpoint = $"inproc://snapshot-peer-{suffix}";
        peer.SetRoutingId(peerRid);
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.SetRoutingId(RoutingId.From($"snapshot-local-{suffix}"));
        var bytes = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            MeshName,
            endpoint,
            17,
            1,
            new Dictionary<string, uint> { [MeshName] = 100 }
        );
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeRouteAdmission(bytes, out _, out var hello, out _)
        );
        var injected = 0;
        node.SetPeerAdmissionSealGate(() =>
        {
            if (Interlocked.Exchange(ref injected, 1) == 0)
            {
                node.ProcessAdmission(
                    peerRid,
                    node.ObservePeerRoute(peerRid).SelectedGeneration,
                    ServiceWireConstants.Command.Hello,
                    hello,
                    node._socket!.Send(peerRid)
                );
                node.AddChannel("publication");
                if (admitBeforeHello)
                    node.ProcessAdmission(
                        peerRid,
                        node.ObservePeerRoute(peerRid).SelectedGeneration,
                        ServiceWireConstants.Command.Admit,
                        hello,
                        null
                    );
            }
            return false;
        });
        node.Start();
        node.ConnectPeer(endpoint, peerRid);
        var admit = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Admit, admit.Command);
        Assert.Equal(1UL, admit.Record.DescriptorRevision);
        if (!admitBeforeHello)
        {
            var firstHello = ReceiveAdmission(peer, poller);
            Assert.Equal(ServiceWireConstants.Command.Hello, firstHello.Command);
            Assert.Equal(1UL, firstHello.Record.DescriptorRevision);
            Assert.Equal(
                admit.Bytes,
                ZLinkServiceWireCodec.EncodeRouteAdmission(
                    ServiceWireConstants.Command.Admit,
                    firstHello.Record
                )
            );
            node.ProcessAdmission(
                peerRid,
                node.ObservePeerRoute(peerRid).SelectedGeneration,
                ServiceWireConstants.Command.Admit,
                hello,
                null
            );
        }
        var update = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Update, update.Command);
        Assert.Equal(2UL, update.Record.DescriptorRevision);
        node.ProcessAdmission(
            peerRid,
            node.ObservePeerRoute(peerRid).SelectedGeneration,
            ServiceWireConstants.Command.Hello,
            hello,
            node._socket!.Send(peerRid)
        );
        var duplicateResponse = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Admit, duplicateResponse.Command);
        Assert.Equal(2UL, duplicateResponse.Record.DescriptorRevision);
        node.AddChannel("after-admission");
        var laterUpdate = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Update, laterUpdate.Command);
        Assert.Equal(3UL, laterUpdate.Record.DescriptorRevision);
        await CompleteSubmissions(node);
    }

    [Fact]
    public async Task HelloBeforeFirstSubmission_DoesNotSuppressLocalHello()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"first-peer-{suffix}");
        var endpoint = $"inproc://first-peer-{suffix}";
        peer.SetRoutingId(peerRid);
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.SetRoutingId(RoutingId.From($"first-local-{suffix}"));
        var bytes = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            MeshName,
            endpoint,
            17,
            1,
            new Dictionary<string, uint> { [MeshName] = 100 }
        );
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeRouteAdmission(bytes, out _, out var hello, out _)
        );
        var injected = 0;
        node.SetPeerAdmissionSealGate(() =>
        {
            // Management has collected the first HELLO, but has not created its submission.
            if (Interlocked.Exchange(ref injected, 1) == 0)
                node.ProcessAdmission(
                    peerRid,
                    node.ObservePeerRoute(peerRid).SelectedGeneration,
                    ServiceWireConstants.Command.Hello,
                    hello,
                    null
                );
            return false;
        });
        node.Start();
        node.ConnectPeer(endpoint, peerRid);
        Assert.Equal(ServiceWireConstants.Command.Hello, ReceiveAdmission(peer, poller).Command);
        Assert.Equal(1, injected);
        Assert.Equal(0U, node.Status().AdmittedPeerCount);
        await CompleteSubmissions(node);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task HelloOnly_RequiresRemoteAdmitAndKeepsRetrying(bool listenOnly)
    {
        var time = new ControllableTimeProvider();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: time
        );
        using var monitor = node.OpenMonitor();
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"admit-peer-{suffix}");
        var localRid = RoutingId.From($"admit-local-{suffix}");
        var endpoint = $"inproc://admit-peer-{suffix}";
        var localEndpoint = $"inproc://admit-local-{suffix}";
        peer.SetRoutingId(peerRid);
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.SetRoutingId(localRid);
        node.SetBind(localEndpoint);
        node.AddChannel(MeshName);
        node.Start();
        byte[]? firstHelloBytes = null;
        if (listenOnly)
            peer.Connect(localEndpoint);
        else
        {
            node.ConnectPeer(endpoint, peerRid);
            var firstHello = ReceiveAdmission(peer, poller);
            Assert.Equal(ServiceWireConstants.Command.Hello, firstHello.Command);
            firstHelloBytes = firstHello.Bytes;
            await CompleteSubmissions(node);
        }
        var helloBytes = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            MeshName,
            endpoint,
            17,
            1,
            new Dictionary<string, uint> { [MeshName] = 100 }
        );
        using (var hello = Message.From(helloBytes))
            await peer.Send(localRid).Message(hello).Async().EnsureAcceptedAsync();
        Assert.Equal(ServiceWireConstants.Command.Admit, ReceiveAdmission(peer, poller).Command);
        await CompleteSubmissions(node);
        Assert.Equal(0U, node.Status().AdmittedPeerCount);
        Assert.Equal(0UL, monitor.Status().PeerAdmitted);
        Assert.Equal(0UL, node.ObservePeerRoute(peerRid).ReadyGeneration);
        Assert.False(
            await node._lane.RunAsync(() => node._channelSelection.TrySelect(MeshName, out _))
        );
        using (var payload = Message.From("not-ready"u8))
        {
            Assert.Equal(SubmitResult.NotConnected, node.SendToNode(peerRid, [payload]));
            // Submit §5 accepts a request while service admission is pending;
            // its original operation still owns the deadline.
            var completion = new TaskCompletionSource<RequestResult>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            var timeout = TimeSpan.FromMilliseconds(1);
            Assert.Equal(
                SubmitResult.Ok,
                node.RequestToNode(
                    peerRid,
                    [payload],
                    (result, replies) =>
                    {
                        foreach (var reply in replies)
                            reply.Dispose();
                        completion.TrySetResult(result);
                    },
                    timeout
                )
            );
            Assert.False(completion.Task.IsCompleted);
            Assert.Equal(1, node.PendingOperationCount);
            time.AdvanceMonotonic(timeout);
            Assert.Equal(
                RequestResult.TimedOut,
                await completion.Task.WaitAsync(ObservationTimeout)
            );
            Assert.Equal(0, node.PendingOperationCount);
        }
        ManagementTurn(node);
        await CompleteSubmissions(node);
        if (listenOnly)
        {
            var firstHello = ReceiveAdmission(peer, poller);
            Assert.Equal(ServiceWireConstants.Command.Hello, firstHello.Command);
            firstHelloBytes = firstHello.Bytes;
            await CompleteSubmissions(node);
        }
        if (listenOnly)
        {
            // Removing another endpoint intent must not retire this inbound exchange.
            var unrelated = node.ConnectPeer(
                $"inproc://unrelated-{suffix}",
                RoutingId.From($"unrelated-{suffix}")
            );
            node.RemovePeerConnection(unrelated);
            node.PublishDraining();
            await CompleteSubmissions(node);
            // Publication waits for the remote ADMIT; the inbound exchange retains its snapshot.
            using var received = Received.Create();
            while (peer.Recv(received, RecvFlags.DontWait))
                Assert.False(
                    ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                        received.FirstPart().AsSpan(),
                        out _,
                        out _,
                        out _
                    )
                );
        }
        for (var period = 0; period < 2; period++)
        {
            time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval);
            ManagementTurn(node);
            await CompleteSubmissions(node);
            var retry = ReceiveAdmission(peer, poller);
            Assert.Equal(ServiceWireConstants.Command.Hello, retry.Command);
            Assert.Equal(firstHelloBytes, retry.Bytes);
            await CompleteSubmissions(node);
            Assert.Equal(0U, node.Status().AdmittedPeerCount);
        }
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                helloBytes,
                out _,
                out var admission,
                out _
            )
        );
        var generation = node.ObservePeerRoute(peerRid).SelectedGeneration;
        for (var duplicate = 0; duplicate < 2; duplicate++)
            node.ProcessAdmission(
                peerRid,
                generation,
                ServiceWireConstants.Command.Admit,
                admission,
                null
            );
        await CompleteSubmissions(node);
        Assert.Equal(1U, node.Status().AdmittedPeerCount);
        Assert.Equal(1UL, monitor.Status().PeerAdmitted);
        Assert.Equal(generation, node.ObservePeerRoute(peerRid).ReadyGeneration);
        await node._lane.RunAsync(() =>
        {
            Assert.True(node._channelSelection.TrySelect(MeshName, out var selected));
            Assert.Equal(peerRid, selected);
        });
        for (var period = 0; period < 2; period++)
        {
            time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval);
            ManagementTurn(node);
            await CompleteSubmissions(node);
            AssertNoHello(peer);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ManagementBeforeAdmissionDue_DoesNotEnterInboundOperationLane(bool pending)
    {
        var time = new ControllableTimeProvider();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: time
        );
        var lane = node._inboundOperationLane;
        await lane.RunAsync(() =>
        {
            if (pending)
                node._admissionActivityTicks = time.GetTimestamp();
            time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval - TimeSpan.FromTicks(1));
            // Re-entering this lane throws. Every completed rotation therefore
            // proves that management made zero inbound-operation lane calls.
            const int rotations = 64;
            var completed = 0;
            for (; completed < rotations; completed++)
                ManagementTurn(node);
            Assert.Equal(rotations, completed);
        });
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task CrossedHello_AdmitUsesOriginalHelloDescriptor(bool changeDescriptor)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"crossed-peer-{suffix}");
        var localRid = RoutingId.From($"crossed-local-{suffix}");
        var endpoint = $"inproc://crossed-peer-{suffix}";
        peer.SetRoutingId(peerRid);
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.SetRoutingId(localRid);
        node.Start();
        node.ConnectPeer(endpoint, peerRid);
        var hello = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Hello, hello.Command);
        Assert.Equal(1UL, hello.Record.DescriptorRevision);
        await CompleteSubmissions(node);
        if (changeDescriptor)
            node.PublishDraining();

        using (
            var message = Message.From(
                ZLinkServiceWireCodec.EncodeRouteAdmission(
                    ServiceWireConstants.Command.Hello,
                    MeshName,
                    endpoint,
                    17,
                    1,
                    new Dictionary<string, uint> { [MeshName] = 100 }
                )
            )
        )
            await peer.Send(localRid).Message(message).Async().EnsureAcceptedAsync();
        var response = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Admit, response.Command);
        Assert.Equal(hello.Record.DescriptorRevision, response.Record.DescriptorRevision);
        Assert.Equal(hello.Record.DescriptorBytes, response.Record.DescriptorBytes);
        Assert.Equal(
            ZLinkServiceAdmissionDecision.Idempotent,
            ZLinkServiceAdmissionGuard.Evaluate(hello.Record, response.Command, response.Record)
        );

        using (
            var message = Message.From(
                ZLinkServiceWireCodec.EncodeRouteAdmission(
                    ServiceWireConstants.Command.Admit,
                    MeshName,
                    endpoint,
                    17,
                    1,
                    new Dictionary<string, uint> { [MeshName] = 100 }
                )
            )
        )
            await peer.Send(localRid).Message(message).Async().EnsureAcceptedAsync();
        if (changeDescriptor)
        {
            var update = ReceiveAdmission(peer, poller);
            Assert.Equal(ServiceWireConstants.Command.Update, update.Command);
            Assert.Equal(2UL, update.Record.DescriptorRevision);
            Assert.Equal((byte)2, update.Record.RuntimeState);
        }
    }

    [Fact]
    public async Task DescriptorChangedBeforeAdmit_KeepsOriginalHelloAndPublishesUpdateAfterAdmit()
    {
        var scheduler = new GatedTaskScheduler();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler
        );
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"descriptor-peer-{suffix}");
        peer.SetRoutingId(peerRid);
        var endpoint = $"inproc://descriptor-peer-{suffix}";
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.SetRoutingId(RoutingId.From($"descriptor-local-{suffix}"));
        node.Start();
        node.ConnectPeer(endpoint, peerRid);
        try
        {
            await scheduler.Queued.WaitAsync(ObservationTimeout);
            // The first HELLO is encoded, but its submission has not completed.
            Assert.Equal(1UL, node.Status().DescriptorRevision);
            node.PublishDraining();
            Assert.Equal(2UL, node.Status().DescriptorRevision);
            ManagementTurn(node);
        }
        finally
        {
            await scheduler.ReleaseAsync();
        }
        await CompleteSubmissions(node);
        var first = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Hello, first.Command);
        Assert.Equal(1UL, first.Record.DescriptorRevision);
        Assert.Equal((byte)1, first.Record.RuntimeState);

        // A completed send remains owned until admission/route termination. A
        // management turn must not re-encode it with the newer descriptor.
        ManagementTurn(node);
        await CompleteSubmissions(node);
        while (true)
        {
            using var received = Received.Create();
            if (!peer.Recv(received, RecvFlags.DontWait))
                break;
            Assert.Equal(first.Bytes, received.FirstPart().AsSpan().ToArray());
        }

        var admitBytes = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Admit,
            MeshName,
            endpoint,
            17,
            1,
            new Dictionary<string, uint> { [MeshName] = 100 }
        );
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeRouteAdmission(admitBytes, out _, out var admit, out _)
        );
        var generation = await node._lane.RunAsync(() =>
            node._selectedRoutes.GenerationOf(peerRid)
        );
        Assert.NotEqual(0UL, generation);
        node.ProcessAdmission(peerRid, generation, ServiceWireConstants.Command.Admit, admit, null);
        Assert.Equal(1U, node.Status().AdmittedPeerCount);
        await CompleteSubmissions(node);
        var update = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Update, update.Command);
        Assert.Equal(2UL, update.Record.DescriptorRevision);
        Assert.Equal((byte)2, update.Record.RuntimeState);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Admit_SubmitsUpdateOnlyForANewerLocalDescriptor(bool changeDescriptor)
    {
        var scheduler = new GatedTaskScheduler();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler
        );
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"revision-peer-{suffix}");
        var endpoint = $"inproc://revision-peer-{suffix}";
        peer.SetRoutingId(peerRid);
        peer.Bind(endpoint);
        node.SetRoutingId(RoutingId.From($"revision-local-{suffix}"));
        node.Start();
        node.ConnectPeer(endpoint, peerRid);
        try
        {
            await scheduler.Queued.WaitAsync(ObservationTimeout);
            if (changeDescriptor)
                node.PublishDraining();
            var bytes = ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Admit,
                MeshName,
                endpoint,
                17,
                1,
                new Dictionary<string, uint> { [MeshName] = 100 }
            );
            Assert.True(
                ZLinkServiceWireCodec.TryDecodeRouteAdmission(bytes, out _, out var admit, out _)
            );
            // Submissions cannot finish while the scheduler is held. The first
            // HELLO is still queued; only a newer descriptor adds an UPDATE.
            // Observe in the admission turn before liveness can queue a probe.
            await node._lane.RunAsync(async ValueTask () =>
            {
                var generation = node._selectedRoutes.GenerationOf(peerRid);
                Assert.NotEqual(0UL, generation);
                node.ProcessAdmissionCore(
                    peerRid,
                    generation,
                    ServiceWireConstants.Command.Admit,
                    admit,
                    null
                );
                var count = await node._inboundOperationLane.RunAsync(() =>
                    node._inboundOperations.Count
                );
                Assert.Equal(changeDescriptor ? 2 : 1, count);
            });
            Assert.Equal(1U, node.Status().AdmittedPeerCount);
        }
        finally
        {
            await scheduler.ReleaseAsync();
        }
        await CompleteSubmissions(node);
    }

    [Theory]
    [InlineData(true, false)]
    [InlineData(false, false)]
    [InlineData(true, true)]
    public async Task UnansweredHello_RetriesOnlyOnLivenessPeriodsUntilAdmit(
        bool receiveAdmit,
        bool admitDuringManagement
    )
    {
        var time = new ControllableTimeProvider();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: time
        );
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"cadence-peer-{suffix}");
        var endpoint = $"inproc://cadence-peer-{suffix}";
        peer.SetRoutingId(peerRid);
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.SetRoutingId(RoutingId.From($"cadence-local-{suffix}"));
        node.Start();
        var intent = node.ConnectPeer(endpoint, peerRid);
        var first = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Hello, first.Command);
        await CompleteSubmissions(node);
        node.PublishDraining();

        const int periods = 3;
        var submissions = 1;
        for (var period = 0; period < periods; period++)
        {
            time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval - TimeSpan.FromTicks(1));
            ManagementTurn(node);
            await CompleteSubmissions(node);
            AssertNoHello(peer);
            Assert.Equal(TimeSpan.FromTicks(time.GetTimestamp() + 1), ReadWaitDeadline(node));
            time.AdvanceMonotonic(TimeSpan.FromTicks(1));
            ManagementTurn(node);
            await CompleteSubmissions(node);
            var retry = ReceiveAdmission(peer, poller);
            Assert.Equal(ServiceWireConstants.Command.Hello, retry.Command);
            Assert.Equal(first.Bytes, retry.Bytes);
            submissions++;
            ManagementTurn(node);
            await CompleteSubmissions(node);
            AssertNoHello(peer);
            Assert.True(ReadWaitDeadline(node) > TimeSpan.FromTicks(time.GetTimestamp()));
        }
        Assert.Equal(periods + 1, submissions);

        if (receiveAdmit)
        {
            var admitBytes = ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Admit,
                MeshName,
                endpoint,
                17,
                1,
                new Dictionary<string, uint> { [MeshName] = 100 }
            );
            Assert.True(
                ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                    admitBytes,
                    out _,
                    out var admit,
                    out _
                )
            );
            var generation = await node._lane.RunAsync(() =>
                node._selectedRoutes.GenerationOf(peerRid)
            );
            void Admit() =>
                node.ProcessAdmission(
                    peerRid,
                    generation,
                    ServiceWireConstants.Command.Admit,
                    admit,
                    null
                );
            if (admitDuringManagement)
            {
                var admitted = 0;
                node.SetPeerAdmissionSealGate(() =>
                {
                    // The gate is observed after the management turn collected its intents.
                    if (Interlocked.Exchange(ref admitted, 1) == 0)
                        Admit();
                    return false;
                });
                time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval);
                ManagementTurn(node);
            }
            else
                Admit();
            await CompleteSubmissions(node);
            Assert.Equal(
                ServiceWireConstants.Command.Update,
                ReceiveAdmission(peer, poller).Command
            );
        }
        else
            node.RemovePeerConnection(intent);
        Assert.Equal(-1, node._admissionActivityTicks);
        for (var period = 0; period < periods; period++)
        {
            time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval);
            ManagementTurn(node);
            await CompleteSubmissions(node);
            AssertNoHello(peer);
            Assert.True(ReadWaitDeadline(node) > TimeSpan.FromTicks(time.GetTimestamp()));
        }
        Assert.Equal(0, time.ActiveTimerCount);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task Reject_ClearsAdmissionAndRetriesHelloUntilReadmitted(
        bool admitted,
        bool knownRoutingId
    )
    {
        var time = new ControllableTimeProvider();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: time
        );
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"reject-peer-{suffix}");
        var endpoint = $"inproc://reject-peer-{suffix}";
        peer.SetRoutingId(peerRid);
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.AddChannel(MeshName);
        node.SetRoutingId(RoutingId.From($"reject-local-{suffix}"));
        node.Start();
        var intent = node.ConnectPeer(endpoint, knownRoutingId ? peerRid : null);
        var firstHello = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Hello, firstHello.Command);
        await CompleteSubmissions(node);
        var generation = node.ObservePeerRoute(peerRid).SelectedGeneration;
        var bytes = ZLinkServiceWireCodec.EncodeRouteAdmission(
            ServiceWireConstants.Command.Hello,
            MeshName,
            endpoint,
            17,
            1,
            new Dictionary<string, uint> { [MeshName] = 100 }
        );
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeRouteAdmission(bytes, out _, out var descriptor, out _)
        );
        node.ProcessAdmission(
            peerRid,
            generation,
            ServiceWireConstants.Command.Hello,
            descriptor,
            node._socket!.Send(peerRid)
        );
        await CompleteSubmissions(node);
        Assert.Equal(ServiceWireConstants.Command.Admit, ReceiveAdmission(peer, poller).Command);
        if (admitted)
            node.ProcessAdmission(
                peerRid,
                generation,
                ServiceWireConstants.Command.Admit,
                descriptor,
                null
            );
        Assert.Equal(admitted, node.ObservePeerRoute(peerRid).ReadyGeneration != 0);
        Assert.Equal(
            admitted,
            await node._lane.RunAsync(() => node._peersByRid[peerRid].Liveness is not null)
        );

        var reject = ServiceWireCodec.EncodeReject3Header(
            new ServiceWireCodec.Reject3(0, ServiceWireCodec.RejectReason.RuntimeNotServing),
            ServiceWireCodec.DecodeContext.Empty
        );
        Assert.False(ProcessReject(node, peerRid, generation, reject[..^1]));
        Assert.False(ProcessReject(node, peerRid, generation, reject, extraPart: true));
        Assert.Equal(admitted, node.ObservePeerRoute(peerRid).ReadyGeneration != 0);
        Assert.True(ProcessReject(node, peerRid, generation + 1, reject));
        Assert.Equal(admitted, node.ObservePeerRoute(peerRid).ReadyGeneration != 0);
        Assert.True(ProcessReject(node, peerRid, generation, reject));
        Assert.Equal(0UL, node.ObservePeerRoute(peerRid).ReadyGeneration);
        await node._lane.RunAsync(() =>
        {
            var current = node._peersByRid[peerRid];
            Assert.False(current.Admitted);
            Assert.Null(current.Liveness);
            Assert.Equal(MeshPeerState.Connecting, current.State);
            Assert.Equal(generation, current.RouteGeneration);
        });
        Assert.Equal(generation, node.ObservePeerRoute(peerRid).SelectedGeneration);
        Assert.Equal(intent, Assert.Single(node.Peers()).ConnectionIntentId);
        Assert.Single(peer.RoutesSnapshot());
        // An already pending exchange keeps its cadence. An admitted route starts
        // the ordinary unanswered-HELLO exchange again on the management turn.
        ManagementTurn(node);
        await CompleteSubmissions(node);
        if (admitted)
            Assert.Equal(
                ServiceWireConstants.Command.Hello,
                ReceiveAdmission(peer, poller).Command
            );
        else
            AssertNoHello(peer);
        time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval - TimeSpan.FromTicks(1));
        ManagementTurn(node);
        await CompleteSubmissions(node);
        AssertNoHello(peer);
        time.AdvanceMonotonic(TimeSpan.FromTicks(1));
        ManagementTurn(node);
        await CompleteSubmissions(node);
        var retry = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Hello, retry.Command);
        if (!admitted)
            Assert.Equal(firstHello.Bytes, retry.Bytes);
        // Repeated REJECT is idempotent and does not restart the existing cadence.
        Assert.True(ProcessReject(node, peerRid, generation, reject));
        ManagementTurn(node);
        await CompleteSubmissions(node);
        AssertNoHello(peer);
        node.ProcessAdmission(
            peerRid,
            generation,
            ServiceWireConstants.Command.Admit,
            descriptor,
            null
        );
        Assert.Equal(generation, node.ObservePeerRoute(peerRid).ReadyGeneration);
        time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval);
        ManagementTurn(node);
        await CompleteSubmissions(node);
        AssertNoHello(peer);
        Assert.Equal(0, time.ActiveTimerCount);
        // Removing the original intent still works after rejection and recovery.
        node.RemovePeerConnection(intent);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Reject_InboundRouteRetainsPeerWithoutCreatingConnectionIntent(bool hasPeer)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var rid = RoutingId.From("reject-inbound");
        var current = new ZLinkMeshPeer(1)
        {
            RoutingId = rid,
            PhysicalRoutingId = rid,
            RouteGeneration = 7,
            State = MeshPeerState.Admitted,
            Admitted = true,
            Liveness = new ZLinkServiceLiveness(Stopwatch.GetTimestamp()),
        };
        var peers = new Dictionary<RoutingId, ZLinkMeshPeer>();
        if (hasPeer)
            peers.Add(rid, current);
        await using var node = new ZLinkManagedMeshNode(context, MeshName, peersByRid: peers);
        var reject = ServiceWireCodec.EncodeReject3Header(
            new ServiceWireCodec.Reject3(0, ServiceWireCodec.RejectReason.RuntimeNotServing),
            ServiceWireCodec.DecodeContext.Empty
        );
        Assert.True(ProcessReject(node, rid, 7, reject));
        await node._lane.RunAsync(() =>
        {
            Assert.Equal(hasPeer ? 1 : 0, peers.Count);
            if (hasPeer)
            {
                Assert.Same(current, peers[rid]);
                Assert.False(current.Admitted);
                Assert.Null(current.Liveness);
                Assert.Equal(MeshPeerState.Connecting, current.State);
                Assert.Equal(7UL, current.RouteGeneration);
            }
        });
        Assert.Equal(hasPeer ? 1 : 0, node.Peers().Length);
    }

    private static bool ProcessReject(
        ZLinkManagedMeshNode node,
        RoutingId peerRid,
        ulong generation,
        byte[] head,
        bool extraPart = false
    )
    {
        using var message = Message.From(head);
        return node.ProcessInfrastructureControl(
            peerRid,
            generation,
            17,
            null,
            extraPart ? [message, message] : [message],
            head,
            () => throw new Xunit.Sdk.XunitException("REJECT must not capture a reply."),
            () => throw new Xunit.Sdk.XunitException("REJECT must not capture a send.")
        );
    }

    private static TimeSpan ReadWaitDeadline(ZLinkManagedMeshNode node) =>
        node.ReadOperationWaitDeadline();

    private static void AssertNoHello(IRouterSocket peer)
    {
        using var received = Received.Create();
        while (peer.Recv(received, RecvFlags.DontWait))
        {
            if (
                ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                    received.FirstPart().AsSpan(),
                    out var command,
                    out _,
                    out _
                )
            )
                Assert.NotEqual(ServiceWireConstants.Command.Hello, command);
        }
    }

    private static void ManagementTurn(ZLinkManagedMeshNode node) =>
        node.ProcessInfrastructure(Stopwatch.GetTimestamp());

    private static async Task CompleteSubmissions(ZLinkManagedMeshNode node)
    {
        var tasks = await node._inboundOperationLane.RunAsync(() =>
            node._inboundOperations.ToArray()
        );
        await Task.WhenAll(tasks).WaitAsync(ObservationTimeout);
    }

    private static (
        ServiceWireConstants.Command Command,
        ZLinkServiceWireCodec.AdmissionRecord Record,
        byte[] Bytes
    ) ReceiveAdmission(IRouterSocket peer, IPoller poller)
    {
        var events = new PollEvent[1];
        var started = Stopwatch.GetTimestamp();
        while (Stopwatch.GetElapsedTime(started) < ObservationTimeout)
        {
            var remaining = ObservationTimeout - Stopwatch.GetElapsedTime(started);
            if (remaining <= TimeSpan.Zero || poller.Wait(events, remaining) == 0)
                break;
            using var received = Received.Create();
            if (
                peer.Recv(received, RecvFlags.DontWait)
                && ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                    received.FirstPart().AsSpan(),
                    out var command,
                    out var record,
                    out _
                )
            )
                return (command, record, received.FirstPart().AsSpan().ToArray());
        }
        throw new Xunit.Sdk.XunitException(
            "No admission descriptor was submitted within the observation bound."
        );
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task UnansweredHello_ReconnectPreservesObservedEpochCadence(bool knownRoutingId)
    {
        var time = new ControllableTimeProvider();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var peer = context.CreateRouterSocket();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: time
        );
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        using var releaseOwner = new ManualResetEventSlim();
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"hello-peer-{suffix}");
        peer.SetRoutingId(peerRid);
        var endpoint = $"inproc://hello-peer-{suffix}";
        peer.Bind(endpoint);
        poller.Add(peer, PollEventFlags.PollIn, 1);
        node.SetRoutingId(RoutingId.From($"hello-local-{suffix}"));
        node.SetBind($"inproc://hello-local-{suffix}");
        node.AddChannel(MeshName);
        node.Start();
        var intent = node.ConnectPeer(endpoint, knownRoutingId ? peerRid : null);
        var first = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Hello, first.Command);
        await CompleteSubmissions(node);
        var route = Assert.Single(peer.RoutesSnapshot());
        var observed = node.ObservePeerRoute(peerRid).SelectedGeneration;
        Assert.NotEqual(0UL, observed);
        ManagementTurn(node);
        await CompleteSubmissions(node);
        AssertNoHello(peer);

        var ownerHeld = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var held = 0;
        node.SetPeerAdmissionSealGate(() =>
        {
            if (Interlocked.Exchange(ref held, 1) == 0)
            {
                ownerHeld.TrySetResult();
                releaseOwner.Wait();
            }
            return false;
        });
        node.SignalReceiveOwner();
        try
        {
            await ownerHeld.Task.WaitAsync(ObservationTimeout);
            node.RemovePeerConnection(intent);
            node.ConnectPeer(endpoint, knownRoutingId ? peerRid : null);
            // Core can select the replacement before its sole observer resumes.
            // This HELLO belongs to the still-observed epoch, not the next one.
            ManagementTurn(node);
            await CompleteSubmissions(node);
            var beforeObservation = ReceiveAdmission(peer, poller);
            Assert.Equal(ServiceWireConstants.Command.Hello, beforeObservation.Command);
            Assert.Equal(first.Bytes, beforeObservation.Bytes);
            Assert.Equal(observed, node.ObservePeerRoute(peerRid).SelectedGeneration);
            Assert.NotEqual(
                route.RouteGeneration,
                Assert.Single(peer.RoutesSnapshot()).RouteGeneration
            );
            node.PublishDraining();
        }
        finally
        {
            releaseOwner.Set();
        }
        var replacement = ReceiveAdmission(peer, poller);
        Assert.Equal(ServiceWireConstants.Command.Hello, replacement.Command);
        Assert.NotEqual(observed, node.ObservePeerRoute(peerRid).SelectedGeneration);
        Assert.Equal(first.Record.DescriptorRevision + 1, replacement.Record.DescriptorRevision);
        await CompleteSubmissions(node);
        for (var rotation = 0; rotation < 64; rotation++)
            ManagementTurn(node);
        await CompleteSubmissions(node);
        AssertNoHello(peer);
        time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval - TimeSpan.FromTicks(1));
        ManagementTurn(node);
        await CompleteSubmissions(node);
        AssertNoHello(peer);
        time.AdvanceMonotonic(TimeSpan.FromTicks(1));
        ManagementTurn(node);
        await CompleteSubmissions(node);
        Assert.Equal(replacement.Bytes, ReceiveAdmission(peer, poller).Bytes);
        ManagementTurn(node);
        await CompleteSubmissions(node);
        AssertNoHello(peer);
    }
}
