using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

//  service-wire-v1.schema.json actor-join-reply-tail (reply(20),
//  originalOperationKind actorJoin): golden vectors shared across the four
//  language runtimes at framework/runtime/protocol/golden/actor-join-reply-v1.json.
public sealed class ServiceWireActorJoinReplyCodecTests
{
    [Fact]
    public void Generated_request_codec_matches_the_golden_and_rejects_malformed_vectors()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(RequestGoldenPath()));
        var root = document.RootElement;

        foreach (var vector in root.GetProperty("valid").EnumerateArray())
        {
            var expected = vector.GetProperty("framesHex").EnumerateArray()
                .Select(static frame => Convert.FromHexString(frame.GetString()!))
                .ToArray();
            var encoded = ServiceWirePilotCodec.EncodeActorJoin28(
                ReadGeneratedRequest(vector.GetProperty("input")));

            Assert.Equal(expected, encoded);
            var decoded = ServiceWirePilotCodec.DecodeActorJoin28(expected);
            Assert.Equal(encoded, ServiceWirePilotCodec.EncodeActorJoin28(decoded));
        }

        foreach (var vector in root.GetProperty("invalid").EnumerateArray())
        {
            var malformed = vector.GetProperty("framesHex").EnumerateArray()
                .Select(static frame => Convert.FromHexString(frame.GetString()!))
                .ToArray();
            Assert.Throws<InvalidDataException>(() =>
                ServiceWirePilotCodec.DecodeActorJoin28(malformed));
        }

        var nulInActorId = Convert.FromHexString(root.GetProperty("valid")[0]
            .GetProperty("framesHex")[0].GetString()!);
        // Prefix (5), correlation (8), then text8 length (1).
        nulInActorId[14] = 0;
        Assert.Throws<InvalidDataException>(() =>
            ServiceWirePilotCodec.DecodeActorJoin28([nulInActorId]));
    }

    [Fact]
    public void Request_round_trips_the_schema_actor_and_spot_route_fences()
    {
        var actorNode = RoutingId.From(new byte[] { 1, 2, 3 });
        var targetNode = RoutingId.From(new byte[] { 4, 5, 6 });
        var request = new ActorJoinRequest(
            42,
            new ActorRef("actor-1", 7, "mesh", actorNode),
            8,
            9,
            10,
            Entry: true,
            "spot-1",
            11,
            targetNode,
            12,
            13,
            14);

        var frames = new[]
        {
            Message.From(ZLinkMeshRecordAdapters.EncodeCanonicalActorJoinHead(request))
        };
        try
        {
            var decoded = ZLinkMeshRecordAdapters.TryDecodeCanonicalActorJoin(
                frames, "mesh");

            Assert.NotNull(decoded);
            Assert.Equal(request, decoded!.Request.Request);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(frames);
        }
    }

    [Fact]
    public void Canonical_sender_uses_the_direct_application_payload_frame()
    {
        var actorNode = RoutingId.From(new byte[] { 1, 2, 3 });
        var request = new ActorJoinRequest(
            42,
            new ActorRef("actor-1", 7, "mesh", actorNode),
            8, 9, 10, false,
            "spot-1", 11, RoutingId.From(new byte[] { 4, 5, 6 }), 12, 13, 14);
        var application = ZLinkApplicationPayloadEnvelopeCodec.Encode(
            "ZLinkFrameworkActorJoinRequest",
            "application/json",
            "{\"request\":true}"u8);
        var frames = new[]
        {
            Message.From(ZLinkMeshRecordAdapters.EncodeCanonicalActorJoinHead(request)),
            Message.From(application)
        };

        try
        {
            var decoded = ZLinkMeshRecordAdapters.TryDecodeCanonicalActorJoin(
                frames, "mesh");

            Assert.NotNull(decoded);
            Assert.Equal(request, decoded!.Request.Request);
            Assert.Equal("ZLinkFrameworkActorJoinRequest", decoded.Payload!.Value.PacketName);
            Assert.Equal("application/json", decoded.Payload!.Value.ContentType);
            Assert.Equal("{\"request\":true}"u8.ToArray(),
                decoded.Payload!.Value.Payload.ToArray());
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(frames);
        }
    }

    [Fact]
    public void Canonical_handoff_identity_is_deterministic_local_bookkeeping()
    {
        var source = RoutingId.From(new byte[] { 1, 2, 3 });
        var handoff = ZLinkRemoteActorJoinPackets.CreateCanonicalHandoffId(
            source, "actor-1", 7, 8, 42);

        Assert.Equal(handoff,
            ZLinkRemoteActorJoinPackets.CreateCanonicalHandoffId(
                source, "actor-1", 7, 8, 42));
        Assert.True(Guid.TryParseExact(handoff, "N", out _));
        Assert.NotEqual(handoff,
            ZLinkRemoteActorJoinPackets.CreateCanonicalHandoffId(
                source, "actor-1", 7, 8, 43));
        Assert.NotEqual(handoff,
            ZLinkRemoteActorJoinPackets.CreateCanonicalHandoffId(
                source, "actor-2", 7, 8, 42));
        Assert.NotEqual(handoff,
            ZLinkRemoteActorJoinPackets.CreateCanonicalHandoffId(
                source, "actor-1", 8, 8, 42));
        Assert.NotEqual(handoff,
            ZLinkRemoteActorJoinPackets.CreateCanonicalHandoffId(
                source, "actor-1", 7, 9, 42));
    }

    [Fact]
    public void Request_rejects_a_non_bool_entry_discriminant()
    {
        var encoded = ZLinkMeshRecordAdapters.EncodeCanonicalActorJoinHead(
            new ActorJoinRequest(
                1,
                new ActorRef("actor", 1, "mesh", RoutingId.From(new byte[] { 1 })),
                1, 1, 1, false, "spot", 1,
                RoutingId.From(new byte[] { 2 }), 1, 1, 1));
        // Prefix (5), correlation (8), actor text/ref/fence (1+5+8+2+8+8+8).
        encoded[53] = 2;

        Assert.Throws<InvalidDataException>(() =>
            ServiceWirePilotCodec.DecodeActorJoin28([encoded]));
    }

    [Theory]
    [InlineData((byte)' ')]
    [InlineData((byte)0)]
    public void Request_applies_schema_text_boundary_for_actor_id(byte actorIdByte)
    {
        var encoded = ZLinkMeshRecordAdapters.EncodeCanonicalActorJoinHead(
            new ActorJoinRequest(
                1,
                new ActorRef("x", 1, "mesh", RoutingId.From(new byte[] { 1 })),
                1, 1, 1, false, "spot", 1,
                RoutingId.From(new byte[] { 2 }), 1, 1, 1));
        // Prefix (5), correlation (8), then text8 length (1).
        encoded[14] = actorIdByte;

        if (actorIdByte == 0)
            Assert.Throws<InvalidDataException>(() =>
                ServiceWirePilotCodec.DecodeActorJoin28([encoded]));
        else
            Assert.Equal(" ", ServiceWirePilotCodec.DecodeActorJoin28([encoded]).Actor.Id);
    }

    [Theory]
    [InlineData((byte)' ')]
    [InlineData((byte)0)]
    public void Request_applies_schema_text_boundary_for_target_spot_id(byte targetSpotIdByte)
    {
        var encoded = ZLinkMeshRecordAdapters.EncodeCanonicalActorJoinHead(
            new ActorJoinRequest(
                1,
                new ActorRef("actor", 1, "mesh", RoutingId.From(new byte[] { 1 })),
                1, 1, 1, false, "target", 1,
                RoutingId.From(new byte[] { 2 }), 1, 1, 1));
        var targetOffset = encoded.AsSpan().IndexOf("target"u8);
        Assert.True(targetOffset >= 0);
        encoded.AsSpan(targetOffset, "target"u8.Length).Fill(targetSpotIdByte);

        if (targetSpotIdByte == 0)
            Assert.Throws<InvalidDataException>(() =>
                ServiceWirePilotCodec.DecodeActorJoin28([encoded]));
        else
            Assert.Equal(new string(' ', "target".Length),
                ServiceWirePilotCodec.DecodeActorJoin28([encoded]).TargetSpot.Id);
    }

    [Fact]
    public void Accepted_canonical_vectors_round_trip_field_for_field()
    {
        AssertAcceptedRoundTrip(
            "actorJoinAcceptedTypical",
            42,
            "spot-1",
            3,
            5,
            32_768);

        AssertAcceptedRoundTrip(
            "actorJoinAcceptedAtRelocationChunkLimitBound",
            9_223_372_036_854_775_807UL,
            "spot-max",
            9_223_372_036_854_775_807UL,
            9_223_372_036_854_775_807UL,
            ZLinkServiceWireCodec.RelocationChunkBytesBound);

        AssertAcceptedRoundTrip(
            "actorJoinAcceptedNotAdvertised",
            7,
            "spot-7",
            1,
            1,
            0);
    }

    [Fact]
    public void Rejected_canonical_vectors_round_trip_field_for_field()
    {
        var withSpot = ReadGolden("actorJoinRejectedWithSpot");
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
            withSpot, out var withSpotReply, out var replyError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, replyError);
        Assert.Equal(8UL, withSpotReply.Correlation);
        Assert.Equal((int)RequestResult.Ok, withSpotReply.TerminalResult);
        Assert.True(ZLinkServiceWireCodec.TryDecodeActorJoinReply(
            withSpotReply, out var withSpotCompletion, out var decodeError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, decodeError);
        Assert.NotNull(withSpotCompletion);
        Assert.Equal(ActorJoinResult.Rejected, withSpotCompletion!.JoinResult);
        Assert.Equal(new ActorJoinReplySpot("spot-2", 7), withSpotCompletion.Spot);
        Assert.Equal(0UL, withSpotCompletion.MembershipEpoch);
        Assert.Equal(0U, withSpotCompletion.ReceiveChunkLimitBytes);
        Assert.Equal(
            withSpot,
            ZLinkServiceWireCodec.EncodeActorJoinReply(
                withSpotReply.Correlation,
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                withSpotCompletion));

        var withoutSpot = ReadGolden("actorJoinRejectedWithoutSpot");
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
            withoutSpot, out var withoutSpotReply, out _));
        Assert.True(ZLinkServiceWireCodec.TryDecodeActorJoinReply(
            withoutSpotReply, out var withoutSpotCompletion, out var withoutSpotError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, withoutSpotError);
        Assert.NotNull(withoutSpotCompletion);
        Assert.Equal(ActorJoinResult.Rejected, withoutSpotCompletion!.JoinResult);
        Assert.Null(withoutSpotCompletion.Spot);
        Assert.Equal(
            withoutSpot,
            ZLinkServiceWireCodec.EncodeActorJoinReply(
                withoutSpotReply.Correlation,
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                withoutSpotCompletion));
    }

    [Fact]
    public void Malformed_vectors_are_rejected_with_the_expected_error()
    {
        foreach (var (name, expected) in new[]
                 {
                     ("actorJoinAcceptedTruncatedReceiveChunkLimit",
                         ZLinkServiceWireCodec.DecodeError.TruncatedField),
                     ("actorJoinAcceptedZeroMembershipEpoch",
                         ZLinkServiceWireCodec.DecodeError.InvalidField),
                     ("actorJoinInvalidJoinResultDiscriminant",
                         ZLinkServiceWireCodec.DecodeError.InvalidField),
                     ("actorJoinAcceptedReceiveChunkLimitExceedsBound",
                         ZLinkServiceWireCodec.DecodeError.InvalidField),
                     ("actorJoinAcceptedTrailingByte",
                         ZLinkServiceWireCodec.DecodeError.TrailingByte),
                     ("actorJoinAcceptedBodyLengthTooLarge",
                         ZLinkServiceWireCodec.DecodeError.InvalidField)
                 })
        {
            var bytes = ReadMalformedGolden(name);
            Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
                bytes, out var reply, out var replyError));
            Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, replyError);
            Assert.False(ZLinkServiceWireCodec.TryDecodeActorJoinReply(
                reply, out var completion, out var error));
            Assert.Null(completion);
            Assert.Equal(expected, error);
        }
    }

    [Fact]
    public void Forbidden_flag_vector_is_rejected_by_the_outer_reply_decode()
    {
        var bytes = ReadMalformedGolden("actorJoinAcceptedForbiddenFlag");
        Assert.False(ZLinkServiceWireCodec.TryDecodeReply(
            bytes, out _, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.ForbiddenFlag, error);
    }

    [Fact]
    public void Encode_rejects_out_of_range_completions()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeActorJoinReply(
                1,
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new ActorJoinReplyCompletion(
                    ActorJoinResult.Accepted,
                    new ActorJoinReplySpot("spot-1", 1),
                    5,
                    ZLinkServiceWireCodec.RelocationChunkBytesBound + 1)));

        Assert.Throws<ArgumentOutOfRangeException>(() =>
            ZLinkServiceWireCodec.EncodeActorJoinReply(
                1,
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new ActorJoinReplyCompletion(
                    ActorJoinResult.Accepted,
                    null,
                    5,
                    0)));

        Assert.Throws<ArgumentException>(() =>
            ZLinkServiceWireCodec.EncodeActorJoinReply(
                1,
                RequestResult.Rejected,
                ServiceWireConstants.FrameworkErrorCode.None,
                new ActorJoinReplyCompletion(
                    ActorJoinResult.Rejected,
                    null,
                    0,
                    0)));
    }

    private static void AssertAcceptedRoundTrip(
        string name,
        ulong correlation,
        string spotId,
        ulong spotGeneration,
        ulong membershipEpoch,
        uint receiveChunkLimitBytes)
    {
        var golden = ReadGolden(name);
        Assert.True(ZLinkServiceWireCodec.TryDecodeReply(
            golden, out var reply, out var replyError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, replyError);
        Assert.Equal(correlation, reply.Correlation);
        Assert.Equal((int)RequestResult.Ok, reply.TerminalResult);
        Assert.Equal(0U, reply.FailureCode);

        Assert.True(ZLinkServiceWireCodec.TryDecodeActorJoinReply(
            reply, out var completion, out var decodeError));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, decodeError);
        Assert.NotNull(completion);
        Assert.Equal(ActorJoinResult.Accepted, completion!.JoinResult);
        Assert.Equal(new ActorJoinReplySpot(spotId, spotGeneration), completion.Spot);
        Assert.Equal(membershipEpoch, completion.MembershipEpoch);
        Assert.Equal(receiveChunkLimitBytes, completion.ReceiveChunkLimitBytes);

        var reencoded = ZLinkServiceWireCodec.EncodeActorJoinReply(
            correlation,
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            completion);
        Assert.Equal(golden, reencoded);
    }

    private static byte[] ReadGolden(string name) =>
        ReadGolden("canonical", name);

    private static string RequestGoldenPath()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        return Path.GetFullPath(
            "../../runtime/protocol/golden/actor-join-request-v1.json",
            frameworkRoot);
    }

    private static ServiceWirePilotCodec.ActorJoin28 ReadGeneratedRequest(
        JsonElement input)
    {
        var payload = input.TryGetProperty("payload", out var payloadElement)
            ? new ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1(
                payloadElement.GetProperty("packetName").GetString()!,
                payloadElement.GetProperty("contentType").GetString()!,
                Convert.FromHexString(payloadElement.GetProperty("payloadHex").GetString()!))
            : null;
        return new ServiceWirePilotCodec.ActorJoin28(
            ulong.Parse(input.GetProperty("correlation").GetString()!),
            ReadFence(input.GetProperty("actor")),
            input.GetProperty("entry").GetBoolean(),
            ReadFence(input.GetProperty("targetSpot")),
            payload);
    }

    private static ServiceWirePilotCodec.Fence ReadFence(JsonElement input) => new(
        input.GetProperty("id").GetString()!,
        ulong.Parse(input.GetProperty("generation").GetString()!),
        Convert.FromHexString(input.GetProperty("targetNodeRidHex").GetString()!),
        ulong.Parse(input.GetProperty("targetNodeGeneration").GetString()!),
        ulong.Parse(input.GetProperty("expectedAuthorityOwnerGeneration").GetString()!),
        ulong.Parse(input.GetProperty("expectedOwnerLeaseGeneration").GetString()!));

    private static byte[] ReadMalformedGolden(string name) =>
        ReadGolden("malformed", name);

    private static byte[] ReadGolden(string section, string name)
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var fixturePath = Path.GetFullPath(
            "../../runtime/protocol/golden/actor-join-reply-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(fixturePath));
        var fixture = document.RootElement.GetProperty(section)
            .EnumerateArray()
            .Single(item => item.GetProperty("name").GetString() == name);
        return Convert.FromHexString(fixture.GetProperty("hex").GetString()!);
    }
}
