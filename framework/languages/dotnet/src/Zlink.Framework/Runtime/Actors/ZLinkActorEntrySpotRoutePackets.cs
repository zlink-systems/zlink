namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkActorEntrySpotRoutePackets
{
    public const string JoinEntrySpotPacketName = "__zlink.actor.joinEntrySpot";

    public static ZLinkActorEntrySpotRouteJoinRequest CreateJoinRequest(
        string actorId,
        string actorType,
        ZLinkBackendActorRef sourceActorRef,
        string? previousSpotId,
        ZLinkMessage request,
        ZLinkCodecRegistryBuilder codecs)
    {
        var encodedRequest = request.Encode(codecs);
        return new ZLinkActorEntrySpotRouteJoinRequest(
            actorId,
            actorType,
            sourceActorRef.NodeRid.ToHex(),
            previousSpotId ?? string.Empty,
            sourceActorRef.Generation,
            encodedRequest.ContentType,
            encodedRequest.Payload.ToArray());
    }

    public static ZLinkActorEntrySpotRouteJoinRequest DecodeJoinRequest(
        IReadOnlyList<Message> parts)
    {
        return (ZLinkActorEntrySpotRouteJoinRequest?)ZLinkEnvelopeCodec.DecodeBody(
                   parts,
                   typeof(ZLinkActorEntrySpotRouteJoinRequest))
               ?? throw new InvalidOperationException("Actor EntrySpot route join request was empty.");
    }

    public static ZLinkMessage DecodeJoinRequestPayload(
        ZLinkActorEntrySpotRouteJoinRequest request,
        ZLinkCodecRegistryBuilder codecs)
    {
        using var payload = Message.From(request.RequestPayload);
        return ZLinkMessage.FromEnvelopePayload(
            request.RequestContentType,
            payload,
            codecs);
    }

    public static Message EncodeJoinReply(
        bool accepted,
        string actorId,
        string actorType,
        ZLinkBackendActorRef actorRef,
        ZLinkMessage? reply,
        ZLinkCodecRegistryBuilder codecs)
    {
        var replyContentType = ZLinkEnvelopeCodec.DefaultContentType;
        Message? replyPayload = null;
        if (reply is not null)
        {
            var encodedReply = reply.Encode(codecs);
            replyContentType = encodedReply.ContentType;
            replyPayload = Message.From(encodedReply.Payload.Bytes.Span);
        }

        using (replyPayload)
        {
            return ZLinkEnvelopeCodec.EncodePart(new ZLinkActorEntrySpotRouteJoinReply(
                accepted,
                actorId,
                actorType,
                actorRef.NodeRid.ToHex(),
                actorRef.Generation,
                replyContentType,
                replyPayload?.ToArray() ?? Array.Empty<byte>()));
        }
    }

    public static ZLinkMessage DecodeJoinReplyPayload(
        ZLinkActorEntrySpotRouteJoinReply reply,
        ZLinkCodecRegistryBuilder codecs)
    {
        using var payload = Message.From(reply.ReplyPayload);
        return ZLinkMessage.FromEnvelopePayload(
            reply.ReplyContentType,
            payload,
            codecs);
    }

    public static ZLinkBackendActorRef ToActorRef(
        ZLinkActorEntrySpotRouteJoinReply reply)
    {
        return new ZLinkBackendActorRef(
            RoutingId.From(reply.TargetNodeRid),
            reply.ActorId,
            reply.ActorGeneration);
    }
}

internal sealed record ZLinkActorEntrySpotRouteJoinRequest(
    string ActorId,
    string ActorType,
    string SourceNodeRid,
    string SourceSpotId,
    ulong SourceGeneration,
    string RequestContentType,
    byte[] RequestPayload);

internal sealed record ZLinkActorEntrySpotRouteJoinReply(
    bool Accepted,
    string ActorId,
    string ActorType,
    string TargetNodeRid,
    ulong ActorGeneration,
    string ReplyContentType,
    byte[] ReplyPayload);

internal sealed record ZLinkActorJoinSinglePartEnvelope(
    string ContentType,
    byte[] Payload);
