using System.Reflection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshPermitHandoffTests
{
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
        using (var received = Received.Create())
            Assert.True(socket.Recv(received));

        using var queue = new ZLinkApplicationJobQueue(new(
            ZLinkApplicationJobQueueProfile.Balanced, capacity, 1, capacity));
        await using var node = new ZLinkManagedMeshNode(
            context, "permit-ingress", applicationJobQueue: queue);
        using var stop = new CancellationTokenSource();
        AddAdmittedPeer(node, sourceRid, sourceGeneration);
        SetField(node, "_socket", socket);

        try
        {
            for (var index = 0; index < suppliedRecords; index++)
            {
                using var header = Message.From(ZLinkServiceWireCodec.EncodeApplication(
                    ServiceWireConstants.Command.NodeSend, 0, null, false));
                using var payload = Message.From(ZLinkApplicationPayloadEnvelopeCodec
                    .EncodeFrameworkMultipart(
                        new ReadOnlyMemory<byte>[] { new byte[] { checked((byte)index) } }));
                var submission = dealer.Send()
                    .Message(header)
                    .Message(payload)
                    .Async();
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
            Drain(node, stop.Token);
            Drain(node, stop.Token);
            Drain(node, stop.Token);
            Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);
            Assert.Equal((uint)capacity, node.Status().PendingApplicationMessages);
            using var retained = Received.Create();
            Assert.True(socket.Recv(retained, RecvFlags.DontWait));
            Assert.Equal(ReceivedMessageType.Raw, retained.MessageType);
            Assert.Equal(ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                    new ReadOnlyMemory<byte>[] { new byte[] { capacity } }),
                retained.Parts[1].ToArray());

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
        using var queue = new ZLinkApplicationJobQueue(new(
            ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1));
        await using var node = new ZLinkManagedMeshNode(
            context, "permit-handoff", applicationJobQueue: queue);
        using var stop = new CancellationTokenSource();
        SetField(node, "_socket", socket);

        // Arrange the producer's published state. Its reservation can be
        // consumed before the producer returns; only that consumer owns the
        // successful handoff flag, including registration of a successor.
        var reservation = await queue.AcquireAsync(stop.Token);
        SetField(node, "_reservedRawApplicationAdmission", reservation);
        SetField(node, "_rawApplicationAdmissionWaitActive", 1);
        ZLinkApplicationJobQueueLease? held = null;
        try
        {
            Drain(node, stop.Token);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
            held = await queue.AcquireAsync(stop.Token);

            Drain(node, stop.Token);
            Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);
        }
        finally
        {
            stop.Cancel();
            held?.Dispose();
            SetField(node, "_socket", null);
        }
    }

    private static void Drain(ZLinkManagedMeshNode node, CancellationToken stop) =>
        node.GetType().GetMethod("DrainRawSocket",
                BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(node, [stop, new ZLinkApplicationJobQueueLease?[64]]);

    private static void SetField(object instance, string name, object? value) =>
        instance.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(instance, value);

    private static T Field<T>(ZLinkManagedMeshNode node, string name) =>
        (T)typeof(ZLinkManagedMeshNode)
            .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .GetValue(node)!;

    private static void AddAdmittedPeer(ZLinkManagedMeshNode node,
        RoutingId routingId, ulong lifecycleGeneration)
    {
        var peer = new ZLinkMeshPeer(1, "inproc://permit-source", routingId,
            string.Empty, ZLinkServiceConnectionDirection.Inbound)
        {
            RoutingId = routingId,
            PhysicalRoutingId = routingId,
            LifecycleGeneration = lifecycleGeneration,
            State = MeshPeerState.Admitted,
            Admitted = true
        };
        Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid")
            .Add(routingId, peer);
    }

    private static void DrainAndDisposeApplicationRecords(
        ZLinkManagedMeshNode node, int expectedRecords)
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
            Assert.All(Enumerable.Range(0, received.Count),
                index => Assert.Equal(MeshRecordKind.NodeSend, received[index].Kind));
        }
    }
}
