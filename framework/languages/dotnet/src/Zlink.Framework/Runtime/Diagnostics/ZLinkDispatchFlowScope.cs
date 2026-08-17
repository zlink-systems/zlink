using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Diagnostics;

internal readonly struct ZLinkDispatchFlowScope(
    ZLinkDispatchErrorSurface surface,
    string surfaceName,
    ZLinkDispatchMessageKind messageKind,
    string kindName,
    string packetName,
    string? channelName = null,
    string? contentType = null,
    string? correlationId = null,
    string? topic = null,
    string? sourceRid = null,
    string? spotId = null,
    string? actorId = null,
    string? actorType = null,
    string? logSpotId = null)
{
    // The request owns the flow observed at its dispatch boundary. Application
    // awaits and nested downstream calls may change the ambient context before
    // the terminal trace is emitted.
    private readonly ZLinkFlowValue? capturedFlow = ZLinkFlowContext.Current;

    public string? ChannelName => channelName;

    public string? ContentType => contentType;

    public string PacketName => packetName;

    public bool TryDecode(
        IReadOnlyList<Message> parts,
        Type messageType,
        string contentType,
        ZLinkCodecRegistryBuilder codecs,
        ILogger logger,
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkDispatchErrorAction failureAction,
        out object? message) =>
        TryDecode(
            parts,
            messageType,
            contentType,
            codecs,
            logger,
            dispatchErrors,
            failureAction,
            decodeErrorSubject: null,
            out message,
            out _);

    // decodeErrorSubject names the payload in the reported wrapper exception
    // (e.g. "request"); passing it instead of a wrapping callback keeps the
    // success path free of the closure the callback used to allocate.
    public bool TryDecode(
        IReadOnlyList<Message> parts,
        Type messageType,
        string contentType,
        ZLinkCodecRegistryBuilder codecs,
        ILogger logger,
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkDispatchErrorAction failureAction,
        string? decodeErrorSubject,
        out object? message,
        out ZLinkFrameworkException? decodeError)
    {
        try
        {
            message = ZLinkEnvelopeCodec.DecodeBody(
                parts,
                messageType,
                contentType,
                codecs);
            decodeError = null;
            return true;
        }
        catch (Exception ex)
        {
            message = null;
            decodeError = decodeErrorSubject is null
                ? null
                : new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    $"PayloadDecodeFailed: failed to decode {decodeErrorSubject} payload "
                    + $"for '{channelName}:{packetName}'.",
                    innerException: ex);
            PayloadDecodeFailed(
                logger,
                dispatchErrors,
                failureAction,
                ex,
                decodeError);
            return false;
        }
    }

    public void HandlerMissing(
        ILogger logger,
        ZLinkDispatchErrorReporter dispatchErrors,
        LogLevel logLevel,
        ZLinkDispatchErrorAction action,
        Exception? exception = null)
    {
        Report(
            dispatchErrors,
            ZLinkDispatchErrorReason.HandlerMissing,
            action,
            exception);
    }

    public void Dropped(
        ILogger logger,
        ZLinkDispatchErrorReporter dispatchErrors,
        LogLevel logLevel,
        ZLinkDispatchErrorReason reason = ZLinkDispatchErrorReason.HandlerMissing,
        string reasonName = "no-handler")
    {
        Report(
            dispatchErrors,
            reason,
            ZLinkDispatchErrorAction.Drop);
    }

    public void PayloadDecodeFailed(
        ILogger logger,
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkDispatchErrorAction action,
        Exception exception,
        Exception? reportedException = null)
    {
        Report(
            dispatchErrors,
            ZLinkDispatchErrorReason.PayloadDecodeFailed,
            action,
            reportedException ?? exception);
    }

    public void HandlerException(
        ILogger logger,
        ZLinkDispatchErrorReporter dispatchErrors,
        LogLevel? logLevel,
        ZLinkDispatchErrorAction action,
        Exception exception)
    {
        Report(
            dispatchErrors,
            ZLinkDispatchErrorReason.HandlerException,
            action,
            exception);
    }

    public void ReplyPathMissing(
        ILogger logger,
        ZLinkDispatchErrorReporter dispatchErrors,
        Exception exception)
    {
        Report(
            dispatchErrors,
            ZLinkDispatchErrorReason.ReplyPathMissing,
            ZLinkDispatchErrorAction.FailCaller,
            exception);
    }

    public void Trace(
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkMessageFlowOutcome outcome)
    {
        if (!dispatchErrors.Flow.Enabled(outcome)) return;

        dispatchErrors.Flow.Trace(CreateEvent(outcome, forLog: false));
    }

    private void Report(
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action,
        Exception? exception = null)
    {
        dispatchErrors.Report(new ZLinkDispatchFailure(
            surface,
            messageKind,
            reason,
            action,
            packetName,
            channelName,
            topic,
            SourceRid: sourceRid,
            SpotId: spotId,
            ActorId: actorId,
            CorrelationId: correlationId,
            Exception: exception,
            FlowId: capturedFlow?.FlowId,
            FlowOrigin: capturedFlow?.Origin));
    }

    private ZLinkMessageFlowEvent CreateEvent(
        ZLinkMessageFlowOutcome outcome,
        bool forLog)
    {
        return new ZLinkMessageFlowEvent(
            outcome,
            surface,
            messageKind,
            packetName,
            channelName,
            topic,
            correlationId,
            sourceRid,
            SpotId: forLog ? logSpotId ?? spotId : spotId,
            ActorId: actorId)
        {
            FlowId = capturedFlow?.FlowId ?? string.Empty,
            FlowOrigin = capturedFlow?.Origin
        };
    }

}
