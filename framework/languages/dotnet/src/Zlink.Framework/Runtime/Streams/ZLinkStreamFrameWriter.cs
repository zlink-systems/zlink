namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamFrameWriter
{
    public static async ValueTask WriteAsync(
        Func<Message, CancellationToken, ValueTask> submit,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload.Span);
        var payloadMessage = Message.From(frame);
        await submit(payloadMessage, cancellationToken).ConfigureAwait(false);
    }

    public static ValueTask WriteAsync(
        IZLinkStream stream,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        return WriteAsync(
            (message, token) =>
                ZLinkSessionStreamTransport.SubmitAsync(stream, message, token),
            header,
            payload,
            cancellationToken);
    }

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
