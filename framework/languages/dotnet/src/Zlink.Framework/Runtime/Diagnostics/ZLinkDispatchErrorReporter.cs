using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Diagnostics;

internal sealed class ZLinkDispatchErrorReporter(
    ZLinkDispatchOptionsModel options,
    ILogger? logger = null,
    ZLinkFrameworkRuntime? runtime = null)
{
    // Success-path tracer companion: every surface already receives a reporter, so
    // exposing the flow tracer here wires all dispatch sites without threading a new
    // parameter. It shares the live options, logger, and generation-owned observer pump.
    public ZLinkMessageFlowTracer Flow { get; } = new(
        options,
        logger,
        runtime,
        runtime is null ? null : runtime.ErrorSink);

    public void Report(ZLinkDispatchFailure error)
    {
        Report(error, ZLinkMessageFlowOutcome.Error);
    }

    internal void ReportDropped(ZLinkDispatchFailure error)
    {
        Report(error, ZLinkMessageFlowOutcome.Dropped);
    }

    private void Report(
        ZLinkDispatchFailure error,
        ZLinkMessageFlowOutcome outcome)
    {
        if (Flow.Enabled(outcome))
            Flow.Trace(new ZLinkMessageFlowEvent(
                outcome,
                error.Surface,
                error.MessageKind,
                error.PacketName,
                error.ChannelName,
                error.Topic,
                error.CorrelationId,
                error.SourceRid,
                SpotId: error.SpotId,
                ActorId: error.ActorId,
                ErrorReason: error.Reason,
                ErrorAction: error.Action,
                ErrorType: error.Exception?.GetType().Name,
                ErrorMessage: error.Exception?.Message));
    }
}
