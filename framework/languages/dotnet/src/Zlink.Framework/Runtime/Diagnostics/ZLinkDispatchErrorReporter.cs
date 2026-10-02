using Microsoft.Extensions.Logging;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Diagnostics;

internal sealed class ZLinkDispatchErrorReporter(
    ZLinkDispatchOptionsModel options,
    ILogger? logger = null,
    ZLinkFrameworkRuntime? runtime = null
)
{
    // Success-path tracer companion: every surface already receives a reporter, so
    // exposing the flow tracer here wires all dispatch sites without threading a new
    // parameter. It shares the live options, logger, and generation-owned observer pump.
    public ZLinkMessageFlowTracer Flow { get; } =
        new(options, logger, runtime, runtime is null ? null : runtime.ErrorSink);

    public bool Enabled => Flow.CaptureEnabled;

    private static readonly object BoundaryKey = new();

    public static T WithBoundary<T>(T error, ServiceWireConstants.FrameworkErrorCode failureCode)
        where T : Exception
    {
        if (failureCode != ServiceWireConstants.FrameworkErrorCode.None)
            error.Data[BoundaryKey] = failureCode;
        return error;
    }

    public static ZLinkDispatchErrorReason ReasonFrom(
        Exception error,
        ServiceWireConstants.FrameworkErrorCode? failureCode = null
    )
    {
        failureCode ??= error.Data[BoundaryKey] as ServiceWireConstants.FrameworkErrorCode?;
        if (failureCode == ServiceWireConstants.FrameworkErrorCode.PayloadDecodeFailed)
            return ZLinkDispatchErrorReason.PayloadDecodeFailed;
        if (
            failureCode
            is ServiceWireConstants.FrameworkErrorCode.RouteNotConnected
                or ServiceWireConstants.FrameworkErrorCode.ActorLocationStale
                or ServiceWireConstants.FrameworkErrorCode.SpotGenerationStale
                or ServiceWireConstants.FrameworkErrorCode.SpotMoving
        )
            return ZLinkDispatchErrorReason.StaleTarget;
        return error is ZLinkFrameworkException framework
            ? framework.Kind switch
            {
                ZLinkFrameworkErrorKind.NotFound => ZLinkDispatchErrorReason.HandlerMissing,
                ZLinkFrameworkErrorKind.ProtocolError => ZLinkDispatchErrorReason.InvalidFrame,
                ZLinkFrameworkErrorKind.ShuttingDown => ZLinkDispatchErrorReason.Shutdown,
                _ => ZLinkDispatchErrorReason.HandlerException,
            }
            : ZLinkDispatchErrorReason.HandlerException;
    }

    public void Report(ZLinkDispatchFailure error)
    {
        Flow.TraceDispatchError(error);
    }
}
