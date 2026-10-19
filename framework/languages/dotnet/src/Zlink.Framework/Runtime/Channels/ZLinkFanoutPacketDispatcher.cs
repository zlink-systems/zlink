using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkFanoutPacketDispatcher
{
    private static readonly IReadOnlySet<string> EmptyGroups = new HashSet<string>(
        StringComparer.Ordinal
    );
    private readonly ZLinkDispatchErrorReporter _dispatchErrors;
    private readonly ZLinkChannelPublishDispatchPipeline _publishPipeline;
    private readonly ZLinkFrameworkRuntime? _runtime;

    public ZLinkFanoutPacketDispatcher(
        ZLinkHandlerRegistry handlerRegistry,
        ZLinkHandlerDispatcher dispatcher,
        ZLinkFrameworkRegistration registration,
        ZLinkFrameworkRuntime? runtime,
        ILogger<ZLinkFanoutPacketDispatcher>? logger = null
    )
    {
        _runtime = runtime;
        _dispatchErrors = new ZLinkDispatchErrorReporter(
            registration.DispatchOptions,
            runtime is null
                ? logger
                : ZLinkMessageFlowTracer.CreateLogger(
                    runtime.Services.GetService<ILoggerFactory>(),
                    logger
                ),
            runtime
        );
        _publishPipeline = new ZLinkChannelPublishDispatchPipeline(
            null,
            handlerRegistry,
            dispatcher,
            channelName => ResolveMappedGroups(registration, channelName),
            _dispatchErrors,
            registration.Codecs
        );
    }

    public async Task DispatchEventMessageAsync(
        string channelName,
        TopicMessage topicMessage,
        CancellationToken cancellationToken,
        (ZLinkEnvelopeHeader? Header, ZLinkEnvelopeProtocolException? Error)? decodedEnvelope = null
    )
    {
        var envelope = decodedEnvelope ?? ReadHeader(topicMessage.Parts);
        if (envelope.Error is { } protocolError)
        {
            ReportPublishProtocolError(channelName, topicMessage, protocolError);
            return;
        }
        var header = envelope.Header!;

        var admission = _runtime is null
            ? new ZLinkInboundOperationAdmission(
                true,
                ZLinkFrameworkRuntime.ZLinkRuntimeOperationLease.None
            )
            //  Delivering a fanout record to its handler changes nothing in the
            //  Location Store, so it is not object work (spec 21 §4).
            : (
                await _runtime
                    .TryAdmitInboundOperationAsync(countAsRequest: false, ownsObjectWork: false)
                    .ConfigureAwait(false)
            ).EnterInbound();
        if (!admission.Accepted)
            return;
        using (admission.Lease)
        {
            using var currentFlow = ZLinkFlowContext.Enter(
                header.FlowId,
                header.FlowOrigin,
                _dispatchErrors.Flow.CaptureEnabled,
                ZLinkFlowOrigin.Inbound
            );

            await _publishPipeline
                .DispatchAsync(channelName, topicMessage, header, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    internal (ZLinkEnvelopeHeader? Header, ZLinkEnvelopeProtocolException? Error) ReadHeader(
        IReadOnlyList<Message> parts
    )
    {
        try
        {
            var header = ZLinkEnvelopeCodec.DecodeHeader(
                parts,
                _dispatchErrors.Flow.CaptureEnabled
            );
            ZLinkEnvelopeCodec.ValidateDispatchHeader(header);
            return (header, null);
        }
        catch (ZLinkEnvelopeProtocolException error)
        {
            return (null, error);
        }
    }

    private void ReportPublishProtocolError(
        string channelName,
        TopicMessage topicMessage,
        ZLinkEnvelopeProtocolException protocolError
    )
    {
        if (!_dispatchErrors.Enabled)
            return;

        // Keep whatever flow the invalid frame carried in readable form,
        // but never fabricate a fresh id for its failure record (spec 27 §7).
        var validFlow = ZLinkEnvelopeCodec.ValidFlow(protocolError.Header);
        using var invalidFlow = ZLinkFlowContext.Enter(
            validFlow.FlowId,
            validFlow.FlowOrigin,
            _dispatchErrors.Flow.CaptureEnabled,
            ZLinkFlowOrigin.Inbound,
            createIfAbsent: false
        );
        _dispatchErrors.Report(
            new ZLinkDispatchFailure(
                ZLinkDispatchErrorSurface.ClassicFanout,
                ZLinkDispatchMessageKind.Send,
                ZLinkDispatchErrorReason.InvalidFrame,
                ZLinkDispatchErrorAction.Drop,
                protocolError.Header.MessageName,
                channelName,
                topicMessage.Topic,
                SourceRid: protocolError.Header.Source,
                CorrelationId: protocolError.Header.CorrelationId,
                Exception: protocolError
            )
        );
    }

    private static IReadOnlySet<string> ResolveMappedGroups(
        ZLinkFrameworkRegistration registration,
        string channelName
    )
    {
        return registration.Channels.TryGetValue(channelName, out var channel)
            ? channel.HandlerGroups
            : EmptyGroups;
    }
}
