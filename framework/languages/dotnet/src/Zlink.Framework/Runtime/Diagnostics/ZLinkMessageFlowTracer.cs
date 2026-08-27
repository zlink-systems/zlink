using System;
using System.Collections;
using System.Text;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;

namespace Zlink.Framework.Runtime.Diagnostics;

// Success-path message-flow tracer — the twin of ZLinkDispatchErrorReporter for
// received/admitted/dispatched/completed/replied/sent/reply_received and
// backpressure/drop transitions.
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
    private long _localSamplingSequence;

    public ZLinkMessageFlowTracer(
        ZLinkDispatchOptionsModel options,
        ILogger? logger = null,
        ZLinkFrameworkRuntime? runtime = null,
        IZLinkRuntimeFailureReporter? errorSink = null)
    {
        _options = options;
        _errorSink = errorSink ?? (runtime is null ? null : runtime.ErrorSink);
        _logger = logger ?? NullLogger.Instance;
    }

    public bool CaptureEnabled =>
        _options.Diagnostics.EffectiveLevel != ZLinkDiagnosticsLevel.Off;

    internal bool DetailedEnabled =>
        _options.Diagnostics.EffectiveLevel >= ZLinkDiagnosticsLevel.Detailed;

    // Cheap mode gate (relaxed/volatile read of the live mode). Build the event only
    // after this returns true.
    public bool Enabled(ZLinkMessageFlowOutcome outcome)
    {
        return ShouldLog(outcome);
    }

    internal bool Enabled(
        ZLinkMessageFlowOutcome outcome,
        ZLinkMessageFlowResult? result) => ShouldLog(outcome, result);

    public void Trace(ZLinkMessageFlowEvent flow)
    {
        var logEnabled = ShouldLog(flow.Outcome, flow.Result);
        if (!logEnabled) return;

        flow = NormalizeFlowPair(flow);
        if (string.IsNullOrEmpty(flow.FlowId))
        {
            var current = ZLinkFlowContext.Current;
            if (current is { } value)
                flow = flow with { FlowId = value.FlowId, FlowOrigin = value.Origin };
        }

        var diagnostics = _options.Diagnostics;
        if (flow.MessageSize is not null
            && (diagnostics.EffectiveLevel < ZLinkDiagnosticsLevel.Detailed
                || !diagnostics.MessageSizesIncluded))
            flow = flow with { MessageSize = null };
        if (flow.DurationSeconds is not null
            && diagnostics.EffectiveLevel < ZLinkDiagnosticsLevel.Detailed)
            flow = flow with { DurationSeconds = null };

        // Sampling thins healthy traffic; failures, drops, and backpressure always pass through.
        var logSampled =
            flow.Outcome is ZLinkMessageFlowOutcome.Dropped or ZLinkMessageFlowOutcome.Backpressured
            || flow.Result is not null and not ZLinkMessageFlowResult.Succeeded
            || Sample(
                ZLinkTraceFormat.FlowIdKey(flow),
                flow.SourceMeshGeneration ?? _options.Diagnostics.SourceMeshGeneration);
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

    public void TraceDispatchError(ZLinkDispatchFailure error)
    {
        if (_options.Diagnostics.EffectiveLevel == ZLinkDiagnosticsLevel.Off) return;

        var flowId = error.FlowId;
        var flowOrigin = error.FlowOrigin;
        if (string.IsNullOrEmpty(flowId) || flowOrigin is null)
        {
            var current = ZLinkFlowContext.Current;
            flowId = current?.FlowId;
            flowOrigin = current?.Origin;
        }

        try
        {
            LogDefault(error, flowId, flowOrigin);
            ZLinkTelemetry.TraceDispatchError(error, flowId, flowOrigin);
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

    private bool ShouldLog(
        ZLinkMessageFlowOutcome outcome,
        ZLinkMessageFlowResult? result) =>
        (int)_options.Diagnostics.EffectiveLevel >= (int)(result is not null
            and not ZLinkMessageFlowResult.Succeeded
            ? ZLinkDiagnosticsLevel.Errors
            : RequiredLevel(outcome));

    internal static ILogger CreateLogger(ILoggerFactory? factory, ILogger? fallback = null) =>
        factory?.CreateLogger(LoggerCategory) ?? fallback ?? NullLogger.Instance;

    private static ZLinkDiagnosticsLevel RequiredLevel(ZLinkMessageFlowOutcome outcome)
    {
        return outcome is ZLinkMessageFlowOutcome.Dropped or ZLinkMessageFlowOutcome.Backpressured
            ? ZLinkDiagnosticsLevel.Errors
            : ZLinkDiagnosticsLevel.Normal;
    }

    private bool Sample(string? flowId, ulong sourceMeshGeneration)
    {
        var rate = _options.Diagnostics.SampleRate;
        if (rate >= 1.0d) return true;

        if (rate <= 0.0d) return false;

        const uint offset = 2166136261u;
        const uint prime = 16777619u;
        var hash = offset;
        if (flowId is not null)
        {
            foreach (var character in flowId)
            {
                hash ^= character;
                hash *= prime;
            }
        }
        else
        {
            var sequence = unchecked((ulong)Interlocked.Increment(ref _localSamplingSequence));
            Hash(sourceMeshGeneration);
            Hash(sequence);

            void Hash(ulong value)
            {
                for (var shift = 0; shift < 64; shift += 8)
                {
                    hash ^= (byte)(value >> shift);
                    hash *= prime;
                }
            }
        }

        return hash / 4294967296.0d < rate;
    }

    private void LogDefault(ZLinkMessageFlowEvent flow)
    {
        var level = ZLinkTraceFormat.ResolveLogLevel(flow);
        if (!_logger.IsEnabled(level)) return;

        var fields = ZLinkTraceFormat.StructuredFields(flow, flow.MessageSize);
        _logger.Log(level, default, new ZLinkStructuredLogState(fields), null,
            static (state, _) => state.ToString());
    }

    private void LogDefault(
        ZLinkDispatchFailure error,
        string? flowId,
        ZLinkFlowOrigin? flowOrigin)
    {
        var level = error.Reason == ZLinkDispatchErrorReason.HandlerException
            ? LogLevel.Error
            : LogLevel.Warning;
        if (!_logger.IsEnabled(level)) return;
        var fields = ZLinkTraceFormat.StructuredFields(error, flowId, flowOrigin);
        _logger.Log(level, default, new ZLinkStructuredLogState(fields), null,
            static (state, _) => state.ToString());
    }

    private static ZLinkMessageFlowEvent NormalizeFlowPair(ZLinkMessageFlowEvent flow)
    {
        var hasFlowId = !string.IsNullOrEmpty(flow.FlowId);
        var hasFlowOrigin = flow.FlowOrigin is not null;
        if (hasFlowId == hasFlowOrigin) return flow;
        return flow with { FlowId = string.Empty, FlowOrigin = null };
    }
}

internal static class ZLinkTraceFormat
{
    public static LogLevel ResolveLogLevel(ZLinkMessageFlowEvent flow)
    {
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
            ZLinkMessageFlowOutcome.ReplyReceived => "reply_received",
            ZLinkMessageFlowOutcome.Admitted => "admitted",
            ZLinkMessageFlowOutcome.Completed => "completed",
            ZLinkMessageFlowOutcome.Backpressured => "backpressured",
            _ => outcome.ToString().ToLowerInvariant()
        };
    }

    public static string ResultKey(ZLinkMessageFlowEvent flow) =>
        ResultKey(flow.Result ?? flow.Outcome switch
        {
            ZLinkMessageFlowOutcome.Dropped => ZLinkMessageFlowResult.Dropped,
            ZLinkMessageFlowOutcome.Backpressured => ZLinkMessageFlowResult.Backpressured,
            _ => ZLinkMessageFlowResult.Succeeded
        });

    public static string ResultKey(ZLinkMessageFlowResult result) => result switch
    {
        ZLinkMessageFlowResult.Succeeded => "succeeded",
        ZLinkMessageFlowResult.Failed => "failed",
        ZLinkMessageFlowResult.Backpressured => "backpressured",
        ZLinkMessageFlowResult.Dropped => "dropped",
        ZLinkMessageFlowResult.Cancelled => "cancelled",
        ZLinkMessageFlowResult.Shutdown => "shutdown",
        _ => result.ToString().ToLowerInvariant()
    };

    public static string? FlowIdKey(ZLinkMessageFlowEvent flow) =>
        !string.IsNullOrEmpty(flow.FlowId) && flow.FlowOrigin is not null
            ? flow.FlowId
            : null;

    public static string? FlowOriginKey(ZLinkMessageFlowEvent flow) =>
        FlowIdKey(flow) is not null
            ? flow.FlowOrigin!.Value.ToString().ToLowerInvariant()
            : null;

    public static string SurfaceKey(ZLinkDispatchErrorSurface surface) => surface switch
    {
        ZLinkDispatchErrorSurface.Node => "node",
        ZLinkDispatchErrorSurface.Channel or ZLinkDispatchErrorSurface.RouteMeshChannel => "channel",
        ZLinkDispatchErrorSurface.SpotRoute or ZLinkDispatchErrorSurface.SpotSubscription => "spot",
        ZLinkDispatchErrorSurface.InstanceSpot => "instance_spot",
        ZLinkDispatchErrorSurface.SpotActor => "actor",
        ZLinkDispatchErrorSurface.StreamSession => "stream",
        ZLinkDispatchErrorSurface.ActorRelocation => "actor_relocation",
        ZLinkDispatchErrorSurface.ClassicFanout => "classic_fanout",
        _ => throw new ArgumentOutOfRangeException(nameof(surface))
    };

    public static string MessageKindKey(ZLinkDispatchMessageKind kind) => kind switch
    {
        ZLinkDispatchMessageKind.Request or ZLinkDispatchMessageKind.ActorRequest => "request",
        ZLinkDispatchMessageKind.Send or ZLinkDispatchMessageKind.ActorSend => "send",
        ZLinkDispatchMessageKind.Response => "response",
        ZLinkDispatchMessageKind.Error => "error",
        ZLinkDispatchMessageKind.Control => "control",
        ZLinkDispatchMessageKind.Publish => "send",
        _ => throw new ArgumentOutOfRangeException(nameof(kind))
    };

    public static string? ChannelRouteKind(
        ZLinkDispatchErrorSurface surface,
        string? explicitKind = null) => surface switch
        {
            ZLinkDispatchErrorSurface.RouteMeshChannel => "route_mesh",
            ZLinkDispatchErrorSurface.Channel => explicitKind switch
            {
                null or "" => "client_server",
                "route_mesh" or "RouteMesh" or "route-mesh" => "route_mesh",
                "client_server" or "ClientServer" or "client-server" => "client_server",
                _ => throw new ArgumentOutOfRangeException(nameof(explicitKind))
            },
            _ => null
        };

    public static string? ActivationStateKey(string? activationState) => activationState switch
    {
        null or "" => null,
        "activating" or "Activating" => "activating",
        "ready" or "Ready" => "ready",
        "closing" or "Closing" => "closing",
        _ => throw new ArgumentOutOfRangeException(nameof(activationState))
    };

    public static string DispatchReasonKey(ZLinkDispatchErrorReason reason) => reason switch
    {
        ZLinkDispatchErrorReason.HandlerMissing => "no_handler",
        ZLinkDispatchErrorReason.PayloadDecodeFailed => "decode_error",
        ZLinkDispatchErrorReason.HandlerException => "handler_exception",
        ZLinkDispatchErrorReason.InvalidFrame => "invalid_frame",
        ZLinkDispatchErrorReason.ReplyPathMissing => "reply_path_missing",
        ZLinkDispatchErrorReason.UnexpectedReply => "unexpected_reply",
        ZLinkDispatchErrorReason.Backpressure => "backpressure",
        ZLinkDispatchErrorReason.StaleTarget => "stale_target",
        ZLinkDispatchErrorReason.Shutdown => "shutdown",
        _ => throw new ArgumentOutOfRangeException(nameof(reason))
    };

    public static string DispatchActionKey(ZLinkDispatchErrorAction action) => action switch
    {
        ZLinkDispatchErrorAction.ReplyError => "reply_error",
        ZLinkDispatchErrorAction.FailCaller => "fail_caller",
        ZLinkDispatchErrorAction.Drop => "drop",
        _ => throw new ArgumentOutOfRangeException(nameof(action))
    };

    public static string? MessageReasonKey(ZLinkMessageFlowReason? reason) => reason switch
    {
        null => null,
        ZLinkMessageFlowReason.Backpressure => "backpressure",
        ZLinkMessageFlowReason.StaleTarget => "stale_target",
        ZLinkMessageFlowReason.TargetClosed => "target_closed",
        ZLinkMessageFlowReason.Shutdown => "shutdown",
        ZLinkMessageFlowReason.LocationUnavailable => "location_unavailable",
        ZLinkMessageFlowReason.ActivationRejected => "activation_rejected",
        ZLinkMessageFlowReason.ActivationTimeout => "activation_timeout",
        _ => throw new ArgumentOutOfRangeException(nameof(reason))
    };

    public static IReadOnlyList<KeyValuePair<string, object?>> StructuredFields(
        ZLinkMessageFlowEvent flow,
        long? size)
    {
        var fields = new List<KeyValuePair<string, object?>>(22);
        //  Structured log 본문의 key는 관찰 스펙의 "Structured log 대체 표기"가 고정한다 —
        //  첫 key는 `event`다. telemetry attribute 이름(`event_id`)과는 다른 집합이다.
        Add(fields, "event", "zlink.message_flow");
        Add(fields, "phase", OutcomeKey(flow.Outcome));
        Add(fields, "surface", SurfaceKey(flow.Surface));
        Add(fields, "kind", MessageKindKey(flow.MessageKind));
        Add(fields, "mesh", flow.MeshName);
        Add(fields, "channel", flow.ChannelName);
        Add(fields, "channel_route", ChannelRouteKind(flow.Surface, flow.ChannelRouteKind));
        Add(fields, "source_rid", flow.SourceRid);
        Add(fields, "target_rid", flow.TargetRid ?? flow.PeerRid);
        Add(fields, "server_rid", flow.ServerRid);
        Add(fields, "packet", flow.PacketName);
        Add(fields, "topic", flow.Topic);
        Add(fields, "spot", flow.SpotId);
        Add(fields, "instance_type", flow.InstanceSpotType);
        Add(fields, "activation_state", ActivationStateKey(flow.ActivationState));
        Add(fields, "actor", flow.ActorId);
        Add(fields, "corr", flow.CorrelationId);
        Add(fields, "flow", FlowIdKey(flow));
        Add(fields, "origin", FlowOriginKey(flow));
        Add(fields, "outcome", ResultKey(flow));
        Add(fields, "reason", MessageReasonKey(flow.Reason));
        Add(fields, "size", size);
        return fields;
    }

    public static IReadOnlyList<KeyValuePair<string, object?>> StructuredFields(
        ZLinkDispatchFailure error,
        string? flowId,
        ZLinkFlowOrigin? flowOrigin)
    {
        var fields = new List<KeyValuePair<string, object?>>(20);
        Add(fields, "event", "zlink.dispatch_error");
        Add(fields, "surface", SurfaceKey(error.Surface));
        Add(fields, "kind", MessageKindKey(error.MessageKind));
        Add(fields, "mesh", error.MeshName);
        Add(fields, "channel", error.ChannelName);
        Add(fields, "channel_route", ChannelRouteKind(error.Surface, error.ChannelRouteKind));
        Add(fields, "source_rid", error.SourceRid);
        Add(fields, "target_rid", error.TargetRid);
        Add(fields, "server_rid", error.ServerRid);
        Add(fields, "packet", error.PacketName);
        Add(fields, "topic", error.Topic);
        Add(fields, "spot", error.SpotId);
        Add(fields, "instance_type", error.InstanceSpotType);
        Add(fields, "activation_state", ActivationStateKey(error.ActivationState));
        Add(fields, "actor", error.ActorId);
        Add(fields, "corr", error.CorrelationId);
        Add(fields, "flow", string.IsNullOrEmpty(flowId) || flowOrigin is null ? null : flowId);
        Add(fields, "origin", string.IsNullOrEmpty(flowId) || flowOrigin is null
            ? null
            : flowOrigin.Value.ToString().ToLowerInvariant());
        Add(fields, "outcome", "failed");
        Add(fields, "reason", DispatchReasonKey(error.Reason));
        return fields;
    }

    private static void Add(
        ICollection<KeyValuePair<string, object?>> fields,
        string key,
        object? value)
    {
        if (value is not null and not "")
            fields.Add(new KeyValuePair<string, object?>(key, value));
    }
}

internal sealed class ZLinkStructuredLogState(
    IReadOnlyList<KeyValuePair<string, object?>> fields) :
    IReadOnlyList<KeyValuePair<string, object?>>
{
    public int Count => fields.Count;

    public KeyValuePair<string, object?> this[int index] => fields[index];

    public IEnumerator<KeyValuePair<string, object?>> GetEnumerator() => fields.GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();

    public override string ToString()
    {
        var message = new StringBuilder("zlink flow:");
        foreach (var field in fields)
            message.Append(' ').Append(field.Key).Append('=').Append(field.Value);
        return message.ToString();
    }

}
