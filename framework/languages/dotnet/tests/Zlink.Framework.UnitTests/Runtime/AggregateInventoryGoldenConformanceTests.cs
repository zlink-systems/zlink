using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class AggregateInventoryGoldenConformanceTests
{
    [Fact]
    public async Task SpotWideSourceSealMatchesGolden()
    {
        using var fixture = LoadFixture();
        var vector = fixture.RootElement.GetProperty("spotWideSeal");
        var input = vector.GetProperty("input");
        var expected = vector.GetProperty("expected");
        var source = input.GetProperty("source");
        var spot = input.GetProperty("spot");
        var spotId = spot.GetProperty("id").GetString()!;
        var spotGeneration = ulong.Parse(spot.GetProperty("objectGeneration").GetString()!);
        var mesh = source.GetProperty("meshName").GetString()!;
        var rid = RoutingId.FromHex(source.GetProperty("routingIdHex").GetString()!);
        var nodeGeneration = ulong.Parse(source.GetProperty("nodeGeneration").GetString()!);
        var owner = source.GetProperty("ownerId").GetString()!;
        var lease = long.Parse(source.GetProperty("ownerLeaseGeneration").GetString()!);
        var descriptor = new ZLinkMeshNodeDescriptorKey(mesh, rid);
        var authorities = new Dictionary<ZLinkAuthorityKey, ZLinkAuthoritySnapshot>();
        void AddSourceAuthority(JsonElement row, bool isSpot)
        {
            var id = row.GetProperty("id").GetString()!;
            var type = row.GetProperty("stableType").GetString()!;
            var kind = isSpot ? ZLinkPlacementObjectKind.UserSpot : ZLinkPlacementObjectKind.Actor;
            var payload = isSpot
                ? ZLinkUserSpotAuthorityPayloadCodec.Encode(
                    new(
                        ZLinkUserSpotAuthorityState.Ready,
                        type,
                        id,
                        owner,
                        (ulong)lease,
                        mesh,
                        rid,
                        nodeGeneration
                    )
                )
                : ZLinkActorAuthorityPayloadCodec.Encode(
                    new(
                        ZLinkActorAuthorityState.Ready,
                        type,
                        id,
                        spotId,
                        spotGeneration,
                        ZLinkSpotKind.User,
                        owner,
                        (ulong)lease,
                        mesh,
                        rid,
                        nodeGeneration
                    )
                );
            authorities.Add(
                isSpot
                    ? ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(id)
                    : ZLinkActorAuthorityPayloadCodec.AuthorityKey(id),
                new(
                    row.GetProperty("expectedStoreVersion").GetString()!,
                    payload,
                    ulong.Parse(row.GetProperty("objectGeneration").GetString()!),
                    ulong.Parse(row.GetProperty("authorityOwnerGeneration").GetString()!),
                    owner,
                    lease,
                    new(
                        ZLinkPlacementAllocationState.Active,
                        kind,
                        type,
                        descriptor,
                        nodeGeneration,
                        isSpot ? new(0, 1, new(kind, type, 1)) : new(1, 0, null)
                    ),
                    null,
                    DateTimeOffset.UnixEpoch
                )
            );
        }

        var actors = input.GetProperty("actors").EnumerateArray().ToArray();
        AddSourceAuthority(spot, true);
        foreach (var actor in actors)
            AddSourceAuthority(actor, false);

        ZLinkUserSpotActivation? activation = null;
        var trace = new RelocationBehaviorTrace
        {
            InitializeTarget = (context, _) =>
            {
                activation = Assert.IsType<ZLinkUserSpotActivation>(context);
                return ValueTask.CompletedTask;
            },
        };
        await using var host = await RelocationBehaviorHost.StartAsync(
            "source",
            trace,
            new ZLinkInMemoryProviderLocationStore(),
            new SynchronizedRelocationStore(),
            registerTargetSpot: true,
            relocateSpots: true
        );
        await host
            .Services.GetRequiredService<IZLinkSpotManager>()
            .GetOrCreate(spotId, RelocationBehaviorHost.SpotType)
            .InMesh(RelocationBehaviorHost.MeshName)
            .Async();
        Assert.NotNull(activation);

        var actorManager = host.Services.GetRequiredService<IZLinkActorManager>();
        foreach (var actor in actors)
        {
            var actorId = actor.GetProperty("id").GetString()!;
            await actorManager
                .GetOrCreate(actorId, RelocationBehaviorHost.ActorType)
                .InMesh(RelocationBehaviorHost.MeshName)
                .Request(new BehaviorCreate(0))
                .Async();
            var state = host.Runtime.GetOrCreateActorState(actorId);
            Assert.NotNull(state.Actor);
            await activation.PrepareTransferredActorJoinAndReplayAsync(state.Actor, state, default);
        }

        var boundary = new SpotWideSealBoundary(authorities);
        var scheduler = new ZLinkSpotRetireScheduler(boundary, boundary, host.Runtime);
        var result = await scheduler.TryRelocateAsync(
            activation,
            false,
            default,
            DateTimeOffset.UtcNow.AddSeconds(10),
            _ => throw new InvalidOperationException("No target reserved the Spot."),
            default
        );
        Assert.Equal(ZLinkRelocationUnitOutcome.Pending, result.Outcome);
        const string prefix = SpotWideSealBoundary.Prefix;
        var fence = boundary.Fence;
        Assert.Equal(1UL, fence.AggregateGeneration);
        var store = boundary.Store;
        var digest = boundary.Digest;
        Assert.NotNull(digest);
        var page = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(ZLinkAggregateInventory.PageKey(prefix, fence, 0, 0))
        );
        var actual = await ZLinkAggregateInventory.ReadAsync(store, prefix, fence, digest, default);
        Assert.Equal(
            expected
                .GetProperty("participants")
                .EnumerateArray()
                .Select(record =>
                    (
                        record.GetProperty("authorityKey").GetString(),
                        record.GetProperty("authorityPayloadHex").GetString(),
                        record.GetProperty("membershipMutationHex").GetString(),
                        record.GetProperty("authorityPayloadSha256").GetString(),
                        record.GetProperty("membershipMutationSha256").GetString()
                    )
                ),
            actual.Select(record =>
                (
                    record.Entry.AuthorityKey,
                    Convert.ToHexString(record.Payload.Span).ToLowerInvariant(),
                    Convert.ToHexString(record.Mutation.Span).ToLowerInvariant(),
                    record.Entry.AuthorityPayloadSha256,
                    record.Entry.MembershipMutationSha256
                )
            )
        );
        Assert.Equal(
            expected.GetProperty("pageSha256").GetString(),
            ZLinkAggregateInventory.Hash(page.Value.Bytes.Span)
        );
        Assert.Equal(expected.GetProperty("rootDigestSha256").GetString(), digest);
    }

    [Fact]
    public void StandaloneActorCommitMatchesGolden()
    {
        using var fixture = LoadFixture();
        var vector = fixture.RootElement.GetProperty("standaloneActorCommit");
        var input = vector.GetProperty("input");
        var source = input.GetProperty("source");
        var target = input.GetProperty("target");
        var entry = source.GetProperty("entrySpot");
        var approved = target.GetProperty("approvedUserSpot");
        var actorId = input.GetProperty("actorId").GetString()!;
        var sourcePayload = new ZLinkActorAuthorityPayload(
            ZLinkActorAuthorityState.Ready,
            input.GetProperty("stableType").GetString()!,
            actorId,
            entry.GetProperty("id").GetString()!,
            ulong.Parse(entry.GetProperty("generation").GetString()!),
            ZLinkSpotKind.Entry,
            source.GetProperty("ownerId").GetString()!,
            ulong.Parse(source.GetProperty("ownerLeaseGeneration").GetString()!),
            source.GetProperty("meshName").GetString()!,
            RoutingId.FromHex(source.GetProperty("routingIdHex").GetString()!),
            ulong.Parse(source.GetProperty("nodeGeneration").GetString()!)
        );
        var destination = new ZLinkStandaloneActorRelocationDestination(
            approved.GetProperty("id").GetString()!,
            ulong.Parse(approved.GetProperty("generation").GetString()!),
            ZLinkSpotKind.User,
            RoutingId.FromHex(target.GetProperty("routingIdHex").GetString()!),
            ulong.Parse(target.GetProperty("nodeGeneration").GetString()!),
            target.GetProperty("meshName").GetString()!,
            new ZLinkLocationOwnerToken(
                target.GetProperty("ownerId").GetString()!,
                long.Parse(target.GetProperty("ownerLeaseGeneration").GetString()!)
            )
        );
        var payload = ZLinkActorAuthorityPayloadCodec.Encode(
            ZLinkStandaloneActorRelocationRuntime.PlanTargetAuthority(sourcePayload, destination)
        );
        var digest = ZLinkDirectRelocationInventory.Digest(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId),
            input.GetProperty("expectedStoreVersion").GetString()!,
            payload
        );
        var expected = vector.GetProperty("expected");
        Assert.Equal(
            (
                expected.GetProperty("targetAuthorityPayloadHex").GetString(),
                expected.GetProperty("digestSha256").GetString()
            ),
            (Convert.ToHexString(payload).ToLowerInvariant(), digest)
        );
    }

    [Fact]
    public void StorageKeysMatchGolden()
    {
        using var fixture = LoadFixture();
        var vector = fixture.RootElement.GetProperty("storageKeys");
        var input = vector.GetProperty("input");
        var expected = vector.GetProperty("expected");
        var prefix = input.GetProperty("prefix").GetString()!;
        var fence = new ZLinkAggregateFence(
            input.GetProperty("aggregateId").GetGuid(),
            ulong.Parse(input.GetProperty("aggregateGeneration").GetString()!)
        );
        var participantIndex = input.GetProperty("participantIndex").GetInt32();

        Assert.Equal(
            expected.GetProperty("aggregate").GetString(),
            ZLinkAggregateInventory.AggregateKey(prefix, fence).Value
        );
        Assert.Equal(
            expected.GetProperty("root").GetString(),
            ZLinkAggregateInventory.RootKey(prefix, fence).Value
        );
        Assert.Equal(
            expected.GetProperty("page").GetString(),
            ZLinkAggregateInventory
                .PageKey(
                    prefix,
                    fence,
                    input.GetProperty("level").GetInt32(),
                    input.GetProperty("pageIndex").GetInt32()
                )
                .Value
        );
        Assert.Equal(
            expected.GetProperty("authorityPayload").GetString(),
            ZLinkAggregateInventory.ParticipantKey(prefix, fence, participantIndex, false).Value
        );
        Assert.Equal(
            expected.GetProperty("membershipMutation").GetString(),
            ZLinkAggregateInventory.ParticipantKey(prefix, fence, participantIndex, true).Value
        );
    }

    [Fact]
    public async Task DirectJoinUsesRelocationIdOnInitialSealAndRetry()
    {
        using var fixture = LoadFixture();
        var vector = fixture.RootElement.GetProperty("directJoin");
        var id = vector.GetProperty("relocationId").GetGuid();
        var fence = ZLinkAggregateInventory.RelocationFence(id);
        var store = new ZLinkInMemoryProviderLocationStore();
        var participants = Participants(vector);

        for (var attempt = 0; attempt < 2; attempt++)
            Assert.Equal(
                Digest(vector),
                await ZLinkAggregateInventory.SealAsync(
                    store,
                    "golden:",
                    fence,
                    participants,
                    default
                )
            );

        var changed = participants[0] with { AuthorityPayload = new byte[] { 0xff } };
        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
            ZLinkAggregateInventory.SealAsync(store, "golden:", fence, [changed], default).AsTask()
        );

        var expected = Records(vector);
        var keys = vector.GetProperty("participantKeys")[0];
        Assert.Contains(keys.GetProperty("authorityPayload").GetString()!, expected.Keys);
        Assert.Contains(keys.GetProperty("membershipMutation").GetString()!, expected.Keys);
        var records = Assert.IsType<ZLinkStoreScanResult.Page>(
            await store.ScanAsync(new ZLinkStoreScanRequest("golden:", null, 256))
        );
        Assert.Equal(expected.Count, records.Value.Items.Count);
        foreach (var item in records.Value.Items)
            Assert.Equal(expected[item.Key.Value], item.Value.Bytes.ToArray());
    }

    [Fact]
    public async Task AbortedMultiParticipantSealLeavesNoInventoryRecords()
    {
        using var fixture = LoadFixture();
        var vector = Find(fixture.RootElement, "cases", "small-3");
        var store = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(store);
        var fence = Fence(vector);
        await repository.SealAggregateInventoryAsync(fence, Participants(vector), default);

        await repository.RemoveSealedInventoryAsync(fence, default);

        var scan = Assert.IsType<ZLinkStoreScanResult.Page>(
            await store.ScanAsync(new ZLinkStoreScanRequest("zlink:v11:", null, 256))
        );
        Assert.Empty(scan.Value.Items);
    }

    public static IEnumerable<object[]> GoldenCases()
    {
        using var fixture = LoadFixture();
        foreach (var vector in fixture.RootElement.GetProperty("cases").EnumerateArray())
            yield return [vector.GetProperty("name").GetString()!];
    }

    [Theory]
    [MemberData(nameof(GoldenCases))]
    public async Task SourceInventoryMatchesGolden(string name)
    {
        using var fixture = LoadFixture();
        var vector = Find(fixture.RootElement, "cases", name);
        var store = new ZLinkInMemoryProviderLocationStore();
        var fence = Fence(vector);
        var participants = Participants(vector);
        var digest = await ZLinkAggregateInventory.SealAsync(
            store,
            "golden:",
            fence,
            participants,
            default
        );
        Assert.Equal(Digest(vector), digest);
        var aggregate = vector.GetProperty("aggregate");
        var decoded = aggregate.GetProperty("decoded");
        var target = decoded.GetProperty("targetDescriptor");
        var owner = decoded.GetProperty("targetOwner");
        var capacity = decoded.GetProperty("capacity");
        var aggregateBody = ZLinkAggregateInventory.Describe(
            fence,
            decoded.GetProperty("state").GetString()!,
            participants.Length,
            digest,
            new ZLinkMeshNodeDescriptorKey(
                target.GetProperty("meshName").GetString()!,
                RoutingId.FromHex(target.GetProperty("routingIdHex").GetString()!)
            ),
            ulong.Parse(decoded.GetProperty("targetDescriptorLifecycleGeneration").GetString()!),
            new ZLinkCapacityVector(
                capacity.GetProperty("actors").GetInt32(),
                capacity.GetProperty("spots").GetInt32(),
                null
            ),
            new ZLinkLocationOwnerToken(
                owner.GetProperty("ownerId").GetString()!,
                long.Parse(owner.GetProperty("ownerLeaseGeneration").GetString()!)
            )
        );
        Assert.Equal(
            aggregate.GetProperty("key").GetString(),
            ZLinkAggregateInventory.AggregateKey("golden:", fence).Value
        );
        Assert.Equal(Hex(aggregate, "jcsHex"), ZLinkAggregateInventory.Encode(aggregateBody));
        var expected = Records(vector);
        var seen = 0;
        ZLinkStoreScanCursor? cursor = null;
        do
        {
            var scanned = Assert.IsType<ZLinkStoreScanResult.Page>(
                await store.ScanAsync(new ZLinkStoreScanRequest("golden:", cursor, 256))
            );
            seen += scanned.Value.Items.Count;
            foreach (var item in scanned.Value.Items)
                Assert.Equal(expected[item.Key.Value], item.Value.Bytes.ToArray());
            cursor = scanned.Value.NextCursor;
        } while (cursor is not null);
        Assert.Equal(expected.Count, seen);
        var restored = await ZLinkAggregateInventory.ReadAsync(
            store,
            "golden:",
            fence,
            digest,
            default
        );
        Assert.Equal(participants.Length, restored.Count);
    }

    [Theory]
    [InlineData("missing-page")]
    [InlineData("missing-participant")]
    [InlineData("damaged-participant")]
    [InlineData("damaged-page")]
    [InlineData("reordered-leaf-entries")]
    [InlineData("reordered-page-index")]
    public async Task TargetRejectsDamagedGolden(string name)
    {
        using var fixture = LoadFixture();
        var rejected = Find(fixture.RootElement, "rejected", name);
        var vector = Find(fixture.RootElement, "cases", rejected.GetProperty("base").GetString()!);
        var records = Records(vector);
        if (rejected.TryGetProperty("removeKeys", out var removed))
            foreach (var key in removed.EnumerateArray())
                Assert.True(records.Remove(key.GetString()!));
        if (rejected.TryGetProperty("replaceRecords", out var replaced))
            foreach (var record in replaced.EnumerateArray())
                records[record.GetProperty("key").GetString()!] = Hex(record, "hex");
        var digest = rejected.TryGetProperty("rootDigestSha256", out var hash)
            ? hash.GetString()!
            : Digest(vector);
        var store = new ZLinkInMemoryProviderLocationStore();
        foreach (var (key, bytes) in records)
            await store.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [],
                    [new ZLinkStoreMutation.Put(new ZLinkStoreKey(key), bytes, null)]
                )
            );
        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
            ZLinkAggregateInventory
                .ReadAsync(store, "golden:", Fence(vector), digest, default)
                .AsTask()
        );
    }

    [Theory]
    [InlineData(0, false)]
    [InlineData(1, true)]
    [InlineData(1024 * 1024, true)]
    [InlineData(1024 * 1024 + 1, false)]
    public async Task MembershipMutationSizeAppliesToStoredBody(int length, bool accepted)
    {
        using var fixture = LoadFixture();
        var vector = Find(fixture.RootElement, "cases", "small-3");
        var store = new ZLinkInMemoryProviderLocationStore();
        var fence = Fence(vector);
        var mutation = new byte[length];
        var participants = new[] { Participants(vector)[0] with { MembershipMutation = mutation } };
        if (!accepted)
        {
            await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
                ZLinkAggregateInventory
                    .SealAsync(store, "golden:", fence, participants, default)
                    .AsTask()
            );
            return;
        }
        var digest = await ZLinkAggregateInventory.SealAsync(
            store,
            "golden:",
            fence,
            participants,
            default
        );
        var actual = Assert.Single(
            await ZLinkAggregateInventory.ReadAsync(store, "golden:", fence, digest, default)
        );
        Assert.Equal(mutation, actual.Mutation.ToArray());
    }

    private static Dictionary<string, byte[]> Records(JsonElement vector)
    {
        var records = vector
            .GetProperty("pages")
            .EnumerateArray()
            .Append(vector.GetProperty("root"))
            .ToDictionary(
                record => record.GetProperty("key").GetString()!,
                record => Hex(record, "jcsHex")
            );
        var raw = vector.GetProperty("participantHashInputs");
        var payload = Hex(raw, "authorityPayloadHex");
        var mutation = Hex(raw, "membershipMutationHex");
        var rootKey = vector.GetProperty("root").GetProperty("key").GetString()!;
        var count = vector
            .GetProperty("root")
            .GetProperty("decoded")
            .GetProperty("totalCount")
            .GetInt32();
        for (var index = 0; index < count; index++)
        {
            var participant = rootKey[..^"root".Length] + $"participant:{index}:";
            records.Add(participant + "authorityPayload", payload);
            records.Add(participant + "membershipMutation", mutation);
        }
        return records;
    }

    private static ZLinkAggregateFence Fence(JsonElement vector)
    {
        var aggregate = vector.GetProperty("aggregate").GetProperty("decoded");
        return new ZLinkAggregateFence(
            Guid.Parse(aggregate.GetProperty("aggregateId").GetString()!),
            ulong.Parse(aggregate.GetProperty("aggregateGeneration").GetString()!)
        );
    }

    private static byte[] Hex(JsonElement element, string field) =>
        Convert.FromHexString(element.GetProperty(field).GetString()!);

    private static string Digest(JsonElement vector) =>
        vector.GetProperty("root").GetProperty("decoded").GetProperty("digestSha256").GetString()!;

    private static ZLinkAggregateParticipant[] Participants(JsonElement vector)
    {
        var raw = vector.GetProperty("participantHashInputs");
        var payload = Convert.FromHexString(raw.GetProperty("authorityPayloadHex").GetString()!);
        var mutation = Convert.FromHexString(raw.GetProperty("membershipMutationHex").GetString()!);
        return vector
            .GetProperty("pages")
            .EnumerateArray()
            .SelectMany(page => page.GetProperty("decoded").GetProperty("entries").EnumerateArray())
            .Reverse()
            .Select(entry => new ZLinkAggregateParticipant(
                new ZLinkAuthorityKey(entry.GetProperty("authorityKey").GetString()!),
                entry.GetProperty("expectedStoreVersion").GetString()!,
                entry.GetProperty("ownerTransition").GetString() == "newOwner"
                    ? ZLinkAuthorityGenerationTransition.NewOwner
                    : ZLinkAuthorityGenerationTransition.Preserve,
                payload,
                mutation
            ))
            .ToArray();
    }

    private static JsonElement Find(JsonElement root, string group, string name) =>
        root.GetProperty(group)
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == name);

    private sealed class SpotWideSealBoundary(
        IReadOnlyDictionary<ZLinkAuthorityKey, ZLinkAuthoritySnapshot> authorities
    ) : ZLinkLocationStoreTestDouble, IZLinkSpotRetireTarget
    {
        internal const string Prefix = "golden:";
        internal ZLinkInMemoryProviderLocationStore Store { get; } = new();
        internal ZLinkAggregateFence Fence { get; private set; }
        internal string? Digest { get; private set; }

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        ) =>
            ValueTask.FromResult<ZLinkAuthorityReadResult>(
                new ZLinkAuthorityReadResult.Found(authorities[key])
            );

        public override async ValueTask<string> SealAggregateInventoryAsync(
            ZLinkAggregateFence fence,
            IReadOnlyList<ZLinkAggregateParticipant> participants,
            CancellationToken cancellationToken
        )
        {
            Fence = fence;
            Digest = await ZLinkAggregateInventory.SealAsync(
                Store,
                Prefix,
                fence,
                participants,
                cancellationToken
            );
            return Digest;
        }

        public ValueTask<ZLinkSpotRetireReservation?> TryReserveAsync(
            ZLinkSpotRetireInventory inventory,
            ZLinkRelocationTargetSelection selection,
            CancellationToken cancellationToken
        )
        {
            Assert.NotNull(Digest);
            return ValueTask.FromResult<ZLinkSpotRetireReservation?>(null);
        }

        public ValueTask StageAsync(
            ZLinkSpotRetireReservation reservation,
            ZLinkPreparedSpotRetireStaging relocation,
            Func<ValueTask<IReadOnlyList<ZLinkRelocationQueuedJob>>> freezeBoundary,
            DateTimeOffset deadline,
            CancellationToken cancellationToken
        ) => throw new NotSupportedException();

        public ValueTask<ulong> PublishAsync(
            ZLinkSpotRetireReservation reservation,
            ZLinkAggregateRelocationPublished relocation,
            DateTimeOffset restoreDeadline,
            CancellationToken cancellationToken
        ) => throw new NotSupportedException();

        public ValueTask AbortAsync(
            ZLinkSpotRetireReservation reservation,
            ZLinkAggregateFence? fence
        ) => throw new NotSupportedException();

        public ValueTask RelayCommittedAsync(
            ZLinkSpotRetireReservation reservation,
            ZLinkAggregateRelocationPublished relocation,
            IReadOnlyList<ZLinkAcceptedWorkRecord> held,
            CancellationToken cancellationToken
        ) => throw new NotSupportedException();
    }

    private static JsonDocument LoadFixture() =>
        JsonDocument.Parse(
            File.ReadAllBytes(
                Path.Combine(
                    Common.FrameworkTestEnvironment.GetRepoRoot(),
                    "framework/runtime/protocol/golden/aggregate-storage-v1.json"
                )
            )
        );
}
