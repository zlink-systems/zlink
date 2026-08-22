using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RemoteJoinerCanonicalReplyTests
{
    // service-wire-v1 ActorJoin(28): the ZLJR saved-work `ReplyContentType`
    // recorded for command-40 recovery must always be the outer
    // framework-multipart wire profile, regardless of the reply's own
    // typed content type — see ZLinkRemoteActorAdmissionReply and the
    // canonical construction site in TryRequestCanonicalAdmissionAsync.
    [Fact]
    public async Task Canonical_admission_records_outer_profile_for_recovery_while_keeping_inner_type_for_decode()
    {
        using var raw = Message.From("{\"accepted\":true}"u8.ToArray());
        var transport = new FakeCanonicalActorJoinTransport(
            new ZLinkBackendActorJoinResult(
                RequestResult.Ok,
                JoinResultCode: 0,
                new ZLinkBackendActorRef(RoutingId.From("target"), "actor-1", 1),
                JoinedSpotId: "spot",
                JoinEpoch: 1,
                Flags: 0,
                JoinedSpotGeneration: 1,
                ReplyContentType: "application/json"),
            [raw]);

        var admission = await ZLinkActorRemoteJoiner.TryRequestCanonicalAdmissionAsync(
            transport,
            CanonicalRequest(),
            predictedPayloadBytes: 0,
            timeout: TimeSpan.FromSeconds(2),
            CancellationToken.None);

        var reply = Assert.NotNull(admission).Reply;
        // Inner: the actual application content type, still usable to
        // decode the sole raw reply part locally (DecodeAdmissionReplyPayload).
        Assert.Equal("application/json", reply.ReplyContentType);
        // Outer: the ZLJR/recovery-root profile Java's canonical adapter
        // fences as a literal wire-format invariant.
        Assert.Equal(
            ServiceWireConstants.FrameworkMultipartContentType,
            reply.RecoveryReplyContentType);
    }

    [Fact]
    public void NonCanonical_admission_reply_leaves_recovery_content_type_unset()
    {
        // The legacy router-channel/JSON admission reply (target-side
        // CreateAdmissionReply) never sets RecoveryReplyContentType — the
        // canonical-only ZLJR profile requirement does not apply there, and
        // callers fall back to ReplyContentType unchanged.
        var reply = ZLinkRemoteActorJoinPackets.CreateAdmissionReply(
            accepted: true,
            reply: null,
            new ZLinkCodecRegistryBuilder());

        Assert.Null(reply.RecoveryReplyContentType);
    }

    private static ZLinkBackendCanonicalActorJoinRequest CanonicalRequest() => new(
        new ZLinkBackendActorRef(RoutingId.From("source"), "actor-1", 1),
        ActorNodeGeneration: 1,
        ActorAuthorityOwnerGeneration: 1,
        ActorOwnerLeaseGeneration: 1,
        Entry: false,
        RoutingId.From("target"),
        TargetSpotId: "spot",
        TargetSpotGeneration: 1,
        TargetNodeGeneration: 1,
        TargetAuthorityOwnerGeneration: 1,
        TargetOwnerLeaseGeneration: 1,
        PacketName: "ZLinkFrameworkActorJoinRequest",
        ContentType: "application/json",
        ApplicationPayload: ReadOnlyMemory<byte>.Empty);

    private sealed class FakeCanonicalActorJoinTransport(
        ZLinkBackendActorJoinResult result,
        IReadOnlyList<Message> replyParts) : IZLinkBackendCanonicalActorJoin
    {
        public bool CanRequestCanonicalActorJoin(
            ZLinkBackendCanonicalActorJoinRequest request) => true;

        public bool RequestCanonicalActorJoin(
            ZLinkBackendCanonicalActorJoinRequest request,
            ActorJoinCallback callback,
            TimeSpan? timeout,
            out ulong correlation)
        {
            correlation = 1;
            callback(result, replyParts);
            return true;
        }
    }

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
