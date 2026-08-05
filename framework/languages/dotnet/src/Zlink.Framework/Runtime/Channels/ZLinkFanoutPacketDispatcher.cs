using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkFanoutPacketDispatcher
{
    private static readonly IReadOnlySet<string> EmptyGroups =
        new HashSet<string>(StringComparer.Ordinal);
    private readonly ZLinkDispatchErrorReporter _dispatchErrors;
    private readonly ZLinkChannelPublishDispatchPipeline _publishPipeline;
    private readonly ZLinkFrameworkRuntime? _runtime;

    public ZLinkFanoutPacketDispatcher(
        ZLinkHandlerRegistry handlerRegistry,
        ZLinkHandlerDispatcher dispatcher,
        ZLinkFrameworkRegistration registration,
        ZLinkFrameworkRuntime? runtime,
        ILogger<ZLinkFanoutPacketDispatcher>? logger = null)
    {
        _runtime = runtime;
        var resolvedLogger = logger ?? NullLogger<ZLinkFanoutPacketDispatcher>.Instance;
        _dispatchErrors = new ZLinkDispatchErrorReporter(
            registration.DispatchOptions,
            runtime is null
                ? logger
                : ZLinkMessageFlowTracer.CreateLogger(
                    runtime.Services.GetService<ILoggerFactory>(),
                    logger),
            runtime);
        _publishPipeline = new ZLinkChannelPublishDispatchPipeline(
            null,
            handlerRegistry,
            dispatcher,
            channelName => ResolveMappedGroups(registration, channelName),
            LogLevel.Debug,
            _dispatchErrors,
            registration.Codecs,
            resolvedLogger);
    }

    public async Task DispatchEventMessageAsync(
        string channelName,
        TopicMessage topicMessage,
        CancellationToken cancellationToken)
    {
        if (topicMessage.Parts.Count == 0)
        {
            ReportPublishProtocolError(
                channelName,
                topicMessage,
                ZLinkEnvelopeCodec.MissingHeader());
            return;
        }

        ZLinkEnvelopeHeader header;
        try
        {
            header = ZLinkEnvelopeCodec.DecodeHeader(topicMessage.Parts);
            ZLinkEnvelopeCodec.ValidateDispatchHeader(header);
        }
        catch (ZLinkEnvelopeProtocolException protocolError)
        {
            ReportPublishProtocolError(channelName, topicMessage, protocolError);
            return;
        }

        ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease operation;
        if (_runtime is null)
            operation = new ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease();
        //  Delivering a fanout record to its handler changes nothing in the
        //  Location Store, so it is not object work (spec 21 §4).
        else if (!_runtime.TryEnterInboundOperation(
                     countAsRequest: false, out operation, ownsObjectWork: false))
            return;
        using (operation)
        {
            using var currentFlow = ZLinkFlowContext.Enter(
                header.FlowId,
                header.FlowOrigin,
                _dispatchErrors.Flow.CaptureEnabled,
                ZLinkFlowOrigin.Inbound);

            if (_dispatchErrors.Flow.Enabled(ZLinkMessageFlowOutcome.Received))
                _dispatchErrors.Flow.Trace(new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.Received,
                    ZLinkDispatchErrorSurface.Channel,
                    ZLinkDispatchMessageKind.Publish,
                    header.MessageName,
                    channelName,
                    topicMessage.Topic,
                    SourceRid: header.Source,
                    CorrelationId: header.CorrelationId));

            await _publishPipeline.DispatchAsync(
                    channelName,
                    topicMessage,
                    header,
                    cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private void ReportPublishProtocolError(
        string channelName,
        TopicMessage topicMessage,
        ZLinkEnvelopeProtocolException protocolError)
    {
        var validFlow = ZLinkEnvelopeCodec.ValidFlow(protocolError.Header);
        using var invalidFlow = ZLinkFlowContext.Enter(
            validFlow.FlowId,
            validFlow.FlowOrigin,
            _dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound);
        _dispatchErrors.Report(new ZLinkDispatchFailure(
            ZLinkDispatchErrorSurface.Channel,
            ZLinkDispatchMessageKind.Publish,
            ZLinkDispatchErrorReason.InvalidFrame,
            ZLinkDispatchErrorAction.Drop,
            protocolError.Header.MessageName,
            channelName,
            topicMessage.Topic,
            SourceRid: protocolError.Header.Source,
            CorrelationId: protocolError.Header.CorrelationId,
            Exception: protocolError));
    }

    private static IReadOnlySet<string> ResolveMappedGroups(
        ZLinkFrameworkRegistration registration,
        string channelName)
    {
        return registration.Channels.TryGetValue(channelName, out var channel)
            ? channel.HandlerGroups
            : EmptyGroups;
    }
}
