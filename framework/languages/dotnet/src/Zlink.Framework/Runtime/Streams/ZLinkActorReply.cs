namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkActorReply(
    ZlinkStreamMessageKind kind,
    ZlinkStreamCodec codec,
    byte[] payload,
    ZlinkStreamHeaderFlags flags,
    ZlinkStreamMetadata metadata)
{
    public ZlinkStreamMessageKind Kind { get; } = kind;

    public ZlinkStreamCodec Codec { get; } = codec;

    public byte[] Payload { get; } = payload;

    public ZlinkStreamHeaderFlags Flags { get; } = flags;

    public ZlinkStreamMetadata Metadata { get; } = metadata;

    public static ZLinkActorReply FromPayload(
        ZlinkStreamCodec codec,
        byte[] payload,
        ZLinkSpotActorReplyOptionsSnapshot options,
        IZlinkStreamCompressionCodec? compressionCodec)
    {
        var flags = ZlinkStreamHeaderFlags.None;
        if (options.CompressPayload)
        {
            payload = ZLinkStreamProtocolDefaults.Compress(compressionCodec, payload).ToArray();
            flags |= ZlinkStreamHeaderFlags.PayloadCompressed;
        }

        var metadata = ZlinkStreamMetadata.Empty;
        foreach (var (key, value) in options.Metadata) metadata = metadata.With(key, value);

        return new ZLinkActorReply(ZlinkStreamMessageKind.Response, codec, payload, flags, metadata);
    }

    public static ZLinkActorReply FromError(Exception exception)
    {
        return new ZLinkActorReply(
            ZlinkStreamMessageKind.Error,
            ZlinkStreamCodec.Json,
            ZLinkEnvelopeCodec.EncodeJsonBytes(
                ZLinkStreamWireError.FromException(exception)),
            ZlinkStreamHeaderFlags.None,
            ZlinkStreamMetadata.Empty);
    }

    public ZlinkStreamHeader CreateResponseHeader(ZlinkStreamHeader requestHeader)
    {
        if (requestHeader.RequestSeq is not { } requestSeq)
            throw new InvalidOperationException("Actor reply frame requires a request sequence.");

        return ZLinkStreamReplyHeaders.CreateForRequest(
            requestHeader,
            Kind,
            Codec,
            Flags,
            requestSeq,
            Metadata);
    }

    public byte[] ToFrame(ZlinkStreamHeader requestHeader)
    {
        return EncodeFrame(CreateResponseHeader(requestHeader));
    }

    public byte[] EncodeFrame(ZlinkStreamHeader responseHeader)
    {
        return ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(responseHeader).Span,
            Payload);
    }
}
