namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamReplyHeaders
{
    public static ZlinkStreamHeader CreateForRequest(
        ZlinkStreamHeader requestHeader,
        ZlinkStreamMessageKind kind,
        ZlinkStreamCodec codec,
        ZlinkStreamHeaderFlags flags,
        ZlinkStreamRequestSeq requestSeq,
        ZlinkStreamMetadata metadata)
    {
        // Spec 27 §4/§7: the reply carries the request's correlation id, but
        // flow fields come from the ambient flow context at encode time
        // (ZLinkStreamHeaderCodec.Encode). The context only exists while
        // tracing is on, so an Off host adds no flow fields to the reply.
        return new ZlinkStreamHeader(
            kind,
            codec,
            flags | ZlinkStreamHeaderFlags.HasRequestSeq,
            requestSeq,
            string.Empty,
            metadata,
            requestHeader.CorrelationId,
            null,
            null);
    }
}
