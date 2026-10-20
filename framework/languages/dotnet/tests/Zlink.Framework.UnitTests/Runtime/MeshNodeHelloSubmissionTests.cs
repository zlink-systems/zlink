using System.Diagnostics;
using System.Reflection;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

public sealed class MeshNodeHelloSubmissionTests
{
    private const string MeshName = "hello-submission";
    private static readonly TimeSpan ObservationTimeout = TimeSpan.FromSeconds(2);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ManagementBeforeAdmissionDue_DoesNotEnterInboundOperationLane(bool pending)
    {
        var time = new ControllableTimeProvider();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName, deadlineTimeProvider: time);
        var lane = Field<ZLinkStateLane>(node, "_inboundOperationLane");
        await lane.RunAsync(() =>
        {
            if (pending)
                typeof(ZLinkManagedMeshNode)
                    .GetField("_admissionActivityTicks", BindingFlags.Instance | BindingFlags.NonPublic)!
                    .SetValue(node, time.GetTimestamp());
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
        var generation = await Field<ZLinkStateLane>(node, "_lane")
            .RunAsync(() =>
                Field<ZLinkMeshSelectedRoutes>(node, "_selectedRoutes").GenerationOf(peerRid)
            );
        Assert.NotEqual(0UL, generation);
        Invoke(
            node,
            "ProcessAdmission",
            peerRid,
            generation,
            ServiceWireConstants.Command.Admit,
            admit,
            null
        );
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
            await Field<ZLinkStateLane>(node, "_lane")
                .RunAsync(async ValueTask () =>
                {
                    var generation = Field<ZLinkMeshSelectedRoutes>(node, "_selectedRoutes")
                        .GenerationOf(peerRid);
                    Assert.NotEqual(0UL, generation);
                    Invoke(
                        node,
                        "ProcessAdmissionCore",
                        peerRid,
                        generation,
                        ServiceWireConstants.Command.Admit,
                        admit,
                        null
                    );
                    var count = await Field<ZLinkStateLane>(node, "_inboundOperationLane")
                        .RunAsync(() => Field<HashSet<Task>>(node, "_inboundOperations").Count);
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
            var generation = await Field<ZLinkStateLane>(node, "_lane")
                .RunAsync(() =>
                    Field<ZLinkMeshSelectedRoutes>(node, "_selectedRoutes").GenerationOf(peerRid)
                );
            void Admit() =>
                Invoke(
                    node,
                    "ProcessAdmission",
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
        Assert.Equal(-1, Field<long>(node, "_admissionActivityTicks"));
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

    private static TimeSpan ReadWaitDeadline(ZLinkManagedMeshNode node) =>
        (TimeSpan)
            typeof(ZLinkManagedMeshNode)
                .GetMethod(
                    "ReadOperationWaitDeadline",
                    BindingFlags.Instance | BindingFlags.NonPublic
                )!
                .Invoke(node, null)!;

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
        Invoke(node, "ProcessInfrastructure", Stopwatch.GetTimestamp());

    private static async Task CompleteSubmissions(ZLinkManagedMeshNode node)
    {
        var tasks = await Field<ZLinkStateLane>(node, "_inboundOperationLane")
            .RunAsync(() => Field<HashSet<Task>>(node, "_inboundOperations").ToArray());
        await Task.WhenAll(tasks).WaitAsync(ObservationTimeout);
    }

    private static T Field<T>(ZLinkManagedMeshNode node, string name) =>
        (T)
            typeof(ZLinkManagedMeshNode)
                .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(node)!;

    private static void Invoke(
        ZLinkManagedMeshNode node,
        string name,
        params object?[] arguments
    ) =>
        typeof(ZLinkManagedMeshNode)
            .GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(node, arguments);

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
        await using var node = new ZLinkManagedMeshNode(context, MeshName, deadlineTimeProvider: time);
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

        var ownerHeld = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
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
        Invoke(node, "SignalReceiveOwner");
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
            Assert.NotEqual(route.RouteGeneration, Assert.Single(peer.RoutesSnapshot()).RouteGeneration);
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
