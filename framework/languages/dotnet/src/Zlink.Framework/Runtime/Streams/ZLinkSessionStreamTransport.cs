namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkSessionStreamTransport(
    IZLinkStream stream,
    Action<ZlinkStreamHeader> traceWritten)
{
    public bool Write(Message payload)
    {
        if (stream is ZLinkManagedStream managedStream)
            return managedStream.WriteRaw(payload, SendFlags.DontWait);

        return stream.Write(ZLinkMessage.From(payload.ToArray()), SendFlags.DontWait);
    }

    public ValueTask SubmitAsync(
        Message payload,
        CancellationToken cancellationToken)
        => SubmitAsync(stream, payload, cancellationToken);

    internal static ValueTask SubmitAsync(
        IZLinkStream stream,
        Message payload,
        CancellationToken cancellationToken)
    {
        if (stream is ZLinkManagedStream managedStream)
            return managedStream.SubmitRawAsync(payload, cancellationToken);

        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            if (!stream.Write(
                    ZLinkMessage.From(payload.ToArray()),
                    SendFlags.DontWait))
                throw new ZlinkSubmitException(
                    ZlinkSubmitException.ErrorCode.Backpressured);
        }
        finally
        {
            payload.Dispose();
        }
        return ValueTask.CompletedTask;
    }

    public ValueTask ReplyRawAsync(
        ZlinkStreamHeader requestHeader,
        ZLinkActorReply reply,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var responseHeader = reply.CreateResponseHeader(requestHeader);
        var frame = reply.EncodeFrame(responseHeader);
        return SendReplyAsync(
            Message.From(frame),
            cancellationToken,
            "Client stream reply send failed.",
            responseHeader);
    }

    public ValueTask ReplyErrorAsync(
        ZlinkStreamHeader requestHeader,
        Exception exception,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (requestHeader.RequestSeq is not { } requestSeq) return ValueTask.CompletedTask;

        var header = ZLinkStreamReplyHeaders.CreateForRequest(
            requestHeader,
            ZlinkStreamMessageKind.Error,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            requestSeq,
            ZlinkStreamMetadata.Empty);
        var payload = ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(
            ZLinkStreamWireError.FromException(exception));
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload);
        return SendReplyAsync(
            Message.From(frame),
            cancellationToken,
            "Client stream error reply send failed.",
            header);
    }

    private ValueTask SendReplyAsync(
        Message frame,
        CancellationToken cancellationToken,
        string failureMessage,
        ZlinkStreamHeader responseHeader)
    {
        using (frame)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                if (!Write(frame)) throw new InvalidOperationException(failureMessage);
            }
            catch
            {
                throw;
            }
        }
        traceWritten(responseHeader);
        return ValueTask.CompletedTask;
    }
}
