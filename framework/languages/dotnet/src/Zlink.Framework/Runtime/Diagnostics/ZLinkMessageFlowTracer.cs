using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Diagnostics;

// Success-path message-flow tracer — the twin of ZLinkDispatchErrorReporter for
// received/dispatched/replied/sent/reply_received/error transitions.
//
// PERFORMANCE: callers MUST guard event construction with Enabled(outcome) so that an
// "off" dispatch pays nothing but a volatile mode read (a C# lambda would heap-
// allocate a closure, so we use a call-site guard instead of a lazy delegate):
//     if (tracer.Enabled(outcome)) tracer.Trace(new ZLinkMessageFlowEvent(...));
internal sealed class ZLinkMessageFlowTracer
{
    internal const string LoggerCategory = "zlink.framework.dispatch";

    private readonly ILogger _logger;
    private readonly ZLinkDispatchOptionsModel _options;
    private readonly IZLinkRuntimeFailureReporter? _errorSink;

    public ZLinkMessageFlowTracer(
        ZLinkDispatchOptionsModel options,
        ILogger? logger = null,
        ZLinkFrameworkRuntime? runtime = null,
        IZLinkRuntimeFailureReporter? errorSink = null)
    {
        _options = options;
        _errorSink = errorSink ?? (runtime is null ? null : runtime.ErrorSink);
        _logger = logger ?? ZLinkStandardErrorLogger.Instance;
    }

    public bool CaptureEnabled =>
        _options.Diagnostics.EffectiveLevel != ZLinkDiagnosticsLevel.Off;

    // Cheap mode gate (relaxed/volatile read of the live mode). Build the event only
    // after this returns true.
    public bool Enabled(ZLinkMessageFlowOutcome outcome)
    {
        return ShouldLog(outcome);
    }

    public void Trace(ZLinkMessageFlowEvent flow)
    {
        var logEnabled = ShouldLog(flow.Outcome);
        if (!logEnabled) return;

        flow = NormalizeFlowPair(flow);
        if (string.IsNullOrEmpty(flow.FlowId))
        {
            var current = ZLinkFlowContext.Current;
            if (current is { } value)
                flow = flow with { FlowId = value.FlowId, FlowOrigin = value.Origin };
        }

        // Sampling thins healthy traffic; dropped transitions always pass through.
        var logSampled =
            flow.Outcome is ZLinkMessageFlowOutcome.Dropped or ZLinkMessageFlowOutcome.Error
            || Sample(ZLinkTraceFormat.FlowIdKey(flow) ?? string.Empty);
        if (!logSampled) return;

        try
        {
            LogDefault(flow);
            ZLinkTelemetry.TraceMessageFlow(flow);
        }
        catch (Exception ex)
        {
            ReportUnhandledCallbackException(ex);
        }
    }

    private void ReportUnhandledCallbackException(Exception exception)
    {
        if (_errorSink is not null)
            _errorSink.ReportUnhandledCallbackException(exception);
        else
            ZLinkFrameworkDebugLog.UnhandledCallbackFailure(exception);
    }

    internal bool ShouldLog(ZLinkMessageFlowOutcome outcome) =>
        (int)_options.Diagnostics.EffectiveLevel >= (int)RequiredLevel(outcome);

    internal static ILogger CreateLogger(ILoggerFactory? factory, ILogger? fallback = null) =>
        factory?.CreateLogger(LoggerCategory) ?? fallback ?? ZLinkStandardErrorLogger.Instance;

    private static ZLinkDiagnosticsLevel RequiredLevel(ZLinkMessageFlowOutcome outcome)
    {
        return outcome is ZLinkMessageFlowOutcome.Dropped or ZLinkMessageFlowOutcome.Error
            ? ZLinkDiagnosticsLevel.Errors
            : ZLinkDiagnosticsLevel.Normal;
    }

    private bool Sample(string flowId)
    {
        var rate = _options.Diagnostics.SampleRate;
        if (rate >= 1.0d) return true;

        if (rate <= 0.0d) return false;

        const ulong offset = 14695981039346656037ul;
        const ulong prime = 1099511628211ul;
        var hash = offset;
        foreach (var character in flowId)
        {
            hash ^= (byte)character;
            hash *= prime;
        }

        return hash / (double)ulong.MaxValue < rate;
    }

    private void LogDefault(ZLinkMessageFlowEvent flow)
    {
        var diagnostics = _options.Diagnostics;
        long? size = flow.MessageSize is { } messageSize
                     && diagnostics.EffectiveLevel >= ZLinkDiagnosticsLevel.Detailed
                     && diagnostics.MessageSizesIncluded
            ? messageSize
            : null;

        var level = ZLinkTraceFormat.ResolveLogLevel(flow);
        if (!_logger.IsEnabled(level)) return;

        _logger.Log(
            level,
            "phase={Phase} surface={Surface} kind={Kind} packet={Packet} channel={Channel} topic={Topic} corr={Corr} flow={Flow} origin={Origin} src={Src} localRid={LocalRid} peerRid={PeerRid} socket={Socket} spot={Spot} actor={Actor} errorReason={ErrorReason} errorAction={ErrorAction} errorType={ErrorType} errorMessage={ErrorMessage} size={Size}",
            ZLinkTraceFormat.OutcomeKey(flow.Outcome),
            flow.Surface,
            flow.MessageKind,
            flow.PacketName,
            flow.ChannelName,
            flow.Topic,
            flow.CorrelationId,
            ZLinkTraceFormat.FlowIdKey(flow),
            ZLinkTraceFormat.FlowOriginKey(flow),
            flow.SourceRid,
            flow.LocalRid,
            flow.PeerRid,
            flow.SocketRole,
            flow.SpotId,
            flow.ActorId,
            flow.ErrorReason,
            flow.ErrorAction,
            flow.ErrorType,
            flow.ErrorMessage,
            size);
    }

    private static ZLinkMessageFlowEvent NormalizeFlowPair(ZLinkMessageFlowEvent flow)
    {
        var hasFlowId = !string.IsNullOrEmpty(flow.FlowId);
        var hasFlowOrigin = flow.FlowOrigin is not null;
        if (hasFlowId == hasFlowOrigin) return flow;
        return flow with { FlowId = string.Empty, FlowOrigin = null };
    }
}

internal sealed class ZLinkStandardErrorLogger : ILogger
{
    public static ZLinkStandardErrorLogger Instance { get; } = new();

    private ZLinkStandardErrorLogger()
    {
    }

    public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

    public bool IsEnabled(LogLevel logLevel) => logLevel != LogLevel.None;

    public void Log<TState>(
        LogLevel logLevel,
        EventId eventId,
        TState state,
        Exception? exception,
        Func<TState, Exception?, string> formatter)
    {
        if (!IsEnabled(logLevel)) return;
        Console.Error.WriteLine($"zlink flow: {formatter(state, exception)}");
    }
}

internal static class ZLinkTraceFormat
{
    public static LogLevel ResolveLogLevel(ZLinkMessageFlowEvent flow)
    {
        if (flow.ErrorReason == ZLinkDispatchErrorReason.HandlerException) return LogLevel.Error;
        if (flow.ErrorReason is ZLinkDispatchErrorReason.HandlerMissing
            or ZLinkDispatchErrorReason.PayloadDecodeFailed
            or ZLinkDispatchErrorReason.InvalidFrame)
            return flow.MessageKind == ZLinkDispatchMessageKind.Publish
                ? LogLevel.Debug
                : LogLevel.Warning;
        if (flow.Outcome == ZLinkMessageFlowOutcome.Error) return LogLevel.Error;
        if (flow.Outcome == ZLinkMessageFlowOutcome.Dropped)
            return flow.MessageKind == ZLinkDispatchMessageKind.Publish
                ? LogLevel.Debug
                : LogLevel.Warning;
        return LogLevel.Information;
    }

    public static string OutcomeKey(ZLinkMessageFlowOutcome outcome)
    {
        return outcome switch
        {
            ZLinkMessageFlowOutcome.Received => "received",
            ZLinkMessageFlowOutcome.Dispatched => "dispatched",
            ZLinkMessageFlowOutcome.Replied => "replied",
            ZLinkMessageFlowOutcome.Dropped => "dropped",
            ZLinkMessageFlowOutcome.Sent => "sent",
            ZLinkMessageFlowOutcome.ReplyReceived => "reply-received",
            ZLinkMessageFlowOutcome.Error => "error",
            _ => outcome.ToString().ToLowerInvariant()
        };
    }

    public static string? FlowIdKey(ZLinkMessageFlowEvent flow) =>
        !string.IsNullOrEmpty(flow.FlowId) && flow.FlowOrigin is not null
            ? flow.FlowId
            : null;

    public static string? FlowOriginKey(ZLinkMessageFlowEvent flow) =>
        FlowIdKey(flow) is not null
            ? flow.FlowOrigin!.Value.ToString().ToLowerInvariant()
            : null;

}
