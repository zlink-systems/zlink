using System.Buffers.Binary;
using System.Security.Cryptography;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class CanonicalAuthorityAggregateGenerationTests
{
    [Fact]
    public void Consecutive_publications_preserve_takeover_generation_boundary()
    {
        var steady = UserSpotAuthority();
        var firstRoot = Root(long.MaxValue - 1);
        var first = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                steady,
                State(firstRoot),
                firstRoot);

        Assert.True(
            ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                first,
                out var firstPublication));
        Assert.Equal(long.MaxValue - 1UL, firstPublication.AggregateGeneration);

        var secondRoot = Root(checked(firstPublication.AggregateGeneration + 1));
        var second = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                first,
                State(secondRoot),
                secondRoot);

        Assert.True(
            ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                second,
                out var secondPublication));
        Assert.Equal((ulong)long.MaxValue, secondPublication.AggregateGeneration);
        Assert.Equal(secondRoot.AggregateId, secondPublication.AggregateId);
    }

    [Fact]
    public void Legacy_user_spot_payload_reports_unknown_generation()
    {
        var root = Root(29);
        var current = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(UserSpotAuthority(), State(root), root);
        var legacy = RemoveAggregateGenerationExtension(current);

        Assert.True(
            ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                legacy,
                out var publication));
        Assert.Equal(0UL, publication.AggregateGeneration);
    }

    [Fact]
    public void Legacy_standalone_actor_payload_recovers_known_generation()
    {
        var root = Root(1, ZLinkPlacementObjectKind.Actor);
        var current = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                ActorAuthorityAtEntrySpot(),
                State(root),
                root);
        var legacy = RemoveAggregateGenerationExtension(current);

        Assert.True(
            ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                legacy,
                out var publication));
        Assert.Equal(1UL, publication.AggregateGeneration);
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

    private static byte[] ActorAuthorityAtEntrySpot() =>
        ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                "Game.Player",
                "player-1",
                "source-entry",
                11,
                ZLinkSpotKind.Entry,
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
            "source-node",
            SourceNodeGeneration: 11,
            "source-owner",
            SourceOwnerLeaseGeneration: 7,
            "target-node",
            TargetNodeGeneration: 13,
            "target-owner",
            TargetOwnerLeaseGeneration: 9,
            ReservationGeneration: 13,
            "target-owner",
            CoordinatorLeaseGeneration: 9,
            "target-node",
            CoordinatorNodeGeneration: 13,
            Phase: 4,
            RelocationReference: "root-reference",
            RelocationChecksumCrc32c: 17,
            ApplicationVersion: 3,
            SourceCleanupState: 0);
    }

    private static byte[] RemoveAggregateGenerationExtension(
        ReadOnlySpan<byte> current)
    {
        var bodyLength = checked((int)BinaryPrimitives.ReadUInt32BigEndian(
            current.Slice(7, 4)));
        var bodyStart = 11;
        var offset = bodyStart;
        offset += 2;
        var identityLength = BinaryPrimitives.ReadUInt16BigEndian(
            current.Slice(offset, 2));
        offset += 2 + identityLength;
        offset += 1 + current[offset];
        offset += 8;
        offset += 1 + current[offset];
        offset += 1 + current[offset];
        offset += 8;
        Assert.Equal(1, current[offset]);
        var slotLengthOffset = offset + 1;
        var slotLength = checked((int)BinaryPrimitives.ReadUInt32BigEndian(
            current.Slice(slotLengthOffset, 4)));
        var slotStart = slotLengthOffset + 4;
        var extensionStart = slotStart + slotLength - 9;

        var legacy = new byte[current.Length - 9];
        current[..extensionStart].CopyTo(legacy);
        current[(extensionStart + 9)..^sizeof(uint)]
            .CopyTo(legacy.AsSpan(extensionStart));
        BinaryPrimitives.WriteUInt32BigEndian(
            legacy.AsSpan(7, 4),
            checked((uint)(bodyLength - 9)));
        BinaryPrimitives.WriteUInt32BigEndian(
            legacy.AsSpan(slotLengthOffset, 4),
            checked((uint)(slotLength - 9)));
        BinaryPrimitives.WriteUInt32BigEndian(
            legacy.AsSpan(legacy.Length - sizeof(uint)),
            ZLinkCrc32C.Compute(legacy.AsSpan(0, legacy.Length - sizeof(uint))));
        return legacy;
    }
}
