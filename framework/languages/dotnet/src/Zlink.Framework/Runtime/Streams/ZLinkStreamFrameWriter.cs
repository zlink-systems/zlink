namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamFrameWriter
{
    public static void Write(
        Func<Message, bool> write,
        ZlinkStreamHeader header,
        ReadOnlySpan<byte> payload,
        string failureMessage)
    {
        var frame = ZLinkStreamFrameCodec.Encode(ZLinkStreamProtocolDefaults.EncodeHeader(header).Span, payload);
        using var payloadMessage = Message.From(frame);
        if (!write(payloadMessage)) throw new InvalidOperationException(failureMessage);
    }

    public static void Write(
        IZLinkStream stream,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        string failureMessage)
    {
        Write(
            message => WriteRaw(stream, message),
            header,
            payload.Span,
            failureMessage);
    }

    public static void Write(
        IZLinkStream stream,
        ZlinkStreamHeader header,
        ReadOnlySpan<byte> payload,
        string failureMessage)
    {
        Write(
            message => WriteRaw(stream, message),
            header,
            payload,
            failureMessage);
    }

    private static bool WriteRaw(IZLinkStream stream, Message message)
    {
        if (stream is ZLinkManagedStream managedStream)
            return managedStream.WriteRaw(message, SendFlags.DontWait);

        return stream.Write(ZLinkMessage.From(message.ToArray()), SendFlags.DontWait);
    }
}
