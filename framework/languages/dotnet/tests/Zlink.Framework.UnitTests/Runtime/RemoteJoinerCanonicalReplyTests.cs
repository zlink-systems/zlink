using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RemoteJoinerCanonicalReplyTests
{
    [Fact]
    public void Empty_reply_uses_mesh_completion_content_type()
    {
        var application = ZLinkActorRemoteJoiner.DecodeCanonicalApplicationReply(
            JoinResult("application/x-protobuf"),
            Array.Empty<Message>());

        Assert.Equal("application/x-protobuf", application.ContentType);
        Assert.Empty(application.Payload.ToArray());
    }

    [Fact]
    public void Raw_reply_uses_mesh_completion_content_type_without_decode()
    {
        using var raw = Message.From(new byte[] { 0x08, 0x96, 0x01 });

        var application = ZLinkActorRemoteJoiner.DecodeCanonicalApplicationReply(
            JoinResult("application/x-protobuf"),
            [raw]);

        Assert.Equal("application/x-protobuf", application.ContentType);
        Assert.Equal(new byte[] { 0x08, 0x96, 0x01 }, application.Payload.ToArray());
    }

    [Fact]
    public void Nested_dotnet_reply_is_unwrapped_once_for_compatibility()
    {
        using var nested = Message.From(
            ZLinkApplicationPayloadEnvelopeCodec.Encode(
                "ActorJoinReply",
                "application/json",
                "{\"accepted\":true}"u8));

        var application = ZLinkActorRemoteJoiner.DecodeCanonicalApplicationReply(
            JoinResult("application/x-protobuf"),
            [nested]);

        Assert.Equal("application/json", application.ContentType);
        Assert.Equal("{\"accepted\":true}"u8.ToArray(), application.Payload.ToArray());
    }

    private static ZLinkBackendActorJoinResult JoinResult(string replyContentType) => new(
        RequestResult.Ok,
        JoinResultCode: 0,
        new ZLinkBackendActorRef(RoutingId.From("target"), "actor", 1),
        JoinedSpotId: "spot",
        JoinEpoch: 1,
        Flags: 0,
        JoinedSpotGeneration: 1,
        ReplyContentType: replyContentType);
}
