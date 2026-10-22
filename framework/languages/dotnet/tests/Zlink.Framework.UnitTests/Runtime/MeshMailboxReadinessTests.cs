using System.Collections;
using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.CompilerServices;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshMailboxReadinessTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Fact]
    public void ClaimKeepsItsOriginalPrefixAndEachRecordItsOwnPermit()
    {
        using var queue = new ZLinkApplicationJobQueue(
            new(ZLinkApplicationJobQueueProfile.Balanced, 2, 1, 2)
        );
        var mailbox = new ZLinkMeshNodeOwnedMailbox(_ => { }, _ => { });
        try
        {
            Assert.True(queue.TryAcquire(out var first, ZLinkApplicationJobOrigin.Remote));
            Assert.NotNull(first);
            first.MarkQueued();
            Assert.True(
                mailbox.TryEnqueue(NewRecord(new ZLinkApplicationJobQueueRecordOwner(null, first)))
            );
            using var batch = new MeshReceiveBatch();
            Assert.True(Drain(mailbox, batch));
            Assert.Equal(1, batch.Count);
            Assert.Same(first, batch.GetApplicationJobAdmission(0));

            Assert.True(queue.TryAcquire(out var second, ZLinkApplicationJobOrigin.Remote));
            Assert.NotNull(second);
            second.MarkQueued();
            Assert.True(
                mailbox.TryEnqueue(NewRecord(new ZLinkApplicationJobQueueRecordOwner(null, second)))
            );
            batch.Reset();
            Assert.False(Drain(mailbox, batch));
            Assert.Equal(1, mailbox.Count);
            Assert.True(Release(mailbox));

            Assert.True(Drain(mailbox, batch));
            Assert.Equal(1, batch.Count);
            Assert.Same(second, batch.GetApplicationJobAdmission(0));
            batch.Reset();
            Assert.False(Release(mailbox));
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
        }
        finally
        {
            mailbox.Dispose();
        }
    }

    [Fact]
    public void BoundedMailboxTurn_KeepsClaimExclusiveAndRearmsResidue()
    {
        var queued = 0;
        var mailbox = new ZLinkMeshNodeOwnedMailbox(_ => queued++, _ => queued--);
        try
        {
            for (var index = 0; index < 65; index++)
                Assert.True(mailbox.TryEnqueue(NewRecord()));
            using var batch = new MeshReceiveBatch();
            Assert.True(Drain(mailbox, batch));
            Assert.Equal(64, batch.Count);
            Assert.Equal(1, queued);
            Assert.True(Release(mailbox));
            batch.Reset();
            Assert.True(Drain(mailbox, batch));
            Assert.Equal(1, batch.Count);
            Assert.Equal(0, queued);
            Assert.False(Release(mailbox));
        }
        finally
        {
            mailbox.Dispose();
        }
    }

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
                        Assert.True(Drain(mailbox!, batch));
                    });
                Assert.True(consumerStarted.Wait(TimeSpan.FromSeconds(3)));
                transitions.Enqueue(Interlocked.Increment(ref count));
            },
            _ => transitions.Enqueue(Interlocked.Decrement(ref count))
        );

        Assert.True(mailbox.TryEnqueue(NewRecord()));
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
            }
        );
        Assert.True(mailbox.TryEnqueue(NewRecord(payload)));
        // The mailbox has no bound of its own, so a further record is accepted too.
        Assert.True(mailbox.TryEnqueue(NewRecord()));
        Assert.Equal(2, Volatile.Read(ref count));

        mailbox.Dispose();
        mailbox.Dispose();

        Assert.Equal(0, Volatile.Read(ref count));
        Assert.Equal(1, payload.DisposeCount);
        Assert.Null(payload.LaneDuringDispose);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ReadinessRearmsForResidueAndDoesNotSignalAnEmptyNode(bool installAfterEnqueue)
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

    [Fact]
    public async Task ReadyOwnersAreReservedOnceInIdentityOrderAndKeepRecordFifo()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "ready-order");
        node.SetRoutingId(RoutingId.From("ready-order-node"));
        var actors = new[] { "c", "a", "b" }.Select(actorId => node.CreateActor(actorId)).ToArray();
        Drain(node);
        // Idle owners must not participate in readiness or claim turns.
        for (var index = 0; index < 32; index++)
            node.CreateActor($"idle-{index}");
        Drain(node);
        foreach (var actor in actors)
            for (byte value = 1; value <= 2; value++)
            {
                using var payload = Message.From(new[] { value });
                Assert.Equal(SubmitResult.Ok, node.SendToActor(actor, [payload]));
            }

        using var ready = new MeshReadyBatch();
        Assert.False(node.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait));
        Assert.Equal(
            new[] { "a", "b", "c" },
            Enumerable.Range(0, ready.Count).Select(index => ready[index].Actor.ActorId)
        );
        // Reservation is exclusive even before the first receive claims its prefix.
        using var competing = new MeshReadyBatch();
        Assert.False(node.DrainReady(MeshReadyDomains.Application, competing, RecvFlags.DontWait));
        Assert.Equal(0, competing.Count);
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            Assert.True(claim.Receive(received, RecvFlags.DontWait));
            Assert.Equal(2, received.Count);
            using var whileReceiving = new MeshReadyBatch();
            Assert.False(
                node.DrainReady(MeshReadyDomains.Application, whileReceiving, RecvFlags.DontWait)
            );
            Assert.Equal(0, whileReceiving.Count);
            for (var record = 0; record < received.Count; record++)
            {
                var parts = received.RetainMessage(record);
                try
                {
                    Assert.Equal((byte)(record + 1), parts[0].ToArray()[0]);
                }
                finally
                {
                    foreach (var part in parts)
                        part.Dispose();
                }
            }
        }
        Assert.Equal(0, Drain(node));
    }

    [Fact]
    public void ReadyOrderingPreservesAllKeysAndStableTies()
    {
        using var fixture = new ReadyRegistryFixture();
        foreach (
            var domain in new[] { MeshReadyDomains.Application, MeshReadyDomains.Infrastructure }
        )
        foreach (var kind in Enum.GetValues<MeshOwnerKind>())
        foreach (var identity in new[] { "b", "a", "A" })
            fixture.Add(kind, identity, domain);
        for (ulong generation = 8; generation > 0; generation--)
            fixture.Add(MeshOwnerKind.Actor, "tie", MeshReadyDomains.Application, generation);

        // Preserve registry enumeration order for equal keys, including generations.
        var expected = fixture
            .Ready.Keys.Cast<object>()
            .OrderBy(entry =>
                fixture.KeyValue<MeshReadyDomains>(entry, "Domain")
                == MeshReadyDomains.Infrastructure
                    ? 0
                    : 1
            )
            .ThenBy(entry => fixture.KeyValue<MeshOwnerKind>(entry, "OwnerKind"))
            .ThenBy(entry => fixture.KeyValue<string>(entry, "Identity"), StringComparer.Ordinal)
            .Select(entry => fixture.KeyValue<string>(entry, "SpotId"))
            .ToArray();
        using var batch = new MeshReadyBatch();
        Assert.False(fixture.Node.DrainReady(MeshReadyDomains.All, batch));
        Assert.Equal(expected, Enumerable.Range(0, batch.Count).Select(i => batch[i].SpotId));
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    public void ReadyLimitAppliesAfterGlobalOrdering(int limit)
    {
        using var fixture = new ReadyRegistryFixture();
        fixture.Add(MeshOwnerKind.Node, "a", MeshReadyDomains.Application);
        fixture.Add(MeshOwnerKind.Node, "z", MeshReadyDomains.Infrastructure);
        using var batch = new MeshReadyBatch { MaximumRecords = limit };
        Assert.Equal(limit < 2, fixture.Node.DrainReady(MeshReadyDomains.All, batch));
        Assert.Equal(Math.Min(limit, 2), batch.Count);
        Assert.Equal(batch.Count, fixture.Ready.Values.Cast<bool>().Count(ready => !ready));
        if (limit > 0)
            Assert.Equal(MeshReadyDomains.Infrastructure, batch[0].Domain);
        if (limit > 1)
            Assert.Equal(MeshReadyDomains.Application, batch[1].Domain);
    }

    [Fact]
    public void StaleReadySnapshotCannotReserveAnOwnerAgain()
    {
        using var fixture = new ReadyRegistryFixture();
        var stale = fixture.Add(MeshOwnerKind.Node, "a", MeshReadyDomains.Application);
        var next = fixture.Add(MeshOwnerKind.Node, "b", MeshReadyDomains.Application);
        // Enumeration and sorting do not hash keys. The next hash happens inside
        // TryUpdate, after its input snapshot has been sorted. Reserve the first
        // candidate there to deterministically reproduce a competing reservation.
        fixture.BeforeLookup = () => fixture.Ready[stale] = false;
        using var batch = new MeshReadyBatch { MaximumRecords = 1 };
        Assert.False(fixture.Node.DrainReady(MeshReadyDomains.All, batch));
        Assert.Equal(1, batch.Count);
        Assert.Equal(fixture.KeyValue<string>(next, "SpotId"), batch[0].SpotId);
        Assert.False((bool)fixture.Ready[stale]!);
        Assert.Null(fixture.BeforeLookup);
    }

    [Fact]
    public void ReusedBatchKeepsExistingClaimsAndCompactsCompetingCandidates()
    {
        using var fixture = new ReadyRegistryFixture();
        var first = fixture.Add(MeshOwnerKind.Node, "a", MeshReadyDomains.Application);
        using var batch = new MeshReadyBatch { MaximumRecords = 2 };
        Assert.False(fixture.Node.DrainReady(MeshReadyDomains.All, batch));
        var original = batch.TakeClaim(0);
        var stale = fixture.Add(MeshOwnerKind.Node, "b", MeshReadyDomains.Application);
        var next = fixture.Add(MeshOwnerKind.Node, "c", MeshReadyDomains.Application);
        fixture.BeforeLookup = () => fixture.Ready[stale] = false;
        Assert.False(fixture.Node.DrainReady(MeshReadyDomains.All, batch));
        Assert.Equal(2, batch.Count);
        Assert.Same(original, batch.TakeClaim(0));
        Assert.Equal(fixture.KeyValue<string>(first, "SpotId"), batch[0].SpotId);
        Assert.Equal(fixture.KeyValue<string>(next, "SpotId"), batch[1].SpotId);
        batch.Reset();
        Assert.False(fixture.Node.DrainReady(MeshReadyDomains.All, batch));
        Assert.Equal(0, batch.Count);
    }

    [Theory]
    [InlineData(0, (int)MeshReadyDomains.All)]
    [InlineData(32, (int)MeshReadyDomains.Infrastructure)]
    [InlineData(32, (int)MeshReadyDomains.Application)]
    public void ReadySelectionAddsNoAllocationBeyondRegistryEnumeration(
        int owners,
        int requestedDomains
    )
    {
        var domains = (MeshReadyDomains)requestedDomains;
        const int iterations = 1000;
        using var fixture = new ReadyRegistryFixture();
        for (var index = 0; index < owners; index++)
            fixture.Add(MeshOwnerKind.Node, $"owner-{index}", MeshReadyDomains.Application);
        // A zero output limit exercises selection and global ordering without
        // allocating claims or involving mailbox release. Warm the reused batch
        // first, as the real pump does when it retains its output batch.
        using var batch = new MeshReadyBatch { MaximumRecords = 0 };
        var enumerate = (Action)
            typeof(MeshMailboxReadinessTests)
                .GetMethod(
                    nameof(CreateRegistryEnumerator),
                    BindingFlags.Static | BindingFlags.NonPublic
                )!
                .MakeGenericMethod(fixture.Ready.GetType().GenericTypeArguments[0])
                .Invoke(null, [fixture.Ready])!;
        for (var index = 0; index < iterations; index++)
        {
            enumerate();
            fixture.Node.DrainReady(domains, batch);
        }
        var enumerationBytes = PerCallAllocation.Steady(enumerate, iterations);
        var selectionBytes = PerCallAllocation.Steady(
            () => fixture.Node.DrainReady(domains, batch),
            iterations
        );
        output.WriteLine(
            $"owners={owners}; domains={domains}; calls={iterations}; "
                + $"enumeration bytes/call={enumerationBytes}; "
                + $"selection bytes/call={selectionBytes}"
        );
        // The registry enumeration is the only allowed allocation. Newer runtimes
        // may allocate less for the same enumeration, never more for selection.
        Assert.True(
            selectionBytes <= enumerationBytes,
            $"selection allocated {selectionBytes} B/call, enumeration {enumerationBytes} B/call"
        );
        Assert.Equal(0, batch.Count);
        Assert.All(fixture.Ready.Values.Cast<bool>(), Assert.True);
    }

    private static Action CreateRegistryEnumerator<TKey>(object registry)
        where TKey : notnull
    {
        var ready = (ConcurrentDictionary<TKey, bool>)registry;
        return () =>
        {
            foreach (var entry in ready)
            {
                if (entry.Value)
                    continue;
            }
        };
    }

    // Framework-only fixture: no native context, transport, or worker is started.
    private sealed class ReadyRegistryFixture : IDisposable
    {
        private const BindingFlags Fields = BindingFlags.Instance | BindingFlags.NonPublic;
        private readonly Type _keyType;
        private readonly IDictionary _owned;
        private readonly ILookupHook _hook;
        internal ZLinkManagedMeshNode Node { get; }
        internal IDictionary Ready { get; }
        internal Action? BeforeLookup
        {
            get => _hook.BeforeLookup;
            set => _hook.BeforeLookup = value;
        }

        internal ReadyRegistryFixture()
        {
            Node = (ZLinkManagedMeshNode)
                RuntimeHelpers.GetUninitializedObject(typeof(ZLinkManagedMeshNode));
            var readyField = typeof(ZLinkManagedMeshNode).GetField("_readyMailboxes", Fields)!;
            var ownedField = typeof(ZLinkManagedMeshNode).GetField("_ownedMailboxes", Fields)!;
            _keyType = readyField.FieldType.GenericTypeArguments[0];
            var comparer = Activator.CreateInstance(
                typeof(LookupHook<>).MakeGenericType(_keyType)
            )!;
            _hook = (ILookupHook)comparer;
            Ready = (IDictionary)Activator.CreateInstance(readyField.FieldType, comparer)!;
            _owned = (IDictionary)Activator.CreateInstance(ownedField.FieldType)!;
            readyField.SetValue(Node, Ready);
            ownedField.SetValue(Node, _owned);
        }

        internal object Add(
            MeshOwnerKind kind,
            string identity,
            MeshReadyDomains domain,
            ulong generation = 0
        )
        {
            var key = Activator.CreateInstance(
                _keyType,
                BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                null,
                [
                    kind,
                    identity,
                    generation,
                    domain,
                    $"{domain}:{kind}:{identity}:{generation}",
                    default(ActorRef),
                ],
                null
            )!;
            Ready.Add(key, true);
            _owned.Add(key, new ZLinkMeshNodeOwnedMailbox(_ => { }, _ => { }));
            return key;
        }

        internal T KeyValue<T>(object key, string name) =>
            (T)_keyType.GetProperty(name)!.GetValue(key)!;

        public void Dispose()
        {
            foreach (ZLinkMeshNodeOwnedMailbox mailbox in _owned.Values)
                mailbox.Dispose();
        }
    }

    private interface ILookupHook
    {
        Action? BeforeLookup { get; set; }
    }

    private sealed class LookupHook<TKey> : IEqualityComparer<TKey>, ILookupHook
        where TKey : notnull
    {
        public Action? BeforeLookup { get; set; }

        public bool Equals(TKey? left, TKey? right) =>
            EqualityComparer<TKey>.Default.Equals(left, right);

        public int GetHashCode(TKey key)
        {
            var callback = BeforeLookup;
            BeforeLookup = null;
            callback?.Invoke();
            return EqualityComparer<TKey>.Default.GetHashCode(key);
        }
    }

    private static ZLinkMeshQueuedRecord NewRecord(IDisposable? payloadOwner = null) =>
        new(
            MeshReceiveRecord.CompletionFailure(default, RequestResult.Ok),
            [],
            applicationPayloadBytes: 0,
            payloadOwner: payloadOwner
        );

    private static bool Drain(ZLinkMeshNodeOwnedMailbox mailbox, MeshReceiveBatch batch) =>
        mailbox.ClaimAndDrainAsync(batch, 64).AsTask().GetAwaiter().GetResult();

    // An idle mailbox lane runs the posted release before Release returns.
    private static bool Release(ZLinkMeshNodeOwnedMailbox mailbox)
    {
        mailbox.Release();
        return mailbox.HasRecords;
    }

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
