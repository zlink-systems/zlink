namespace Zlink.Framework.Runtime.Diagnostics;

internal readonly struct ZLinkDispatchFlowScope(
    ZLinkDispatchErrorSurface surface,
    bool captureFlow,
    ZLinkDispatchMessageKind messageKind,
    string packetName,
    string? channelName = null,
    string? contentType = null,
    string? correlationId = null,
    string? topic = null,
    string? sourceRid = null,
    string? spotId = null,
    string? actorId = null)
{
    // The request owns the flow observed at its dispatch boundary. Application
    // awaits and nested downstream calls may change the ambient context before
    // the terminal trace is emitted.
    private readonly ZLinkFlowValue? capturedFlow = captureFlow
        ? ZLinkFlowContext.Current
        : null;

    public string? ChannelName => channelName;

    public string? ContentType => contentType;

    public string PacketName => packetName;

    public bool TryDecode(
        IReadOnlyList<Message> parts,
        Type messageType,
        string contentType,
        ZLinkCodecRegistryBuilder codecs,
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkDispatchErrorAction failureAction,
        out object? message) =>
        TryDecode(
            parts,
            messageType,
            contentType,
            codecs,
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
            // Payload decode failure is framework-generated (zlink.origin
            // marker on the resulting error reply); the application handler
            // never ran.
            decodeError = decodeErrorSubject is null
                ? null
                : new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.ProtocolError,
                    $"PayloadDecodeFailed: failed to decode {decodeErrorSubject} payload "
                    + $"for '{channelName}:{packetName}'.",
                    innerException: ex)
                {
                    Origin = ZLinkErrorOrigin.Framework
                };
            PayloadDecodeFailed(
                dispatchErrors,
                failureAction,
                ex,
                decodeError);
            return false;
        }
    }

    public void HandlerMissing(
        ZLinkDispatchErrorReporter dispatchErrors,
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
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkDispatchErrorReason reason = ZLinkDispatchErrorReason.HandlerMissing)
    {
        Report(
            dispatchErrors,
            reason,
            ZLinkDispatchErrorAction.Drop);
    }

    public void PayloadDecodeFailed(
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
        ZLinkDispatchErrorReporter dispatchErrors,
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

        dispatchErrors.Flow.Trace(CreateEvent(outcome));
    }

    private void Report(
        ZLinkDispatchErrorReporter dispatchErrors,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action,
        Exception? exception = null)
    {
        if (!dispatchErrors.Enabled) return;

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

    private ZLinkMessageFlowEvent CreateEvent(ZLinkMessageFlowOutcome outcome)
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
            SpotId: spotId,
            ActorId: actorId)
        {
            FlowId = capturedFlow?.FlowId ?? string.Empty,
            FlowOrigin = capturedFlow?.Origin
        };
    }

}
