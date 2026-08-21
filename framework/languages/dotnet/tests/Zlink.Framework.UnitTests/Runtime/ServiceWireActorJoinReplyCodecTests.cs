using System.Text.Json;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.UnitTests;

//  service-wire-v1.schema.json actor-join-reply-tail (reply(20),
//  originalOperationKind actorJoin): golden vectors shared across the four
//  language runtimes at framework/runtime/protocol/golden/actor-join-reply-v1.json.
public sealed class ServiceWireActorJoinReplyCodecTests
{
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

        var encoded = ZLinkServiceWireCodec.EncodeActorJoinRequest(request);

        Assert.True(ZLinkServiceWireCodec.TryDecodeActorJoinRequest(
            encoded, "mesh", out var decoded, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, error);
        Assert.Equal(request, decoded.Request);
    }

    [Fact]
    public void Request_rejects_a_non_bool_entry_discriminant()
    {
        var encoded = ZLinkServiceWireCodec.EncodeActorJoinRequest(
            new ActorJoinRequest(
                1,
                new ActorRef("actor", 1, "mesh", RoutingId.From(new byte[] { 1 })),
                1, 1, 1, false, "spot", 1,
                RoutingId.From(new byte[] { 2 }), 1, 1, 1));
        // Prefix (5), correlation (8), actor text/ref/fence (1+5+8+2+8+8+8).
        encoded[53] = 2;

        Assert.False(ZLinkServiceWireCodec.TryDecodeActorJoinRequest(
            encoded, "mesh", out _, out var error));
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.InvalidField, error);
    }

    [Theory]
    [InlineData((byte)' ')]
    [InlineData((byte)0)]
    public void Request_rejects_non_boundary_actor_id(byte actorIdByte)
    {
        var encoded = ZLinkServiceWireCodec.EncodeActorJoinRequest(
            new ActorJoinRequest(
                1,
                new ActorRef("x", 1, "mesh", RoutingId.From(new byte[] { 1 })),
                1, 1, 1, false, "spot", 1,
                RoutingId.From(new byte[] { 2 }), 1, 1, 1));
        // Prefix (5), correlation (8), then text8 length (1).
        encoded[14] = actorIdByte;

        Assert.False(ZLinkServiceWireCodec.TryDecodeActorJoinRequest(
            encoded, "mesh", out _, out var error));
        Assert.NotEqual(ZLinkServiceWireCodec.DecodeError.None, error);
    }

    [Theory]
    [InlineData((byte)' ')]
    [InlineData((byte)0)]
    public void Request_rejects_non_boundary_target_spot_id(byte targetSpotIdByte)
    {
        var encoded = ZLinkServiceWireCodec.EncodeActorJoinRequest(
            new ActorJoinRequest(
                1,
                new ActorRef("actor", 1, "mesh", RoutingId.From(new byte[] { 1 })),
                1, 1, 1, false, "target", 1,
                RoutingId.From(new byte[] { 2 }), 1, 1, 1));
        var targetOffset = encoded.AsSpan().IndexOf("target"u8);
        Assert.True(targetOffset >= 0);
        encoded.AsSpan(targetOffset, "target"u8.Length).Fill(targetSpotIdByte);

        Assert.False(ZLinkServiceWireCodec.TryDecodeActorJoinRequest(
            encoded, "mesh", out _, out var error));
        Assert.NotEqual(ZLinkServiceWireCodec.DecodeError.None, error);
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
