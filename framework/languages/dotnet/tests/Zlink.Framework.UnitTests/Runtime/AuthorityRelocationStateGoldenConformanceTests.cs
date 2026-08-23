using System.Text.Json;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class AuthorityRelocationStateGoldenConformanceTests
{
    [Fact]
    public void Production_slot_codec_matches_all_authority_relocation_vectors()
    {
        using var fixture = LoadFixture();
        var root = fixture.RootElement;
        Assert.Equal("authority-relocation-state-v1",
            root.GetProperty("format").GetString());
        Assert.Contains("dotnet", root.GetProperty("consumers")
            .EnumerateArray().Select(static item => item.GetString()));

        var valid = root.GetProperty("valid").EnumerateArray().ToArray();
        Assert.Equal(6, valid.Length);
        foreach (var vector in valid)
        {
            var expected = State(vector.GetProperty("decoded"));
            var rootGeneration = RootGeneration(vector);
            var bytes = Convert.FromHexString(
                vector.GetProperty("hex").GetString()!);

            Assert.Equal(bytes,
                ZLinkCanonicalRelocationAuthorityStateCodec.EncodeSlot(
                    expected, rootGeneration));
            Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryReadSlot(
                bytes, rootGeneration, out var decoded),
                vector.GetProperty("name").GetString());
            Assert.Equal(expected, decoded);
            Assert.Equal(expected.AggregateGeneration,
                decoded.AggregateGeneration);
            Assert.Equal(expected.RelocationReference,
                decoded.RelocationReference);
            Assert.Equal(expected.RelocationChecksumCrc32c,
                decoded.RelocationChecksumCrc32c);
            Assert.Equal(expected.CoordinatorExpectedAuthorityStoreVersion,
                decoded.CoordinatorExpectedAuthorityStoreVersion);
            Assert.Equal(expected.SourceCleanupState,
                decoded.SourceCleanupState);
        }

        var invalid = root.GetProperty("invalid").EnumerateArray().ToArray();
        Assert.Equal(6, invalid.Length);
        foreach (var vector in invalid)
        {
            Assert.False(ZLinkCanonicalRelocationAuthorityStateCodec.TryReadSlot(
                    Convert.FromHexString(
                        vector.GetProperty("hex").GetString()!),
                    RootGeneration(vector),
                    out _),
                vector.GetProperty("name").GetString());
        }
    }

    private static ZLinkCanonicalRelocationAuthorityState State(JsonElement value)
    {
        var relocation = value.GetProperty("relocation");
        return new ZLinkCanonicalRelocationAuthorityState(
            ulong.Parse(relocation.GetProperty("high").GetString()!),
            ulong.Parse(relocation.GetProperty("low").GetString()!),
            U64(value, "targetAttemptGeneration"),
            value.GetProperty("sourceNodeRidHex").GetString()!,
            U64(value, "sourceNodeGeneration"),
            value.GetProperty("sourceOwnerId").GetString()!,
            U64(value, "sourceOwnerLeaseGeneration"),
            value.GetProperty("targetNodeRidHex").GetString()!,
            U64(value, "targetNodeGeneration"),
            value.GetProperty("targetOwnerId").GetString()!,
            U64(value, "targetOwnerLeaseGeneration"),
            value.GetProperty("coordinatorOwnerId").GetString()!,
            U64(value, "coordinatorLeaseGeneration"),
            value.GetProperty("coordinatorNodeRidHex").GetString()!,
            U64(value, "coordinatorNodeGeneration"),
            Phase(value.GetProperty("phase").GetString()!),
            long.Parse(value.GetProperty("applicationVersion").GetString()!))
        {
            AggregateGeneration = U64(value, "aggregateGeneration"),
            RelocationReference =
                value.GetProperty("relocationReference").GetString()!,
            RelocationChecksumCrc32c =
                value.GetProperty("relocationChecksumCrc32c").GetUInt32(),
            CoordinatorExpectedAuthorityStoreVersion = value
                .GetProperty("coordinatorExpectedStoreVersion").GetString()!,
            SourceCleanupState = Cleanup(
                value.GetProperty("sourceCleanupState").GetString()!)
        };
    }

    private static ulong U64(JsonElement value, string name) =>
        ulong.Parse(value.GetProperty(name).GetString()!);

    private static ulong? RootGeneration(JsonElement vector)
    {
        var value = vector.GetProperty("rootAggregateGeneration");
        return value.ValueKind == JsonValueKind.Null
            ? null
            : ulong.Parse(value.GetString()!);
    }

    private static byte Phase(string value) => value switch
    {
        "preparing" => 1,
        "captured" => 2,
        "prepared" => 3,
        "committed" => 4,
        "activating" => 5,
        "activated" => 6,
        "cleaning" => 7,
        "completed" => 8,
        "aborted" => 9,
        _ => throw new InvalidDataException($"Unknown relocation phase '{value}'.")
    };

    private static byte Cleanup(string value) => value switch
    {
        "pending" => 0,
        "completed" => 1,
        "sourceLeaseExpired" => 2,
        _ => throw new InvalidDataException($"Unknown cleanup state '{value}'.")
    };

    private static JsonDocument LoadFixture()
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/protocol/golden/authority-relocation-state-v1.json");
        return JsonDocument.Parse(File.ReadAllText(path));
    }
}
