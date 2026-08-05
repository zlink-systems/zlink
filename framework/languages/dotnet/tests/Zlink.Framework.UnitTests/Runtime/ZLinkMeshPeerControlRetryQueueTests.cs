using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ZLinkMeshPeerControlRetryQueueTests
{
    [Fact]
    public void BackpressureRetainsTheWholeRecordUntilAFullRetrySucceeds()
    {
        var queue = new ZLinkMeshPeerControlRetryQueue(
            maximumRecords: 4,
            maximumBytes: 64);
        var target = RoutingId.From("peer");
        var payload = new byte[] { 1, 2, 3, 4 };
        var attempts = new List<byte[]>();

        Assert.True(queue.TryRemember(
            target,
            7,
            ServiceWireConstants.Command.Hello,
            payload));
        Assert.Equal(1, queue.Count);
        Assert.Equal(payload.Length, queue.Bytes);

        Assert.Equal(
            1,
            queue.Flush((_, _, record) =>
            {
                attempts.Add(record.ToArray());
                return ZLinkMeshPeerControlRetryResult.Backpressured;
            }));
        Assert.Equal(1, queue.Count);

        Assert.Equal(
            1,
            queue.Flush((_, _, record) =>
            {
                attempts.Add(record.ToArray());
                return ZLinkMeshPeerControlRetryResult.Accepted;
            }));
        Assert.Equal(0, queue.Count);
        Assert.Equal(0, queue.Bytes);
        Assert.Equal(2, attempts.Count);
        Assert.Equal(payload, attempts[0]);
        Assert.Equal(payload, attempts[1]);
    }

    [Fact]
    public void NewerDescriptorReplacesAnOlderRetryForTheSameCommand()
    {
        var queue = new ZLinkMeshPeerControlRetryQueue();
        var target = RoutingId.From("peer");
        var newer = new byte[] { 9, 8, 7 };

        Assert.True(queue.TryRemember(
            target,
            11,
            ServiceWireConstants.Command.Update,
            new byte[] { 1 }));
        Assert.True(queue.TryRemember(
            target,
            11,
            ServiceWireConstants.Command.Update,
            newer));

        byte[]? submitted = null;
        queue.Flush((_, _, record) =>
        {
            submitted = record.ToArray();
            return ZLinkMeshPeerControlRetryResult.Accepted;
        });

        Assert.Equal(newer, submitted);
        Assert.Equal(0, queue.Count);
    }

    [Fact]
    public void AFailedTargetDoesNotPreventAnotherControlFromProgressing()
    {
        var queue = new ZLinkMeshPeerControlRetryQueue();
        var blocked = RoutingId.From("blocked");
        var ready = RoutingId.From("ready");
        Assert.True(queue.TryRemember(
            blocked,
            12,
            ServiceWireConstants.Command.Hello,
            new byte[] { 1 }));
        Assert.True(queue.TryRemember(
            ready,
            13,
            ServiceWireConstants.Command.LivenessAck,
            new byte[] { 2 }));

        var submitted = new List<RoutingId>();
        queue.Flush((target, _, _) =>
        {
            submitted.Add(target);
            return target == ready
                ? ZLinkMeshPeerControlRetryResult.Accepted
                : ZLinkMeshPeerControlRetryResult.Backpressured;
        });

        Assert.Contains(ready, submitted);
        Assert.Equal(1, queue.Count);
    }

    [Fact]
    public async Task ReplacementDuringFlushRemainsQueuedForTheNewConnectionEpoch()
    {
        var queue = new ZLinkMeshPeerControlRetryQueue();
        var target = RoutingId.From("peer");
        var started = new ManualResetEventSlim();
        var release = new ManualResetEventSlim();

        Assert.True(queue.TryRemember(
            target,
            21,
            ServiceWireConstants.Command.Update,
            new byte[] { 1 }));

        var flush = Task.Run(() => queue.Flush((_, _, _) =>
        {
            started.Set();
            release.Wait();
            return ZLinkMeshPeerControlRetryResult.Accepted;
        }));
        Assert.True(started.Wait(TimeSpan.FromSeconds(5)));

        Assert.True(queue.TryRemember(
            target,
            21,
            ServiceWireConstants.Command.Update,
            new byte[] { 9, 8 }));
        release.Set();
        await flush;

        Assert.Equal(1, queue.Count);
        byte[]? replacement = null;
        ulong generation = 0;
        queue.Flush((_, actualGeneration, payload) =>
        {
            generation = actualGeneration;
            replacement = payload.ToArray();
            return ZLinkMeshPeerControlRetryResult.Accepted;
        });

        Assert.Equal((ulong)21, generation);
        Assert.Equal(new byte[] { 9, 8 }, replacement);
        Assert.Equal(0, queue.Count);
    }

    [Fact]
    public void StaleConnectionEpochIsDiscardedWithoutRetryingIntoTheReplacement()
    {
        var queue = new ZLinkMeshPeerControlRetryQueue();
        var target = RoutingId.From("peer");
        Assert.True(queue.TryRemember(
            target,
            31,
            ServiceWireConstants.Command.LivenessAck,
            new byte[] { 3 }));

        ulong attemptedGeneration = 0;
        queue.Flush((_, generation, _) =>
        {
            attemptedGeneration = generation;
            return ZLinkMeshPeerControlRetryResult.Stale;
        });

        Assert.Equal((ulong)31, attemptedGeneration);
        Assert.Equal(0, queue.Count);
    }

    [Fact]
    public void AcceptedOlderIntentDoesNotRemoveAQueuedNewerIntent()
    {
        var queue = new ZLinkMeshPeerControlRetryQueue();
        var target = RoutingId.From("peer");
        var olderVersion = queue.NextIntentVersion();
        var newerVersion = queue.NextIntentVersion();

        Assert.True(queue.TryRemember(
            target,
            41,
            ServiceWireConstants.Command.Update,
            new byte[] { 1 },
            olderVersion));
        Assert.True(queue.TryRemember(
            target,
            41,
            ServiceWireConstants.Command.Update,
            new byte[] { 9 },
            newerVersion));

        queue.RemoveUpTo(
            target,
            41,
            ServiceWireConstants.Command.Update,
            olderVersion);

        byte[]? retained = null;
        queue.Flush((_, _, payload) =>
        {
            retained = payload.ToArray();
            return ZLinkMeshPeerControlRetryResult.Accepted;
        });

        Assert.Equal(new byte[] { 9 }, retained);
        Assert.Equal(0, queue.Count);
    }

    [Fact]
    public void RetryCapacityRejectionIsObservable()
    {
        var queue = new ZLinkMeshPeerControlRetryQueue(
            maximumRecords: 1,
            maximumBytes: 8);

        Assert.True(queue.TryRemember(
            RoutingId.From("first"),
            1,
            ServiceWireConstants.Command.Hello,
            new byte[] { 1 }));
        Assert.False(queue.TryRemember(
            RoutingId.From("second"),
            2,
            ServiceWireConstants.Command.Hello,
            new byte[] { 2 }));

        Assert.Equal(1, queue.Count);
        Assert.Equal(1, queue.CapacityRejectionCount);
    }
}
