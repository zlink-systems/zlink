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
        Flow.TraceDispatchError(error);
    }

}
