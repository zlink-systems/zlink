using System.Text.Json;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed class RedisAggregateInventoryMatrixTests
{
    private const string Prefix = "zlink:v11:";
    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    [SkippableTheory]
    [InlineData("small-3")]
    [InlineData("two-level-1025")]
    [InlineData("direct-join-1")]
    [InlineData("missing-page")]
    [InlineData("missing-participant")]
    [InlineData("damaged-participant")]
    [InlineData("damaged-page")]
    [InlineData("reordered-leaf-entries")]
    [InlineData("reordered-page-index")]
    public async Task SharedInventory(string name)
    {
        var mode = Environment.GetEnvironmentVariable("ZLINK_INVENTORY_MATRIX_MODE");
        Skip.If(
            mode is not ("write" or "read"),
            "Set the inventory matrix mode and Redis namespace."
        );
        var keyPrefix = Environment.GetEnvironmentVariable("ZLINK_INVENTORY_MATRIX_PREFIX");
        Assert.False(string.IsNullOrEmpty(keyPrefix));
        var fixture = Environment.GetEnvironmentVariable("ZLINK_INVENTORY_MATRIX_GOLDEN");
        using var golden = JsonDocument.Parse(await File.ReadAllBytesAsync(fixture!));
        var rejected = golden
            .RootElement.GetProperty("rejected")
            .EnumerateArray()
            .SingleOrDefault(item => item.GetProperty("name").GetString() == name);
        var negative = rejected.ValueKind != JsonValueKind.Undefined;
        var baseName = negative ? rejected.GetProperty("base").GetString() : name;
        var direct = baseName == "direct-join-1";
        var vector = direct
            ? golden.RootElement.GetProperty("directJoin")
            : golden
                .RootElement.GetProperty("cases")
                .EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == baseName);
        var fence = direct
            ? ZLinkAggregateInventory.RelocationFence(vector.GetProperty("relocationId").GetGuid())
            : AggregateFence(vector.GetProperty("aggregate").GetProperty("decoded"));
        var root = vector.GetProperty("root");
        var digest = root.GetProperty("decoded").GetProperty("digestSha256").GetString()!;
        var entries = vector
            .GetProperty("pages")
            .EnumerateArray()
            .SelectMany(page => page.GetProperty("decoded").GetProperty("entries").EnumerateArray())
            .Select(entry => entry.Deserialize<ZLinkAggregateInventory.Entry>(Json)!)
            .ToArray();
        var inputs = vector.GetProperty("participantHashInputs");
        var payload = Hex(inputs, "authorityPayloadHex");
        var mutation = Hex(inputs, "membershipMutationHex");
        await using var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions
            {
                ConnectionString = Environment.GetEnvironmentVariable("ZLINK_REDIS_TEST_ENDPOINT")!,
                KeyPrefix = $"{keyPrefix}:{name}",
            }
        );

        var repository = new ZLinkProviderLocationRepository(store);
        if (mode == "write")
        {
            var participants = entries
                .Reverse()
                .Select(entry => new ZLinkAggregateParticipant(
                    new ZLinkAuthorityKey(entry.AuthorityKey),
                    entry.ExpectedStoreVersion,
                    entry.OwnerTransition == "preserve"
                        ? ZLinkAuthorityGenerationTransition.Preserve
                        : ZLinkAuthorityGenerationTransition.NewOwner,
                    payload,
                    mutation
                ))
                .ToArray();
            Assert.Equal(
                digest,
                await repository.SealAggregateInventoryAsync(fence, participants, default)
            );
            foreach (var record in vector.GetProperty("pages").EnumerateArray().Prepend(root))
            {
                var key = Prefix + record.GetProperty("key").GetString()!["golden:".Length..];
                var found = Assert.IsType<ZLinkStoreReadResult.Found>(
                    await store.ReadAsync(new ZLinkStoreKey(key))
                );
                Assert.Equal(Hex(record, "jcsHex"), found.Value.Bytes.ToArray());
            }
            return;
        }

        if (negative)
        {
            var expected = rejected.TryGetProperty("rootDigestSha256", out var replacement)
                ? replacement.GetString()!
                : digest;
            await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
                repository.ReadAggregateInventoryAsync(fence, expected, default).AsTask()
            );
            return;
        }

        var actual = await repository.ReadAggregateInventoryAsync(fence, digest, default);
        Assert.Equal(entries, actual.Select(participant => participant.Entry));
        Assert.All(
            actual,
            participant =>
            {
                Assert.Equal(payload, participant.Payload.ToArray());
                Assert.Equal(mutation, participant.Mutation.ToArray());
            }
        );
    }

    private static ZLinkAggregateFence AggregateFence(JsonElement aggregate) =>
        new(
            aggregate.GetProperty("aggregateId").GetGuid(),
            ulong.Parse(aggregate.GetProperty("aggregateGeneration").GetString()!)
        );

    private static byte[] Hex(JsonElement element, string field) =>
        Convert.FromHexString(element.GetProperty(field).GetString()!);
}
