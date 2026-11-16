namespace Zlink.Framework.Runtime.Streams;

internal static class ZLinkStreamFrameWriter
{
    public static async ValueTask WriteAsync(
        Func<Message, CancellationToken, Task> submit,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload.Span
        );
        var payloadMessage = Message.From(frame);
        await submit(payloadMessage, cancellationToken).ConfigureAwait(false);
    }

    public static ValueTask WriteAsync(
        IZLinkStream stream,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        return WriteAsync(
            (message, token) => ZLinkSessionStreamTransport.SubmitAsync(stream, message, token),
            header,
            payload,
            cancellationToken
        );
    }

    public static void Write(
        IZLinkStream stream,
        ZlinkStreamHeader header,
        ReadOnlyMemory<byte> payload,
        string failureMessage
    )
    {
        Write(stream, header, payload.Span, failureMessage);
    }

    public static void Write(
        IZLinkStream stream,
        ZlinkStreamHeader header,
        ReadOnlySpan<byte> payload,
        string failureMessage
    )
    {
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload
        );
        var message = Message.From(frame);
        if (!WriteRaw(stream, message))
            throw new InvalidOperationException(failureMessage);
    }

    private static bool WriteRaw(IZLinkStream stream, Message message)
    {
        if (stream is ZLinkManagedStream managedStream)
            return managedStream.WriteRaw(message);

        try
        {
            return stream.Write(ZLinkMessage.From(message.ToArray()));
        }
        finally
        {
            message.Dispose();
        }
    }
}
