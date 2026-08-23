using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text.Json;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class CanonicalAuthorityAggregateGenerationTests
{
    [Fact]
    public void Maximum_issued_generation_is_accepted_and_exhausted_sentinel_is_rejected()
    {
        var steady = UserSpotAuthority();
        var issuedRoot = Root((ulong)long.MaxValue - 1);
        var issued = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                steady,
                State(issuedRoot),
                issuedRoot);

        Assert.True(
            ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                issued,
                out var publication));
        Assert.Equal((ulong)long.MaxValue - 1, publication.AggregateGeneration);

        var exhaustedRoot = Root((ulong)long.MaxValue);
        Assert.Throws<ArgumentException>(() =>
            ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
                issued,
                State(exhaustedRoot),
                exhaustedRoot));
    }

    [Fact]
    public void Golden_legacy_dotnet_node_slot_is_rejected()
    {
        AssertGoldenRejected("legacyDotnetNodeTrailingTagU64");
    }

    [Fact]
    public void Golden_legacy_java_slot_is_rejected()
    {
        AssertGoldenRejected("legacyJavaThirdU64Layout");
    }

    private static byte[] UserSpotAuthority() =>
        ZLinkUserSpotAuthorityPayloadCodec.Encode(
            new ZLinkUserSpotAuthorityPayload(
                ZLinkUserSpotAuthorityState.Ready,
                "Game.Room",
                "room-1",
                "source-owner",
                7,
                "mesh",
                RoutingId.From("source-node"),
                11));

    private static ZLinkRelocationEnvelope Root(
        ulong aggregateGeneration,
        ZLinkPlacementObjectKind kind = ZLinkPlacementObjectKind.UserSpot)
    {
        var key = new ZLinkAuthorityKey(
            kind == ZLinkPlacementObjectKind.Actor
                ? "actor:player-1"
                : "spot:room-1");
        var participant = new ZLinkRelocationParticipantEnvelope(
            key,
            kind,
            5,
            11,
            new byte[] { 1 },
            [],
            [])
        {
            CanonicalParticipantId = 1
        };
        return new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            aggregateGeneration,
            SHA256.HashData([2]),
            [participant]);
    }

    private static ZLinkCanonicalRelocationAuthorityState State(
        ZLinkRelocationEnvelope root)
    {
        Span<byte> id = stackalloc byte[16];
        root.AggregateId.TryWriteBytes(id, bigEndian: true, out _);
        return new ZLinkCanonicalRelocationAuthorityState(
            BinaryPrimitives.ReadUInt64BigEndian(id),
            BinaryPrimitives.ReadUInt64BigEndian(id[8..]),
            TargetAttemptGeneration: 1,
            RoutingId.From("source-node").ToHex(),
            SourceNodeGeneration: 11,
            "source-owner",
            SourceOwnerLeaseGeneration: 7,
            RoutingId.From("target-node").ToHex(),
            TargetNodeGeneration: 13,
            "target-owner",
            TargetOwnerLeaseGeneration: 9,
            "target-owner",
            CoordinatorLeaseGeneration: 9,
            RoutingId.From("target-node").ToHex(),
            CoordinatorNodeGeneration: 13,
            Phase: 4,
            ApplicationVersion: 3)
        {
            AggregateGeneration = root.AggregateGeneration,
            RelocationReference = "root-reference",
            RelocationChecksumCrc32c = 17
        };
    }

    private static void AssertGoldenRejected(string name)
    {
        var path = Path.Combine(
            Common.FrameworkTestEnvironment.GetRepoRoot(),
            "framework/runtime/protocol/golden/authority-relocation-state-v1.json");
        using var fixture = JsonDocument.Parse(File.ReadAllText(path));
        var vector = fixture.RootElement.GetProperty("invalid")
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == name);
        Assert.False(ZLinkCanonicalRelocationAuthorityStateCodec.TryReadSlot(
            Convert.FromHexString(vector.GetProperty("hex").GetString()!),
            ulong.Parse(vector.GetProperty("rootAggregateGeneration").GetString()!),
            out _));
    }
}
