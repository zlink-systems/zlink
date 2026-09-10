using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshMailboxReadinessTests
{
    [Fact]
    public async Task EnqueueAccountingCommitsBeforeACompetingDequeue()
    {
        var transitions = new ConcurrentQueue<long>();
        var count = 0L;
        using var consumerStarted = new ManualResetEventSlim();
        Task? consumer = null;
        ZLinkMeshNodeOwnedMailbox? mailbox = null;
        mailbox = new ZLinkMeshNodeOwnedMailbox(
            _ =>
            {
                // Accounting belongs to the committing mailbox turn. The
                // competing receiver must not publish -1 while it is pending.
                Assert.NotNull(ZLinkStateLane.Current);
                using (ExecutionContext.SuppressFlow())
                    consumer = Task.Run(() =>
                    {
                        consumerStarted.Set();
                        using var batch = new MeshReceiveBatch();
                        Assert.True(mailbox!.TryDequeue(batch, out var dequeued));
                        dequeued.Dispose();
                    });
                Assert.True(consumerStarted.Wait(TimeSpan.FromSeconds(3)));
                transitions.Enqueue(Interlocked.Increment(ref count));
            },
            _ => transitions.Enqueue(Interlocked.Decrement(ref count)));

        Assert.True(mailbox.TryEnqueue(NewRecord(), 1, 1024));
        await consumer!.WaitAsync(TimeSpan.FromSeconds(3));

        Assert.Equal(new long[] { 1, 0 }, transitions.ToArray());
        Assert.Equal(0, Volatile.Read(ref count));
        Assert.False(mailbox.HasRecords);
        mailbox.Dispose();
    }

    [Fact]
    public void RejectedEnqueueAndDisposalPreserveAccountingAndPayloadOwnership()
    {
        var count = 0L;
        var payload = new PayloadOwner();
        var mailbox = new ZLinkMeshNodeOwnedMailbox(
            _ => Interlocked.Increment(ref count),
            _ =>
            {
                Assert.NotNull(ZLinkStateLane.Current);
                Interlocked.Decrement(ref count);
            });
        Assert.True(mailbox.TryEnqueue(NewRecord(payload), 1, 1024));
        using var rejected = NewRecord();
        Assert.False(mailbox.TryEnqueue(rejected, 1, 1024));
        Assert.Equal(1, Volatile.Read(ref count));

        mailbox.Dispose();
        mailbox.Dispose();

        Assert.Equal(0, Volatile.Read(ref count));
        Assert.Equal(1, payload.DisposeCount);
        Assert.Null(payload.LaneDuringDispose);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ReadinessRearmsForResidueAndDoesNotSignalAnEmptyNode(
        bool installAfterEnqueue)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "mesh");
        node.SetRoutingId(RoutingId.From("mailbox-readiness"));
        var actor = node.CreateActor("mailbox-readiness-actor");
        Drain(node);
        var notifications = 0;
        MeshReadyDomains OnReady(MeshReadyDomains domains)
        {
            Interlocked.Increment(ref notifications);
            return domains;
        }

        if (!installAfterEnqueue)
        {
            node.SetReadyHandler(OnReady);
            Assert.Equal(0, notifications);
        }
        using var payload = Message.From(new byte[] { 1 });
        Assert.Equal(SubmitResult.Ok, node.SendToActor(actor, [payload]));
        if (installAfterEnqueue)
            node.SetReadyHandler(OnReady);
        Assert.Equal(1, notifications);
        Assert.Equal(SubmitResult.Ok, node.SendToActor(actor, [payload]));
        Assert.Equal(1, notifications);

        // Releasing an undrained claim must restore readiness for its residue.
        using (var ready = new MeshReadyBatch())
        {
            node.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
            Assert.Equal(1, ready.Count);
        }
        Assert.Equal(2, notifications);
        Assert.Equal(2, Drain(node));
        node.SetReadyHandler(OnReady);
        Assert.Equal(2, notifications);

        Assert.Equal(SubmitResult.Ok, node.SendToActor(actor, [payload]));
        Assert.Equal(3, notifications);
        Assert.Equal(1, Drain(node));
    }

    [Fact]
    public async Task OneClaimDrains64RecordsAndLeavesTheNextRecordQueued()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "batch");
        var rid = RoutingId.From("batch-owner");
        node.SetRoutingId(rid);
        node.Start();
        using var payload = Message.From(new byte[] { 48 });
        for (var index = 0; index < 65; index++)
            Assert.Equal(SubmitResult.Ok, node.SendToNode(rid, [payload]));

        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
        Assert.Equal(1, ready.Count);
        using var claim = ready.TakeClaim(0);
        using var received = new MeshReceiveBatch();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(64, received.Count);
        received.Reset();
        Assert.True(claim.Receive(received, RecvFlags.DontWait));
        Assert.Equal(1, received.Count);
        received.Reset();
        Assert.False(claim.Receive(received, RecvFlags.DontWait));
    }

    private static ZLinkMeshQueuedRecord NewRecord(IDisposable? payloadOwner = null) =>
        new(MeshReceiveRecord.CompletionFailure(default, RequestResult.Ok), [],
            applicationPayloadBytes: 0, payloadOwner: payloadOwner);

    private static int Drain(ZLinkManagedMeshNode node)
    {
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
        var count = 0;
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            while (claim.Receive(received, RecvFlags.DontWait))
            {
                count += received.Count;
                received.Reset();
            }
        }
        return count;
    }

    private sealed class PayloadOwner : IDisposable
    {
        internal int DisposeCount { get; private set; }
        internal ZLinkStateLane? LaneDuringDispose { get; private set; }

        public void Dispose()
        {
            DisposeCount++;
            LaneDuringDispose = ZLinkStateLane.Current;
        }
    }
}
