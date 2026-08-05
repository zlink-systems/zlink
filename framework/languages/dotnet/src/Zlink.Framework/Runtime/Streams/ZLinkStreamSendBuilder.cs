namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkStreamSendBuilder<TMessage>(
    TMessage message,
    ZLinkCodecRegistryBuilder codecs,
    IZlinkStreamCompressionCodec? compressionCodec)
{
    private static readonly IZlinkStreamPacketNameResolver MessageNameResolver =
        ZLinkStreamProtocolDefaults.PacketNameResolver;

    private bool _compress;
    private int _executed;

    private readonly string _messageName = MessageNameResolver.Resolve(typeof(TMessage));
    private ZlinkStreamMetadata _metadata = ZlinkStreamMetadata.Empty;

    public void AddMetadata(string key, string value)
    {
        _metadata = _metadata.With(key, value);
    }

    public void EnableCompression()
    {
        _compress = true;
    }

    public ZlinkStreamHeader Write(
        Func<ZlinkStreamCodec, ZlinkStreamHeaderFlags, string, ZlinkStreamMetadata, ZlinkStreamHeader> createHeader,
        Func<Message, bool> write,
        string errorMessage)
    {
        using var frame = Build(createHeader, out var header);
        if (!write(frame)) throw new InvalidOperationException(errorMessage);
        return header;
    }

    public Message Build(
        Func<ZlinkStreamCodec, ZlinkStreamHeaderFlags, string, ZlinkStreamMetadata, ZlinkStreamHeader> createHeader,
        out ZlinkStreamHeader header)
    {
        if (Interlocked.Exchange(ref _executed, 1) != 0)
            throw new InvalidOperationException("Stream send builders can be executed only once.");

        var encoded = ZLinkStreamPacketPayloadCodec.Encode(message, typeof(TMessage), codecs);
        var payload = encoded.Payload;
        var flags = ZlinkStreamHeaderFlags.None;
        if (_compress)
        {
            payload = ZLinkStreamProtocolDefaults.Compress(compressionCodec, payload);
            flags |= ZlinkStreamHeaderFlags.PayloadCompressed;
        }

        header = createHeader(encoded.Codec, flags, _messageName, _metadata);
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload.Span);
        return Message.From(frame);
    }
}
