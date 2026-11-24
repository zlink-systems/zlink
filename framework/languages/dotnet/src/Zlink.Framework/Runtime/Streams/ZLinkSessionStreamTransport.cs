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

            using (payload)
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

        var authentication = context.AuthenticationAdmissionForReply(header);
        var submission = context.Runtime.SubmitStreamReplyAsync(
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
        return authentication is null
            ? submission
            : ReleaseAfterSubmitAsync(submission, authentication);
    }

    private static async Task ReleaseAfterSubmitAsync(
        Task submission,
        ZLinkAuthenticationAdmission.Lease authentication
    )
    {
        await submission.ConfigureAwait(false);
        authentication.Release();
    }

    internal static Task SubmitAsync(
        IZLinkStream stream,
        Message payload,
        CancellationToken cancellationToken
    )
    {
        if (stream is ZLinkManagedStream managedStream)
            return managedStream.SubmitRawAsync(payload, cancellationToken);

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
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
        if (requestHeader.RequestSeq is null)
            return ValueTask.CompletedTask;

        var (header, frame) = EncodeError(requestHeader, exception, ZlinkStreamMetadata.Empty);
        return SendReplyAsync(frame, cancellationToken, header);
    }

    internal static (ZlinkStreamHeader Header, Message Frame) EncodeError(
        ZlinkStreamHeader requestHeader,
        Exception exception,
        ZlinkStreamMetadata metadata
    )
    {
        var header = ZLinkStreamReplyHeaders.CreateForRequest(
            requestHeader,
            ZlinkStreamMessageKind.Error,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            requestHeader.RequestSeq!.Value,
            metadata
        );
        var payload = ZLinkEnvelopeCodec.EncodeProtocolJsonBytes(
            ZLinkStreamWireError.FromException(exception)
        );
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload
        );
        return (header, Message.From(frame));
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
