using System.Buffers.Binary;
using System.Text.Json;
using Zlink.Framework.Runtime.Actors;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorRemoteJoinRecoveryCodecBoundaryTests
{
    private const int MaximumMessageBytes = 1024 * 1024;

    [Fact]
    public void Exact_request_and_reply_limits_round_trip_together()
    {
        var request = Enumerable.Repeat((byte)0x5a, MaximumMessageBytes)
            .ToArray();
        var reply = Enumerable.Repeat((byte)0xa5, MaximumMessageBytes)
            .ToArray();

        var restored = ZLinkActorRemoteJoinRecoveryCodec.Decode(
            ZLinkActorRemoteJoinRecoveryCodec.Encode(
                CreateRecovery(request, reply)));

        Assert.Equal(request, restored.Request.Request);
        Assert.Equal(reply, restored.Reply);
    }

    [Fact]
    public void Request_above_limit_is_rejected()
    {
        var recovery = CreateRecovery(
            new byte[MaximumMessageBytes + 1],
            []);

        Assert.Throws<ArgumentOutOfRangeException>(
            () => ZLinkActorRemoteJoinRecoveryCodec.Encode(recovery));
    }

    [Fact]
    public void Reply_above_limit_is_rejected()
    {
        var recovery = CreateRecovery(
            [],
            new byte[MaximumMessageBytes + 1]);

        Assert.Throws<ArgumentOutOfRangeException>(
            () => ZLinkActorRemoteJoinRecoveryCodec.Encode(recovery));
    }

    [Fact]
    public void Operation_and_route_metadata_round_trip()
    {
        var expected = CreateRecovery([1, 2, 3], [4, 5, 6]);

        var restored = ZLinkActorRemoteJoinRecoveryCodec.Decode(
            ZLinkActorRemoteJoinRecoveryCodec.Encode(expected));

        Assert.Equal(expected.OperationIdHigh, restored.OperationIdHigh);
        Assert.Equal(expected.OperationIdLow, restored.OperationIdLow);
        Assert.Equal(expected.Request.ActorId, restored.Request.ActorId);
        Assert.Equal(expected.Request.ActorType, restored.Request.ActorType);
        Assert.Equal(expected.Request.HandoffId, restored.Request.HandoffId);
        Assert.Equal(expected.Request.SourceSpotId, restored.Request.SourceSpotId);
        Assert.Equal(expected.Request.SourceNodeRid, restored.Request.SourceNodeRid);
        Assert.Equal(expected.TargetSpotId, restored.TargetSpotId);
        Assert.Equal(expected.TargetNodeRid, restored.TargetNodeRid);
        Assert.Equal(
            expected.TargetNodeGeneration,
            restored.TargetNodeGeneration);
        Assert.Equal(
            expected.TargetSpotGeneration,
            restored.TargetSpotGeneration);
        Assert.Equal(
            expected.TargetAuthorityOwnerGeneration,
            restored.TargetAuthorityOwnerGeneration);
        Assert.Equal(
            expected.Request.BoundSessionNodeRid,
            restored.Request.BoundSessionNodeRid);
        Assert.Equal(
            expected.Request.BoundSessionRid,
            restored.Request.BoundSessionRid);
        Assert.Equal(
            expected.Request.BoundSessionBindingToken,
            restored.Request.BoundSessionBindingToken);
        Assert.Equal(
            expected.Request.BoundSessionBindingGeneration,
            restored.Request.BoundSessionBindingGeneration);
        Assert.Equal(
            expected.Request.BoundSessionAcceptedHighWater,
            restored.Request.BoundSessionAcceptedHighWater);
        Assert.Equal(expected.ReplyContentType, restored.ReplyContentType);
    }

    [Fact]
    public void Truncated_payload_is_rejected()
    {
        var encoded = ZLinkActorRemoteJoinRecoveryCodec.Encode(
            CreateRecovery([1, 2, 3], [4, 5, 6]));

        Assert.Throws<InvalidDataException>(
            () => ZLinkActorRemoteJoinRecoveryCodec.Decode(encoded[..^1]));
    }

    [Fact]
    public void Corrupt_request_length_is_rejected()
    {
        var encoded = ZLinkActorRemoteJoinRecoveryCodec.Encode(
            CreateRecovery([1, 2, 3], [4, 5, 6]));
        BinaryPrimitives.WriteUInt32BigEndian(
            encoded.AsSpan(9, sizeof(uint)),
            MaximumMessageBytes + 1U);

        Assert.Throws<InvalidDataException>(
            () => ZLinkActorRemoteJoinRecoveryCodec.Decode(encoded));
    }

    [Fact]
    public void Legacy_source_fence_v2_recovery_remains_readable()
    {
        var expected = CreateRecovery([1, 2, 3], [4, 5, 6]);
        var legacyJson = JsonSerializer.SerializeToUtf8Bytes(expected);
        var sourceFenceV1 = ZLinkActorRelocationSourceFenceCodec.Encode(
            new ZLinkActorRelocationSourceFence(
                "source-owner",
                3,
                RoutingId.From("source-node"),
                7));
        var sourceFenceV2 = new byte[
            sourceFenceV1.Length + sizeof(uint) + legacyJson.Length];
        sourceFenceV1.CopyTo(sourceFenceV2, 0);
        sourceFenceV2[4] = 2;
        BinaryPrimitives.WriteUInt32BigEndian(
            sourceFenceV2.AsSpan(sourceFenceV1.Length, sizeof(uint)),
            checked((uint)legacyJson.Length));
        legacyJson.CopyTo(
            sourceFenceV2.AsSpan(sourceFenceV1.Length + sizeof(uint)));

        var sourceFence =
            ZLinkActorRelocationSourceFenceCodec.Decode(sourceFenceV2);
        var restored = ZLinkActorRemoteJoinRecoveryCodec.Decode(
            ReadOnlySpan<byte>.Empty,
            sourceFence.LegacyRemoteJoinRecovery.Span);

        Assert.Equal(expected.Request.Request, restored.Request.Request);
        Assert.Equal(expected.Reply, restored.Reply);
        Assert.Equal(expected.OperationIdHigh, restored.OperationIdHigh);
        Assert.Equal(expected.OperationIdLow, restored.OperationIdLow);
    }

    [Fact]
    public void Null_legacy_fields_are_rejected_as_malformed_data()
    {
        var malformed = """
            {"Request":null,"TargetSpotId":"target-spot","Reply":[]}
            """u8.ToArray();

        Assert.Throws<InvalidDataException>(() =>
            ZLinkActorRemoteJoinRecoveryCodec.Decode(
                ReadOnlySpan<byte>.Empty,
                malformed));
    }

    [Fact]
    public void New_and_legacy_recovery_cannot_be_published_together()
    {
        var recovery = CreateRecovery([1], [2]);
        var current = ZLinkActorRemoteJoinRecoveryCodec.Encode(recovery);
        var legacy = JsonSerializer.SerializeToUtf8Bytes(recovery);

        Assert.Throws<InvalidDataException>(() =>
            ZLinkActorRemoteJoinRecoveryCodec.Decode(current, legacy));
    }

    private static ZLinkActorRelocationRecoveryRecord CreateRecovery(
        byte[] requestPayload,
        byte[] replyPayload)
    {
        var aggregateId =
            Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
        var sourceRid = RoutingId.From("source-node").ToBytes().ToArray();
        var targetRid = RoutingId.From("target-node").ToBytes().ToArray();
        var sessionNodeRid =
            RoutingId.From("session-node").ToBytes().ToArray();
        var sessionRid = RoutingId.From("session-1").ToBytes().ToArray();
        return new ZLinkActorRelocationRecoveryRecord(
            new ZLinkRemoteActorJoinRequest(
                "actor-1",
                "player",
                aggregateId.ToString("N"),
                sessionNodeRid,
                sessionRid,
                ZLinkRemoteActorJoinPackets.SnapshotRelocationContentType,
                "root-1",
                17,
                aggregateId,
                3,
                Enumerable.Repeat((byte)0x11, 32).ToArray(),
                "application/json",
                requestPayload,
                [],
                "source-spot",
                sourceRid,
                7,
                3,
                BoundSessionBindingToken: "binding-1",
                BoundSessionBindingGeneration: 23,
                BoundSessionObjectGeneration: 7,
                BoundSessionAuthorityOwnerGeneration: 29,
                BoundSessionMeshName: "play",
                BoundSessionTargetNodeGeneration: 31,
                BoundSessionOwnerLeaseGeneration: 37,
                BoundSessionOwnerNodeGeneration: 41,
                BoundSessionAcceptedHighWater: 43,
                ReservationToken: "reservation-1",
                ReservedPayloadBytes: requestPayload.Length,
                TargetNodeRid: targetRid,
                TargetNodeGeneration: 11,
                TargetSpotGeneration: 5,
                TargetAuthorityOwnerGeneration: 4,
                TargetSpotAuthorityOwnerGeneration: 2),
            "target-spot",
            targetRid,
            11,
            5,
            4,
            19,
            47,
            "application/json",
            replyPayload);
    }
}
