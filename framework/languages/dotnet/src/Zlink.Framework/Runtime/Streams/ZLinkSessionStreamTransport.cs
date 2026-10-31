namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkSessionStreamTransport(
    ZLinkSessionContext context,
    IZLinkStream stream,
    Action<ZlinkStreamHeader> traceWritten
)
{
    public bool Write(Message payload)
    {
        try
        {
            if (stream is ZLinkManagedStream managedStream)
                return managedStream.WriteRaw(payload);

            return stream.Write(ZLinkMessage.From(payload.ToArray()));
        }
        catch (ZlinkSubmitException failure)
        {
            throw ZLinkSubmitFailureMapper.CreateException(failure, "Session write");
        }
    }

    public Task SubmitAsync(Message payload, CancellationToken cancellationToken) =>
        SubmitAsync(stream, payload, cancellationToken);

    internal Task SubmitReplyAsync(
        ZlinkStreamHeader header,
        Message frame,
        CancellationToken cancellationToken
    )
    {
        if (header.Kind is not (ZlinkStreamMessageKind.Response or ZlinkStreamMessageKind.Error))
            return SubmitAsync(frame, cancellationToken);
        return context.Runtime.SubmitStreamReplyAsync(
            context,
            header,
            normalized =>
            {
                if (ReferenceEquals(header, normalized))
                    return SubmitAsync(frame, cancellationToken);
                using (frame)
                {
                    if (!ZLinkStreamFrameCodec.TryDecode(frame.ToArray(), out _, out var payload))
                        throw new InvalidOperationException("STREAM reply frame is invalid.");
                    return SubmitAsync(
                        Message.From(
                            ZLinkStreamFrameCodec.Encode(
                                ZLinkStreamProtocolDefaults.EncodeHeader(normalized).Span,
                                payload
                            )
                        ),
                        cancellationToken
                    );
                }
            }
        );
    }

    internal static Task SubmitAsync(
        IZLinkStream stream,
        Message payload,
        CancellationToken cancellationToken
    )
    {
        if (stream is ZLinkManagedStream managedStream)
            return managedStream.SubmitRawAsync(payload, cancellationToken);

        cancellationToken.ThrowIfCancellationRequested();
        try
        {
            if (!stream.Write(ZLinkMessage.From(payload.ToArray())))
                throw new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        }
        finally
        {
            payload.Dispose();
        }
        return Task.CompletedTask;
    }

    public ValueTask ReplyRawAsync(
        ZlinkStreamHeader requestHeader,
        ZLinkActorReply reply,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        var responseHeader = reply.CreateResponseHeader(requestHeader);
        var frame = reply.EncodeFrame(responseHeader);
        return SendReplyAsync(Message.From(frame), cancellationToken, responseHeader);
    }

    public ValueTask ReplyErrorAsync(
        ZlinkStreamHeader requestHeader,
        Exception exception,
        CancellationToken cancellationToken
    )
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (requestHeader.RequestSeq is not { } requestSeq)
            return ValueTask.CompletedTask;

        var header = ZLinkStreamReplyHeaders.CreateForRequest(
            requestHeader,
            ZlinkStreamMessageKind.Error,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            requestSeq,
            ZlinkStreamMetadata.Empty
        );
        var payload = ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(
            ZLinkStreamWireError.FromException(exception)
        );
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload
        );
        return SendReplyAsync(Message.From(frame), cancellationToken, header);
    }

    private async ValueTask SendReplyAsync(
        Message frame,
        CancellationToken cancellationToken,
        ZlinkStreamHeader responseHeader
    )
    {
        await SubmitReplyAsync(responseHeader, frame, cancellationToken).ConfigureAwait(false);
        traceWritten(responseHeader);
    }
}
