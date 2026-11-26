using System.Text.Json;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class AggregateInventoryGoldenConformanceTests
{
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
