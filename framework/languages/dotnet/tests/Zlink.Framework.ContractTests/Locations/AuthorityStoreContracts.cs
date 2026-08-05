using System.Security.Cryptography;
using System.Text;
using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Locations;

/// <summary>
///     Worked examples for the complete Location Store provider contract and
///     the separate Relocation Store payload contract.
/// </summary>
public sealed class AuthorityStoreContracts
{
    private static readonly DateTimeOffset StoreNow =
        new(2026, 7, 25, 9, 30, 0, TimeSpan.Zero);

    private static readonly ZLinkMeshNodeDescriptorKey MatchNodeB =
        new("play", RoutingId.From("play-node-b"));

    private static readonly ZLinkMeshNodeDescriptorKey MatchNodeC =
        new("play", RoutingId.From("play-node-c"));

    private static readonly ZLinkLocationOwnerToken OwnerB =
        new("play-node-b#0f2c", 41L);

    private static readonly ZLinkLocationOwnerToken OwnerC =
        new("play-node-c#7ab1", 42L);

    [Fact]
    public async Task Authority_store_reserves_commits_and_hands_an_object_to_a_new_owner()
    {
        var store = new ExampleAuthorityStore();
        var actorKey = new ZLinkAuthorityKey("play/actor/player-8821");

        // 1. Creation starts as a reservation over a Missing authority. The
        //    store, not the caller, issues ObjectGeneration,
        //    AuthorityOwnerGeneration and StoreVersion.
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(ActorReservation(actorKey, OwnerB, MatchNodeB)));
        var reservation = reserved.Reservation;
        Assert.Equal(1UL, reservation.ObjectGeneration);
        Assert.Equal(1UL, reservation.AuthorityOwnerGeneration);

        // A second reservation over the same Creating row loses; the winner is
        // the only node that runs the factory.
        Assert.IsType<ZLinkObjectReserveResult.Conflict>(
            await store.ReserveAsync(ActorReservation(actorKey, OwnerC, MatchNodeC)));

        // 2. The reserving node runs the factory and publishes the terminal
        //    outcome with the ready payload in one call, so a retried creation
        //    request replays the same terminal instead of building a second
        //    actor.
        var operation = new ZLinkCreationOperationId(
            RoutingId.From("play-node-b"),
            SourceNodeGeneration: 9,
            OperationIdHigh: 0x0193_5C71_A4E0_0000,
            OperationIdLow: 0x0000_0000_0000_0001);
        var readyPayload = Encoding.UTF8.GetBytes(
            """{"actorType":"player","spotId":"battle-1042","rating":1873}""");
        var terminalEnvelope = Encoding.UTF8.GetBytes(
            """{"state":"created","actorId":"player-8821"}""");
        var created = Assert.IsType<ZLinkObjectCreationCompleteResult.Created>(
            await store.CompleteCreationAsync(
                reservation,
                new ZLinkObjectCreationCompletion.Created(
                    readyPayload,
                    new ZLinkCreationTerminalPublication(
                        operation,
                        terminalEnvelope,
                        SHA256.HashData(terminalEnvelope),
                        StoreNow.AddHours(1)))));
        Assert.Equal(ZLinkPlacementAllocationState.Active, created.Snapshot.Allocation.State);
        Assert.Equal(OwnerB.OwnerId, created.Snapshot.OwnerId);
        Assert.Null(created.Snapshot.ReservedCreation);

        var replayed = Assert.IsType<ZLinkCreationTerminalReadResult.Found>(
            await store.ReadCreationTerminalAsync(operation));
        Assert.Equal(ZLinkCreationTerminalState.Created, replayed.Record.State);

        // 3. Preserve keeps both generations and refreshes only the payload.
        //    It carries no TargetOwner and no capacity fence.
        var active = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(actorKey)).Snapshot;
        var preserved = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                actorKey,
                active.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    Encoding.UTF8.GetBytes(
                        """{"actorType":"player","spotId":"battle-1042","rating":1901}"""),
                    ZLinkAuthorityGenerationTransition.Preserve,
                    TargetOwner: null,
                    RelocationCapacityFence: null)));
        Assert.Equal(active.ObjectGeneration, preserved.Snapshot.ObjectGeneration);
        Assert.Equal(
            active.AuthorityOwnerGeneration,
            preserved.Snapshot.AuthorityOwnerGeneration);

        // A stale StoreVersion loses and mutates nothing; the loser reconciles
        // from the current read carried by Conflict.
        var conflict = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(
            await store.CompareExchangeAuthorityAsync(
                actorKey,
                active.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
        Assert.IsType<ZLinkAuthorityReadResult.Found>(conflict.Current);

        // 4. Relocation to play-node-c: reserve target capacity first, then
        //    hand ownership over with the fence the reservation issued.
        var capacityReservation =
            Assert.IsType<ZLinkRelocationCapacityReserveResult.Reserved>(
            await store.ReserveRelocationCapacityAsync(
                new ZLinkRelocationCapacityReservationRequest(
                    Guid.Parse("6f1d1f8c-6b21-4f3a-9a52-2f0a5f8d4c11"),
                    actorKey,
                    preserved.Snapshot.StoreVersion,
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    MatchNodeB,
                    SourceNodeLifecycleGeneration: 9,
                    OwnerB,
                    MatchNodeC,
                    TargetNodeLifecycleGeneration: 4,
                    OwnerC,
                    ActorCapacity())));
        var fence = capacityReservation.Fence;

        var handedOver = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                actorKey,
                preserved.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    readyPayload,
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    OwnerC,
                    fence)));
        Assert.Equal(
            preserved.Snapshot.ObjectGeneration,
            handedOver.Snapshot.ObjectGeneration);
        Assert.Equal(
            capacityReservation.TargetAuthorityOwnerGeneration,
            handedOver.Snapshot.AuthorityOwnerGeneration);
        Assert.Equal(OwnerC.OwnerId, handedOver.Snapshot.OwnerId);
        Assert.Equal(MatchNodeC, handedOver.Snapshot.Allocation.Descriptor);

        // Committing the same fence twice is idempotent, so a retry after a
        // lost reply does not double-charge the target's capacity.
        Assert.Equal(
            ZLinkRelocationCapacityAbortResult.AlreadyCommitted,
            await store.AbortRelocationCapacityAsync(fence));

        // 5. Host recovery scans the prefix rather than resolving one key.
        var page = Assert.IsType<ZLinkAuthorityScanResult.Page>(
            await store.ListAuthoritiesAsync("play/actor/", cursor: null, limit: 200));
        var entry = Assert.Single(page.Value.Items);
        Assert.Equal(actorKey, entry.Key);
        Assert.Null(page.Value.NextCursor);

        // 6. Fenced delete removes the row completely; no tombstone remains.
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await store.CompareExchangeAuthorityAsync(
                actorKey,
                handedOver.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(actorKey));
    }

    [Fact]
    public async Task Authority_store_aborts_a_failed_activation_and_moves_a_spot_aggregate()
    {
        var store = new ExampleAuthorityStore();
        var spotKey = new ZLinkAuthorityKey("play/spot/battle-1042");
        var memberKey = new ZLinkAuthorityKey("play/actor/player-8821");

        // A factory that throws before Ready aborts its own reservation. The
        // authority returns to Missing so the next explicit call can claim it.
        var abandoned = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                SpotReservation(spotKey, OwnerB, MatchNodeB))).Reservation;
        Assert.IsType<ZLinkObjectAbortResult.Aborted>(await store.AbortAsync(abandoned));
        Assert.IsType<ZLinkObjectAbortResult.AlreadyAborted>(await store.AbortAsync(abandoned));
        Assert.IsType<ZLinkAuthorityReadResult.Missing>(
            await store.ReadAuthorityAsync(spotKey));

        // The retry succeeds and reaches Active through Reserve + Commit.
        var reservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                SpotReservation(spotKey, OwnerB, MatchNodeB))).Reservation;
        var spotSnapshot = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reservation,
                Encoding.UTF8.GetBytes(
                    """{"spotType":"battle-room","members":["player-8821"]}"""))).Snapshot;

        // Once a spot exists, a create for the same key reports AlreadyExists
        // instead of starting a second incarnation.
        Assert.IsType<ZLinkObjectReserveResult.AlreadyExists>(
            await store.ReserveAsync(SpotReservation(spotKey, OwnerC, MatchNodeC)));

        var memberReservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ActorReservation(memberKey, OwnerB, MatchNodeB))).Reservation;
        var memberSnapshot = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                memberReservation,
                Encoding.UTF8.GetBytes("""{"actorType":"player"}"""))).Snapshot;

        // Retiring play-node-b moves the whole User Spot together with the
        // actors that were members at seal time: one prepare, one commit, and
        // every participant changes owner in the same transaction.
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await store.PrepareAggregateAsync(
                new ZLinkAggregatePrepareRequest(
                    Guid.Parse("b4d4e6c0-9a1e-4a5f-8f21-6b4c0f2c7d33"),
                    AggregateGeneration: 1,
                    [
                        new ZLinkAggregateParticipant(
                            spotKey,
                            spotSnapshot.StoreVersion,
                            ZLinkAuthorityGenerationTransition.NewOwner,
                            spotSnapshot.Payload,
                            Encoding.UTF8.GetBytes("""{"members":["player-8821"]}""")),
                        new ZLinkAggregateParticipant(
                            memberKey,
                            memberSnapshot.StoreVersion,
                            ZLinkAuthorityGenerationTransition.NewOwner,
                            memberSnapshot.Payload,
                            Encoding.UTF8.GetBytes("""{"spotId":"battle-1042"}"""))
                    ],
                    SHA256.HashData(Encoding.UTF8.GetBytes("battle-1042/player-8821")),
                    MatchNodeC,
                    TargetDescriptorLifecycleGeneration: 4,
                    SpotAggregateCapacity(),
                    OwnerC))).Fence;

        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await store.CommitAggregateAsync(prepared));
        Assert.Equal(
            ZLinkAggregateCommitResult.AlreadyCommitted,
            await store.CommitAggregateAsync(prepared));
        Assert.Equal(
            ZLinkAggregateAbortResult.Stale,
            await store.AbortAggregateAsync(prepared));

        // Every participant changed owner, took a fresh authority owner
        // generation and kept the object generation it was created with.
        foreach (var (key, before) in new[]
                 {
                     (spotKey, spotSnapshot), (memberKey, memberSnapshot)
                 })
        {
            var moved = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(key)).Snapshot;
            Assert.Equal(OwnerC.OwnerId, moved.OwnerId);
            Assert.Equal(MatchNodeC, moved.Allocation.Descriptor);
            Assert.Equal(4UL, moved.Allocation.DescriptorLifecycleGeneration);
            Assert.Equal(before.ObjectGeneration, moved.ObjectGeneration);
            Assert.True(moved.AuthorityOwnerGeneration > before.AuthorityOwnerGeneration);
        }
    }

    [Fact]
    public async Task Relocation_store_holds_the_opaque_payload_for_one_move_under_a_fixed_retention()
    {
        // The framework always passes exactly 24 hours; retention is not an
        // application option (08-authority-relocation §5).
        var retention = TimeSpan.FromHours(24);
        var store = new ExampleRelocationStore();
        var capture = Encoding.UTF8.GetBytes(
            """{"actorId":"player-8821","rating":1901,"inventory":["potion","torch"]}""");

        var stored = await store.PutRelocationAsync(capture, retention);
        Assert.Equal(StoreNow.Add(retention), stored.ExpiresAt);
        Assert.Equal(StoreNow, stored.StoreNow);

        // The runtime republishes this checksum through the location
        // authority and refuses the move when the two disagree.
        Assert.Equal(Crc32C(capture), stored.ChecksumCrc32c);

        var found = Assert.IsType<ZLinkRelocationReadResult.Found>(
            await store.GetRelocationAsync(stored.Reference));
        Assert.Equal(capture, found.Payload.ToArray());

        // A recovery coordinator that still sees the reference in the
        // authority extends the retention; the store owns the new expiry.
        var renewed = Assert.IsType<ZLinkRelocationRenewResult.Renewed>(
            await store.RenewRelocationAsync(stored.Reference, retention));
        Assert.Equal(StoreNow.Add(retention), renewed.ExpiresAt);
        Assert.IsType<ZLinkRelocationRenewResult.Missing>(
            await store.RenewRelocationAsync("relocation/abandoned-move", retention));

        // Delete is idempotent cleanup: the second call is still a success.
        Assert.Equal(
            ZLinkRelocationDeleteResult.Deleted,
            await store.DeleteRelocationAsync(stored.Reference));
        Assert.Equal(
            ZLinkRelocationDeleteResult.Missing,
            await store.DeleteRelocationAsync(stored.Reference));
        Assert.IsType<ZLinkRelocationReadResult.Missing>(
            await store.GetRelocationAsync(stored.Reference));
    }

    [Fact]
    public async Task Client_server_descriptors_are_channel_scoped_and_paged()
    {
        // ClientServer registration is optional for an application, but its
        // descriptor operations belong to every complete Location Store.
        // Rows carry a ChannelName and no MeshName or RouteMesh membership.
        var store = new ExampleClientServerLocationStore();

        var registered = await store.UpdateClientServerAsync(
            Descriptor(RoutingId.From("inventory-1"), weight: 100),
            ZLinkLocationWriteIntent.NewClaim);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, registered.Status);
        await store.UpdateClientServerAsync(
            Descriptor(RoutingId.From("inventory-2"), weight: 50),
            ZLinkLocationWriteIntent.NewClaim);

        var firstPage = await store.ListClientServersAsync(
            "inventory", new ZLinkPageRequest(PageSize: 1));
        Assert.Single(firstPage.Items);
        Assert.NotNull(firstPage.ContinuationToken);

        var secondPage = await store.ListClientServersAsync(
            "inventory",
            new ZLinkPageRequest(PageSize: 1, ContinuationToken: firstPage.ContinuationToken));
        Assert.Single(secondPage.Items);
        Assert.Null(secondPage.ContinuationToken);
        Assert.Equal("inventory", secondPage.Items[0].ChannelName);

        // A descriptor revision that is not strictly increasing is rejected
        // whole; the store never applies part of the row.
        var stale = await store.UpdateClientServerAsync(
            Descriptor(RoutingId.From("inventory-1"), weight: 10) with { DescriptorRevision = 1 },
            ZLinkLocationWriteIntent.Renew);
        Assert.Equal(ZLinkLocationWriteStatus.IgnoredStale, stale.Status);

        var removed = await store.RemoveClientServerAsync(
            new ZLinkClientServerServerDescriptorKey("inventory", RoutingId.From("inventory-2")),
            OwnerB);
        Assert.Equal(ZLinkLocationWriteStatus.Stored, removed);

        var remaining = await store.ListClientServersAsync("inventory", default);
        Assert.Single(remaining.Items);
        Assert.Equal(100, remaining.Items[0].Weight);
    }

    private static ZLinkClientServerServerDescriptor Descriptor(RoutingId serverRid, int weight) =>
        new(
            "inventory",
            serverRid,
            LifecycleGeneration: 7,
            DescriptorRevision: 2,
            $"tcp://10.4.2.11:{5100 + weight}",
            weight,
            ZLinkFrameworkRuntimeState.Serving,
            SecurityIdentity: "cluster-a",
            OwnerB.OwnerId,
            OwnerB.LeaseGeneration,
            StoreNow);

    private static ZLinkCapacityVector ActorCapacity() =>
        new(Actors: 1, Spots: 0, SpotType: null);

    private static ZLinkCapacityVector SpotAggregateCapacity() =>
        new(
            Actors: 1,
            Spots: 1,
            new ZLinkSpotTypeCapacityDelta(
                ZLinkPlacementObjectKind.UserSpot,
                "battle-room",
                Count: 1));

    private static ZLinkObjectReservationRequest ActorReservation(
        ZLinkAuthorityKey key,
        ZLinkLocationOwnerToken owner,
        ZLinkMeshNodeDescriptorKey target)
    {
        var intent = Encoding.UTF8.GetBytes("""{"actorType":"player","actorId":"player-8821"}""");
        return new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.Actor,
            key,
            "player",
            "relocation/creation-intent/player-8821",
            SHA256.HashData(intent),
            intent.Length,
            target,
            TargetNodeLifecycleGeneration: target == MatchNodeB ? 9UL : 4UL,
            owner,
            Encoding.UTF8.GetBytes("""{"state":"creating"}"""),
            ActorCapacity());
    }

    private static ZLinkObjectReservationRequest SpotReservation(
        ZLinkAuthorityKey key,
        ZLinkLocationOwnerToken owner,
        ZLinkMeshNodeDescriptorKey target)
    {
        var intent = Encoding.UTF8.GetBytes("""{"spotType":"battle-room","mode":"ranked-3v3"}""");
        return new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.UserSpot,
            key,
            "battle-room",
            "relocation/creation-intent/battle-1042",
            SHA256.HashData(intent),
            intent.Length,
            target,
            TargetNodeLifecycleGeneration: target == MatchNodeB ? 9UL : 4UL,
            owner,
            Encoding.UTF8.GetBytes("""{"state":"creating"}"""),
            new ZLinkCapacityVector(
                Actors: 0,
                Spots: 1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "battle-room",
                    Count: 1)));
    }

    private static uint Crc32C(ReadOnlySpan<byte> payload)
    {
        const uint polynomial = 0x82F63B78;
        var crc = 0xFFFFFFFFu;
        foreach (var value in payload)
        {
            crc ^= value;
            for (var bit = 0; bit < 8; bit++)
                crc = (crc >> 1) ^ (polynomial & (uint)-(int)(crc & 1));
        }

        return ~crc;
    }

    private sealed class ExampleAuthorityStore : LocationStoreContractExample
    {
        private readonly Dictionary<string, Row> _rows = new(StringComparer.Ordinal);

        private readonly Dictionary<ZLinkCreationOperationId, ZLinkCreationTerminalRecord>
            _terminals = [];

        private readonly Dictionary<ZLinkRelocationCapacityFence,
            (bool Committed, ulong TargetGeneration)> _capacityFences = [];
        private readonly Dictionary<ZLinkAggregateFence,
            (ZLinkAggregatePrepareRequest Request,
                IReadOnlyDictionary<ZLinkAuthorityKey, ulong> Generations)>
            _aggregates = [];
        private readonly HashSet<ZLinkAggregateFence> _committedAggregates = [];

        private ulong _storeRevision;
        private ulong _objectGeneration;
        private ulong _authorityOwnerGeneration;

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(Read(key));

        public override ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default)
        {
            if (mutation is ZLinkAuthorityMutation.Put put)
            {
                // Invalid owner/fence combinations never reach the backend.
                if (put.GenerationTransition == ZLinkAuthorityGenerationTransition.Preserve
                    && (put.TargetOwner is not null || put.RelocationCapacityFence is not null))
                {
                    throw new ArgumentException(
                        "Preserve carries neither a target owner nor a capacity fence.",
                        nameof(mutation));
                }

                if (put.GenerationTransition == ZLinkAuthorityGenerationTransition.NewOwner
                    && (put.TargetOwner is null || put.RelocationCapacityFence is null))
                {
                    throw new ArgumentException(
                        "NewOwner requires both a target owner and a capacity fence.",
                        nameof(mutation));
                }
            }

            if (!_rows.TryGetValue(key.Value, out var row)
                || row.Allocation.State != ZLinkPlacementAllocationState.Active
                || !string.Equals(row.StoreVersion, expectedStoreVersion, StringComparison.Ordinal))
            {
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Conflict(Read(key)));
            }

            if (mutation is ZLinkAuthorityMutation.Delete)
            {
                _rows.Remove(key.Value);
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Deleted(NextStoreVersion(), StoreNow));
            }

            var write = (ZLinkAuthorityMutation.Put)mutation;
            var updated = write.GenerationTransition switch
            {
                ZLinkAuthorityGenerationTransition.NewOwner => row with
                {
                    StoreVersion = NextStoreVersion(),
                    Payload = write.Payload,
                    AuthorityOwnerGeneration =
                        _capacityFences[
                            write.RelocationCapacityFence!.Value]
                        .TargetGeneration,
                    OwnerId = write.TargetOwner!.Value.OwnerId,
                    OwnerLeaseGeneration = write.TargetOwner!.Value.LeaseGeneration,
                    Allocation = row.Allocation with
                    {
                        Descriptor = _capacityFences.ContainsKey(write.RelocationCapacityFence!.Value)
                            ? MatchNodeC
                            : row.Allocation.Descriptor
                    }
                },
                _ => row with
                {
                    StoreVersion = NextStoreVersion(),
                    Payload = write.Payload
                }
            };

            if (write.GenerationTransition == ZLinkAuthorityGenerationTransition.NewOwner)
                _capacityFences[write.RelocationCapacityFence!.Value] =
                    (true, updated.AuthorityOwnerGeneration);

            _rows[key.Value] = updated;
            return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                new ZLinkAuthorityCompareExchangeResult.Stored(updated.ToSnapshot()));
        }

        public override ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
            string prefix,
            ZLinkAuthorityScanCursor? cursor,
            int limit,
            CancellationToken cancellationToken = default)
        {
            ArgumentOutOfRangeException.ThrowIfLessThan(limit, 1);
            ArgumentOutOfRangeException.ThrowIfGreaterThan(limit, 1000);

            var items = _rows
                .Where(pair => pair.Key.StartsWith(prefix, StringComparison.Ordinal))
                .OrderBy(pair => pair.Key, StringComparer.Ordinal)
                .Skip(cursor is { Encoded: var encoded } ? int.Parse(encoded) : 0)
                .Take(limit)
                .Select(pair => new ZLinkAuthorityEntry(
                    new ZLinkAuthorityKey(pair.Key),
                    pair.Value.ToSnapshot()))
                .ToArray();

            return ValueTask.FromResult<ZLinkAuthorityScanResult>(
                new ZLinkAuthorityScanResult.Page(new ZLinkAuthorityPage(items, NextCursor: null)));
        }

        public override ValueTask<ZLinkObjectReserveResult> ReserveAsync(
            ZLinkObjectReservationRequest request,
            CancellationToken cancellationToken = default)
        {
            if (_rows.TryGetValue(request.Key.Value, out var existing))
            {
                var snapshot = existing.ToSnapshot();
                return ValueTask.FromResult<ZLinkObjectReserveResult>(
                    existing.Allocation.State switch
                    {
                        ZLinkPlacementAllocationState.Reserved =>
                            new ZLinkObjectReserveResult.Conflict(Read(request.Key)),
                        _ when !string.Equals(
                            existing.Allocation.StableType,
                            request.StableType,
                            StringComparison.Ordinal) =>
                            new ZLinkObjectReserveResult.TypeMismatch(snapshot),
                        _ => new ZLinkObjectReserveResult.AlreadyExists(snapshot)
                    });
            }

            var reservationId = $"reservation/{request.Key.Value}/{++_storeRevision}";
            var row = new Row(
                NextStoreVersion(),
                request.CreatingPayload,
                ++_objectGeneration,
                ++_authorityOwnerGeneration,
                request.TargetOwner.OwnerId,
                request.TargetOwner.LeaseGeneration,
                new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.Reserved,
                    request.ObjectKind,
                    request.StableType,
                    request.TargetDescriptor,
                    request.TargetNodeLifecycleGeneration,
                    request.Capacity),
                new ZLinkReservedObjectCreation(
                    reservationId,
                    request.CreationIntentReference,
                    request.CreationIntentHash,
                    request.CreationIntentEncodedSize));
            _rows[request.Key.Value] = row;

            return ValueTask.FromResult<ZLinkObjectReserveResult>(
                new ZLinkObjectReserveResult.Reserved(
                    new ZLinkObjectReservation(
                        request.Key,
                        row.StoreVersion,
                        row.ObjectGeneration,
                        row.AuthorityOwnerGeneration,
                        reservationId,
                        request.TargetDescriptor,
                        request.TargetNodeLifecycleGeneration,
                        request.TargetOwner)));
        }

        public override ValueTask<ZLinkObjectCommitResult> CommitAsync(
            ZLinkObjectReservation reservation,
            ReadOnlyMemory<byte> readyPayload,
            CancellationToken cancellationToken = default)
        {
            if (!TryTakeReservation(reservation, out var row))
            {
                return ValueTask.FromResult<ZLinkObjectCommitResult>(
                    _rows.TryGetValue(reservation.Key.Value, out var current)
                    && current.Allocation.State == ZLinkPlacementAllocationState.Active
                    && string.Equals(
                        current.PendingReservationId,
                        reservation.ReservationVersion,
                        StringComparison.Ordinal)
                        ? new ZLinkObjectCommitResult.AlreadyCommitted(current.ToSnapshot())
                        : new ZLinkObjectCommitResult.Stale());
            }

            var committed = Activate(reservation.Key, row, readyPayload);
            return ValueTask.FromResult<ZLinkObjectCommitResult>(
                new ZLinkObjectCommitResult.Committed(committed));
        }

        public override ValueTask<ZLinkObjectCreationCompleteResult> CompleteCreationAsync(
            ZLinkObjectReservation reservation,
            ZLinkObjectCreationCompletion completion,
            CancellationToken cancellationToken = default)
        {
            var publication = completion switch
            {
                ZLinkObjectCreationCompletion.Created value => value.Terminal,
                ZLinkObjectCreationCompletion.Rejected value => value.Terminal,
                ZLinkObjectCreationCompletion.Failed value => ((ZLinkObjectCreationCompletion.Failed)
                    completion).Terminal,
                _ => throw new ArgumentOutOfRangeException(nameof(completion))
            };

            if (_terminals.TryGetValue(publication.Operation, out var replay))
            {
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    new ZLinkObjectCreationCompleteResult.AlreadyCompleted(replay));
            }

            if (!TryTakeReservation(reservation, out var row))
            {
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    new ZLinkObjectCreationCompleteResult.Stale());
            }

            var state = completion switch
            {
                ZLinkObjectCreationCompletion.Created => ZLinkCreationTerminalState.Created,
                ZLinkObjectCreationCompletion.Rejected => ZLinkCreationTerminalState.Rejected,
                _ => ZLinkCreationTerminalState.Failed
            };
            var record = new ZLinkCreationTerminalRecord(
                publication.Operation,
                reservation.ReservationVersion,
                row.Allocation.ObjectKind,
                state,
                publication.TerminalEnvelope,
                publication.TerminalEnvelopeSha256,
                publication.ExpiresAt,
                StoreNow);
            _terminals[publication.Operation] = record;

            if (completion is not ZLinkObjectCreationCompletion.Created created)
            {
                _rows.Remove(reservation.Key.Value);
                return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                    state == ZLinkCreationTerminalState.Rejected
                        ? new ZLinkObjectCreationCompleteResult.Rejected(record)
                        : new ZLinkObjectCreationCompleteResult.Failed(record));
            }

            var snapshot = Activate(reservation.Key, row, created.ReadyPayload);
            return ValueTask.FromResult<ZLinkObjectCreationCompleteResult>(
                new ZLinkObjectCreationCompleteResult.Created(snapshot, record));
        }

        public override ValueTask<ZLinkCreationTerminalReadResult> ReadCreationTerminalAsync(
            ZLinkCreationOperationId operation,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ZLinkCreationTerminalReadResult>(
                _terminals.TryGetValue(operation, out var record)
                    ? new ZLinkCreationTerminalReadResult.Found(record)
                    : new ZLinkCreationTerminalReadResult.Missing(StoreNow));

        public override ValueTask<ZLinkObjectAbortResult> AbortAsync(
            ZLinkObjectReservation reservation,
            CancellationToken cancellationToken = default)
        {
            if (TryTakeReservation(reservation, out _))
            {
                _rows.Remove(reservation.Key.Value);
                return ValueTask.FromResult<ZLinkObjectAbortResult>(
                    new ZLinkObjectAbortResult.Aborted());
            }

            return ValueTask.FromResult<ZLinkObjectAbortResult>(
                _rows.ContainsKey(reservation.Key.Value)
                    ? new ZLinkObjectAbortResult.Stale()
                    : new ZLinkObjectAbortResult.AlreadyAborted());
        }

        public override ValueTask<ZLinkRelocationCapacityReserveResult> ReserveRelocationCapacityAsync(
            ZLinkRelocationCapacityReservationRequest request,
            CancellationToken cancellationToken = default)
        {
            var fence = new ZLinkRelocationCapacityFence(
                $"fence/{request.Key.Value}/{request.ReservationId:N}");
            if (_capacityFences.ContainsKey(fence))
            {
                var existing = _capacityFences[fence];
                return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                    new ZLinkRelocationCapacityReserveResult.AlreadyReserved(fence)
                    {
                        TargetAuthorityOwnerGeneration =
                            existing.TargetGeneration
                    });
            }

            if (!_rows.TryGetValue(request.Key.Value, out var row)
                || !string.Equals(
                    row.StoreVersion,
                    request.ExpectedStoreVersion,
                    StringComparison.Ordinal))
            {
                return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                    new ZLinkRelocationCapacityReserveResult.Conflict(Read(request.Key)));
            }

            var targetGeneration = ++_authorityOwnerGeneration;
            _capacityFences[fence] = (false, targetGeneration);
            return ValueTask.FromResult<ZLinkRelocationCapacityReserveResult>(
                new ZLinkRelocationCapacityReserveResult.Reserved(fence)
                {
                    TargetAuthorityOwnerGeneration = targetGeneration
                });
        }

        public override ValueTask<ZLinkRelocationCapacityAbortResult> AbortRelocationCapacityAsync(
            ZLinkRelocationCapacityFence fence,
            CancellationToken cancellationToken = default)
        {
            if (!_capacityFences.TryGetValue(fence, out var committed))
                return ValueTask.FromResult(ZLinkRelocationCapacityAbortResult.AlreadyAborted);
            if (committed.Committed)
                return ValueTask.FromResult(ZLinkRelocationCapacityAbortResult.AlreadyCommitted);

            _capacityFences.Remove(fence);
            return ValueTask.FromResult(ZLinkRelocationCapacityAbortResult.Aborted);
        }

        public override ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
            ZLinkAggregatePrepareRequest request,
            CancellationToken cancellationToken = default)
        {
            var fence = new ZLinkAggregateFence(request.AggregateId, request.AggregateGeneration);
            if (_aggregates.ContainsKey(fence))
            {
                var existing = _aggregates[fence];
                return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                    new ZLinkAggregatePrepareResult.AlreadyPrepared(fence)
                    {
                        TargetAuthorityOwnerGenerations =
                            existing.Generations
                    });
            }

            foreach (var participant in request.Participants)
            {
                if (!_rows.TryGetValue(participant.Key.Value, out var row)
                    || row.Allocation.State != ZLinkPlacementAllocationState.Active
                    || !string.Equals(
                        row.StoreVersion,
                        participant.ExpectedStoreVersion,
                        StringComparison.Ordinal))
                {
                    return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                        new ZLinkAggregatePrepareResult.Conflict());
                }
            }

            var generations = request.Participants.ToDictionary(
                static participant => participant.Key,
                _ => ++_authorityOwnerGeneration);
            _aggregates[fence] = (request, generations);
            return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                new ZLinkAggregatePrepareResult.Prepared(fence)
                {
                    TargetAuthorityOwnerGenerations = generations
                });
        }

        public override ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default)
        {
            if (_committedAggregates.Contains(fence))
                return ValueTask.FromResult(ZLinkAggregateCommitResult.AlreadyCommitted);
            if (!_aggregates.TryGetValue(fence, out var aggregate))
                return ValueTask.FromResult(ZLinkAggregateCommitResult.Stale);

            foreach (var participant in aggregate.Request.Participants)
            {
                var row = _rows[participant.Key.Value];
                _rows[participant.Key.Value] = row with
                {
                    StoreVersion = NextStoreVersion(),
                    Payload = participant.AuthorityPayload,
                    AuthorityOwnerGeneration =
                        aggregate.Generations[participant.Key],
                    OwnerId = aggregate.Request.TargetOwner.OwnerId,
                    OwnerLeaseGeneration =
                        aggregate.Request.TargetOwner.LeaseGeneration,
                    Allocation = row.Allocation with
                    {
                        Descriptor = aggregate.Request.TargetDescriptor,
                        DescriptorLifecycleGeneration =
                            aggregate.Request
                                .TargetDescriptorLifecycleGeneration
                    }
                };
            }

            _committedAggregates.Add(fence);
            return ValueTask.FromResult(ZLinkAggregateCommitResult.Committed);
        }

        public override ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default)
        {
            if (_committedAggregates.Contains(fence))
                return ValueTask.FromResult(ZLinkAggregateAbortResult.Stale);

            return ValueTask.FromResult(
                _aggregates.Remove(fence)
                    ? ZLinkAggregateAbortResult.Aborted
                    : ZLinkAggregateAbortResult.AlreadyAborted);
        }

        private ZLinkAuthorityReadResult Read(ZLinkAuthorityKey key) =>
            _rows.TryGetValue(key.Value, out var row)
                ? new ZLinkAuthorityReadResult.Found(row.ToSnapshot())
                : new ZLinkAuthorityReadResult.Missing(StoreNow);

        private bool TryTakeReservation(ZLinkObjectReservation reservation, out Row row)
        {
            if (_rows.TryGetValue(reservation.Key.Value, out var current)
                && current.Allocation.State == ZLinkPlacementAllocationState.Reserved
                && string.Equals(
                    current.StoreVersion,
                    reservation.StoreVersion,
                    StringComparison.Ordinal)
                && string.Equals(
                    current.PendingReservationId,
                    reservation.ReservationVersion,
                    StringComparison.Ordinal))
            {
                row = current;
                return true;
            }

            row = null!;
            return false;
        }

        private ZLinkAuthoritySnapshot Activate(
            ZLinkAuthorityKey key,
            Row row,
            ReadOnlyMemory<byte> readyPayload)
        {
            var active = row with
            {
                StoreVersion = NextStoreVersion(),
                Payload = readyPayload,
                Allocation = row.Allocation with
                {
                    State = ZLinkPlacementAllocationState.Active
                },
                ReservedCreation = null
            };
            _rows[key.Value] = active;
            return active.ToSnapshot();
        }

        private string NextStoreVersion() =>
            (++_storeRevision).ToString("D6", System.Globalization.CultureInfo.InvariantCulture);

        private sealed record Row(
            string StoreVersion,
            ReadOnlyMemory<byte> Payload,
            ulong ObjectGeneration,
            ulong AuthorityOwnerGeneration,
            string OwnerId,
            long OwnerLeaseGeneration,
            ZLinkPlacementAllocation Allocation,
            ZLinkReservedObjectCreation? ReservedCreation)
        {
            public string? PendingReservationId =>
                ReservedCreation?.ReservationId;

            public ZLinkAuthoritySnapshot ToSnapshot() => new(
                StoreVersion,
                Payload,
                ObjectGeneration,
                AuthorityOwnerGeneration,
                OwnerId,
                OwnerLeaseGeneration,
                Allocation,
                ReservedCreation,
                StoreNow);
        }
    }

    private sealed class ExampleRelocationStore : IZLinkRelocationRepository
    {
        private readonly Dictionary<string, byte[]> _payloads = new(StringComparer.Ordinal);
        private int _sequence;

        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            // The provider copies at the boundary: the framework may reuse the
            // buffer as soon as the operation completes.
            var stored = payload.ToArray();
            var reference = $"relocation/play/{++_sequence:D4}";
            _payloads[reference] = stored;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                Crc32C(stored),
                StoreNow.Add(retention),
                StoreNow));
        }

        public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
            string reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var stored = payload.ToArray();
            if (_payloads.TryGetValue(reference, out var current)
                && !current.AsSpan().SequenceEqual(stored))
                throw new InvalidDataException("Relocation reference collision.");
            _payloads[reference] = stored;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                Crc32C(stored),
                StoreNow.Add(retention),
                StoreNow));
        }

        public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ZLinkRelocationReadResult>(
                _payloads.TryGetValue(reference, out var payload)
                    ? new ZLinkRelocationReadResult.Found(payload)
                    : new ZLinkRelocationReadResult.Missing());

        public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ZLinkRelocationRenewResult>(
                _payloads.ContainsKey(reference)
                    ? new ZLinkRelocationRenewResult.Renewed(StoreNow.Add(retention), StoreNow)
                    : new ZLinkRelocationRenewResult.Missing());

        public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(
                _payloads.Remove(reference)
                    ? ZLinkRelocationDeleteResult.Deleted
                    : ZLinkRelocationDeleteResult.Missing);
    }

    private sealed class ExampleClientServerLocationStore : LocationStoreContractExample
    {
        private readonly Dictionary<ZLinkClientServerServerDescriptorKey,
            ZLinkClientServerServerDescriptor> _rows = [];

        public override ValueTask<ZLinkLocationWriteResult> UpdateClientServerAsync(
            ZLinkClientServerServerDescriptor descriptor,
            ZLinkLocationWriteIntent intent,
            CancellationToken cancellationToken = default)
        {
            var key = new ZLinkClientServerServerDescriptorKey(
                descriptor.ChannelName,
                descriptor.ServerRid);
            if (_rows.TryGetValue(key, out var current)
                && descriptor.DescriptorRevision <= current.DescriptorRevision)
            {
                return ValueTask.FromResult(ZLinkLocationWriteResult.IgnoredStale);
            }

            _rows[key] = descriptor;
            return ValueTask.FromResult(
                ZLinkLocationWriteResult.Stored(descriptor.LifecycleGeneration, StoreNow));
        }

        public override ValueTask<ZLinkLocationWriteStatus> RemoveClientServerAsync(
            ZLinkClientServerServerDescriptorKey key,
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default)
        {
            if (!_rows.TryGetValue(key, out var current)
                || !string.Equals(current.OwnerId, owner.OwnerId, StringComparison.Ordinal)
                || current.LeaseGeneration != owner.LeaseGeneration)
            {
                return ValueTask.FromResult(ZLinkLocationWriteStatus.IgnoredStale);
            }

            _rows.Remove(key);
            return ValueTask.FromResult(ZLinkLocationWriteStatus.Stored);
        }

        public override ValueTask<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> ListClientServersAsync(
            string channelName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default)
        {
            var pageSize = page.PageSize == 0 ? 100 : page.PageSize;
            var offset = page.ContinuationToken is null
                ? 0
                : int.Parse(page.ContinuationToken, System.Globalization.CultureInfo.InvariantCulture);
            var ordered = _rows.Values
                .Where(descriptor => string.Equals(
                    descriptor.ChannelName,
                    channelName,
                    StringComparison.Ordinal))
                .OrderByDescending(descriptor => descriptor.Weight)
                .ToArray();
            var items = ordered.Skip(offset).Take(pageSize).ToArray();
            var next = offset + items.Length < ordered.Length
                ? (offset + items.Length).ToString(System.Globalization.CultureInfo.InvariantCulture)
                : null;
            return ValueTask.FromResult(
                new ZLinkLocationPage<ZLinkClientServerServerDescriptor>(items, next));
        }
    }
}
