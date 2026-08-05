using Systems.Zlink.Stream.Connector.Runtime;

namespace Zlink.Framework.Runtime.Streams;

internal abstract class ZLinkSessionStreamCallBase<TMessage>(
    ZLinkSessionContext context,
    TMessage message)
{
    private readonly ZLinkOneWayCallGate _submission = new("Session operation");

    private readonly ZLinkStreamSendBuilder<TMessage> _builder = new(
        message,
        context.Codecs,
        context.CompressionCodec);

    public ZLinkSessionStreamCallBase<TMessage> Metadata(string key, string value)
    {
        _builder.AddMetadata(key, value);
        return this;
    }

    public ZLinkSessionStreamCallBase<TMessage> Metadata(ZLinkMessageMetadata metadata)
    {
        ArgumentNullException.ThrowIfNull(metadata);
        foreach (var (key, value) in metadata.Values) _builder.AddMetadata(key, value);
        return this;
    }

    public ZLinkSessionStreamCallBase<TMessage> Compress()
    {
        _builder.EnableCompression();
        return this;
    }

    protected void ClaimSubmission() => _submission.Claim();

    protected async ValueTask<ZLinkOneWaySubmitResult> ExecuteAsync(
        CancellationToken cancellationToken,
        bool validateBeforeCancellation = false,
        bool isReply = false)
    {
        if (!validateBeforeCancellation) cancellationToken.ThrowIfCancellationRequested();
        var frame = _builder.Build(
            (codec, flags, messageName, metadata) => CreateHeader(
                codec,
                flags,
                messageName,
                metadata,
                context.CurrentDispatchContext),
            out var header);
        if (validateBeforeCancellation && cancellationToken.IsCancellationRequested)
        {
            frame.Dispose();
            cancellationToken.ThrowIfCancellationRequested();
        }
        var result = await context.SubmitAsync(frame, cancellationToken, isReply).ConfigureAwait(false);
        if (result.Status == ZLinkOneWaySubmitStatus.Submitted) context.TraceWritten(header);
        return result;
    }

    protected abstract ZlinkStreamHeader CreateHeader(
        ZlinkStreamCodec codec,
        ZlinkStreamHeaderFlags flags,
        string messageName,
        ZlinkStreamMetadata metadata,
        ZLinkSessionDispatchContext? currentDispatch);
}

internal sealed class ZLinkSessionSendCall<TMessage>(
    ZLinkSessionContext context,
    TMessage message)
    : ZLinkSessionStreamCallBase<TMessage>(context, message), IZLinkSessionSendCall
{
    IZLinkSessionSendCall IZLinkMetadataCall<IZLinkSessionSendCall>.Metadata(
        string key,
        string value)
    {
        return (IZLinkSessionSendCall)Metadata(key, value);
    }

    IZLinkSessionSendCall IZLinkMetadataCall<IZLinkSessionSendCall>.Metadata(
        ZLinkMessageMetadata metadata)
    {
        return (IZLinkSessionSendCall)Metadata(metadata);
    }

    IZLinkSessionSendCall IZLinkSessionSendCall.Compress()
    {
        return (IZLinkSessionSendCall)Compress();
    }

    public ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        ClaimSubmission();
        return ExecuteAsync(cancellationToken).EnsureAcceptedAsync("Session send");
    }

    protected override ZlinkStreamHeader CreateHeader(
        ZlinkStreamCodec codec,
        ZlinkStreamHeaderFlags flags,
        string messageName,
        ZlinkStreamMetadata metadata,
        ZLinkSessionDispatchContext? currentDispatch)
    {
        _ = currentDispatch;
        return new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            codec,
            flags,
            null,
            messageName,
            metadata,
            ZlinkStreamCorrelation.Next());
    }
}

internal sealed class ZLinkSessionReplyCall<TMessage>(
    ZLinkSessionContext context,
    TMessage message)
    : ZLinkSessionStreamCallBase<TMessage>(context, message), IZLinkSessionReplyCall
{
    IZLinkSessionReplyCall IZLinkSessionReplyCall.Compress()
    {
        return (IZLinkSessionReplyCall)Compress();
    }

    public ValueTask Async(
        CancellationToken cancellationToken = default)
    {
        ClaimSubmission();
        return ExecuteAsync(
                cancellationToken,
                validateBeforeCancellation: true,
                isReply: true)
            .EnsureAcceptedAsync("Session reply");
    }

    protected override ZlinkStreamHeader CreateHeader(
        ZlinkStreamCodec codec,
        ZlinkStreamHeaderFlags flags,
        string messageName,
        ZlinkStreamMetadata metadata,
        ZLinkSessionDispatchContext? currentDispatch)
    {
        if (currentDispatch?.RuntimeState is not ZlinkStreamHeader header
            || header.RequestSeq is not { } requestSeq)
            throw new InvalidOperationException("Reply is only available while handling a request packet.");
        if (!currentDispatch.TryClaimReply())
            throw new InvalidOperationException("The reply token has already been used.");

        return ZLinkStreamReplyHeaders.CreateForRequest(
            header,
            ZlinkStreamMessageKind.Response,
            codec,
            flags,
            requestSeq,
            metadata);
    }
}
