using Zlink.Framework.Runtime.Dispatch;

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

    public ValueTask ReplyRawAsync(
        ZlinkStreamHeader requestHeader,
        ZLinkActorReply reply,
        CancellationToken cancellationToken,
        ZLinkCompletionAdmissionOwner.ResponderLease? completionPermit = null)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var responseHeader = reply.CreateResponseHeader(requestHeader);
        var frame = reply.EncodeFrame(responseHeader);
        return SendReplyAsync(
            Message.From(frame),
            completionPermit,
            cancellationToken,
            "Client stream reply send failed.",
            responseHeader);
    }

    public ValueTask ReplyErrorAsync(
        ZlinkStreamHeader requestHeader,
        Exception exception,
        CancellationToken cancellationToken,
        ZLinkCompletionAdmissionOwner.ResponderLease? completionPermit = null)
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
        var payload = ZLinkEnvelopeCodec.EncodeJsonBytes(
            ZLinkStreamWireError.FromException(exception));
        var frame = ZLinkStreamFrameCodec.Encode(
            ZLinkStreamProtocolDefaults.EncodeHeader(header).Span,
            payload);
        return SendReplyAsync(
            Message.From(frame),
            completionPermit,
            cancellationToken,
            "Client stream error reply send failed.",
            header);
    }

    private async ValueTask SendReplyAsync(
        Message frame,
        ZLinkCompletionAdmissionOwner.ResponderLease? completionPermit,
        CancellationToken cancellationToken,
        string failureMessage,
        ZlinkStreamHeader responseHeader)
    {
        using (frame)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (completionPermit is not null)
                await completionPermit.ReserveReplyAsync(
                        checked((ulong)Math.Max(frame.Size, 1)),
                        cancellationToken)
                    .ConfigureAwait(false);
            try
            {
                if (!Write(frame)) throw new InvalidOperationException(failureMessage);
                completionPermit?.TransferToCore();
            }
            catch
            {
                completionPermit?.Dispose();
                throw;
            }
        }
        traceWritten(responseHeader);
    }
}
