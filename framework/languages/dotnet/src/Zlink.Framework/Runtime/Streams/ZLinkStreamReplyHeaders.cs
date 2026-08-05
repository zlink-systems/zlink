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
        return new ZlinkStreamHeader(
            kind,
            codec,
            flags | ZlinkStreamHeaderFlags.HasRequestSeq,
            requestSeq,
            string.Empty,
            metadata,
            requestHeader.CorrelationId,
            requestHeader.FlowId,
            requestHeader.FlowOrigin);
    }
}
