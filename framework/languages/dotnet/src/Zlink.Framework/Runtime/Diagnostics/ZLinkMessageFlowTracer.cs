using System;
using System.Collections;
using System.Text;
using System.Text.RegularExpressions;
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
        IZLinkRuntimeFailureReporter? errorSink = null
    )
    {
        _options = options;
        _errorSink = errorSink ?? (runtime is null ? null : runtime.ErrorSink);
        _logger = logger ?? NullLogger.Instance;
    }

    public bool CaptureEnabled => _options.Diagnostics.EffectiveLevel != ZLinkDiagnosticsLevel.Off;

    internal bool DetailedEnabled =>
        _options.Diagnostics.EffectiveLevel >= ZLinkDiagnosticsLevel.Detailed;

    // Cheap mode gate (relaxed/volatile read of the live mode). Build the event only
    // after this returns true.
    public bool Enabled(ZLinkMessageFlowOutcome outcome)
    {
        return ShouldLog(outcome);
    }

    internal bool Enabled(ZLinkMessageFlowOutcome outcome, ZLinkMessageFlowResult? result) =>
        ShouldLog(outcome, result);

    public void Trace(ZLinkMessageFlowEvent flow)
    {
        var logEnabled = ShouldLog(flow.Outcome, flow.Result);
        if (!logEnabled)
            return;

        flow = NormalizeFlowPair(flow);
        if (string.IsNullOrEmpty(flow.FlowId))
        {
            var current = ZLinkFlowContext.Current;
            if (current is { } value)
                flow = flow with { FlowId = value.FlowId, FlowOrigin = value.Origin };
        }

        var diagnostics = _options.Diagnostics;
        if (
            flow.MessageSize is not null
            && (
                diagnostics.EffectiveLevel < ZLinkDiagnosticsLevel.Detailed
                || !diagnostics.MessageSizesIncluded
            )
        )
            flow = flow with { MessageSize = null };
        if (
            flow.DurationSeconds is not null
            && diagnostics.EffectiveLevel < ZLinkDiagnosticsLevel.Detailed
        )
            flow = flow with { DurationSeconds = null };

        // Sampling thins healthy traffic; failures, drops, and backpressure always pass through.
        var logSampled =
            flow.Outcome is ZLinkMessageFlowOutcome.Dropped or ZLinkMessageFlowOutcome.Backpressured
            || flow.Result is not null and not ZLinkMessageFlowResult.Succeeded
            || Sample(
                ZLinkTraceFormat.FlowIdKey(flow),
                flow.SourceMeshGeneration ?? _options.Diagnostics.SourceMeshGeneration
            );
        if (!logSampled)
            return;

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
        if (_options.Diagnostics.EffectiveLevel == ZLinkDiagnosticsLevel.Off)
            return;

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
            var errorDetails = ZLinkTraceFormat.DispatchErrorDetails(error.Exception);
            LogDefault(error, errorDetails, flowId, flowOrigin);
            ZLinkTelemetry.TraceDispatchError(error, errorDetails, flowId, flowOrigin);
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

    private bool ShouldLog(ZLinkMessageFlowOutcome outcome, ZLinkMessageFlowResult? result) =>
        (int)_options.Diagnostics.EffectiveLevel
        >= (int)(
            result is not null and not ZLinkMessageFlowResult.Succeeded
                ? ZLinkDiagnosticsLevel.Errors
                : RequiredLevel(outcome)
        );

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
        if (rate >= 1.0d)
            return true;

        if (rate <= 0.0d)
            return false;

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
        if (!_logger.IsEnabled(level))
            return;

        var fields = ZLinkTraceFormat.StructuredFields(flow, flow.MessageSize);
        _logger.Log(
            level,
            default,
            new ZLinkStructuredLogState(fields),
            null,
            static (state, _) => state.ToString()
        );
    }

    private void LogDefault(
        ZLinkDispatchFailure error,
        ZLinkDispatchErrorDetails errorDetails,
        string? flowId,
        ZLinkFlowOrigin? flowOrigin
    )
    {
        var level =
            error.Reason == ZLinkDispatchErrorReason.HandlerException
                ? LogLevel.Error
                : LogLevel.Warning;
        if (!_logger.IsEnabled(level))
            return;
        var fields = ZLinkTraceFormat.StructuredFields(error, errorDetails, flowId, flowOrigin);
        _logger.Log(
            level,
            default,
            new ZLinkStructuredLogState(fields),
            null,
            static (state, _) => state.ToString()
        );
    }

    private static ZLinkMessageFlowEvent NormalizeFlowPair(ZLinkMessageFlowEvent flow)
    {
        var hasFlowId = !string.IsNullOrEmpty(flow.FlowId);
        var hasFlowOrigin = flow.FlowOrigin is not null;
        if (hasFlowId == hasFlowOrigin)
            return flow;
        return flow with { FlowId = string.Empty, FlowOrigin = null };
    }
}

internal static class ZLinkTraceFormat
{
    internal const string EventField = "event";
    internal const string PhaseField = "phase";
    internal const string SurfaceField = "surface";
    internal const string KindField = "kind";
    internal const string MeshField = "mesh";
    internal const string ChannelField = "channel";
    internal const string ChannelRouteField = "channel_route";
    internal const string SourceRidField = "source_rid";
    internal const string TargetRidField = "target_rid";
    internal const string ServerRidField = "server_rid";
    internal const string PacketField = "packet";
    internal const string TopicField = "topic";
    internal const string SpotField = "spot";
    internal const string InstanceTypeField = "instance_type";
    internal const string ActivationStateField = "activation_state";
    internal const string ActorField = "actor";
    internal const string SessionField = "session";
    internal const string CorrField = "corr";
    internal const string FlowField = "flow";
    internal const string OriginField = "origin";
    internal const string OutcomeField = "outcome";
    internal const string ReasonField = "reason";
    internal const string SizeField = "size";
    internal const string ActionField = "action";
    internal const string EventIdField = "event_id";
    internal const string MessageKindField = "message_kind";
    internal const string PacketNameField = "packet_name";
    internal const string ChannelNameField = "channel_name";
    internal const string ChannelRouteKindField = "channel_route_kind";
    internal const string MeshNameField = "mesh_name";
    internal const string SpotIdField = "spot_id";
    internal const string InstanceSpotTypeField = "instance_spot_type";
    internal const string ActorIdField = "actor_id";
    internal const string StreamSessionIdField = "stream_session_id";
    internal const string CorrelationIdField = "correlation_id";
    internal const string FlowIdField = "flow_id";
    internal const string FlowOriginField = "flow_origin";
    internal const string MessageSizeBytesField = "message_size_bytes";
    internal const string DurationSecondsField = "duration_seconds";
    internal const string ErrorTypeField = "error_type";
    internal const string ErrorMessageField = "error_message";
    internal const string MessageFlowEventName = "zlink.message_flow";
    internal const string DispatchErrorEventName = "zlink.dispatch_error";

    private const int MessageFlowFieldCapacity = 23;
    private const int DispatchErrorFieldCapacity = 21;
    internal const int ErrorMessageMaxLength = 512;
    private static readonly (Regex Pattern, string Replacement)[] CredentialPatterns =
    [
        (
            new(@"Authorization\s*:\s*(?:(?:Bearer|Basic)\s+)?[^\s,;]+", RegexOptions.IgnoreCase),
            "Authorization: <redacted>"
        ),
        (new(@"Bearer\s+[^\s,;]+", RegexOptions.IgnoreCase), "Bearer <redacted>"),
        (new(@"password\s*=\s*[^\s,;]+", RegexOptions.IgnoreCase), "password=<redacted>"),
        (new(@"token\s*=\s*[^\s,;]+", RegexOptions.IgnoreCase), "token=<redacted>"),
    ];

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
            _ => outcome.ToString().ToLowerInvariant(),
        };
    }

    public static string ResultKey(ZLinkMessageFlowEvent flow) =>
        ResultKey(
            flow.Result
                ?? flow.Outcome switch
                {
                    ZLinkMessageFlowOutcome.Dropped => ZLinkMessageFlowResult.Dropped,
                    ZLinkMessageFlowOutcome.Backpressured => ZLinkMessageFlowResult.Backpressured,
                    _ => ZLinkMessageFlowResult.Succeeded,
                }
        );

    public static string ResultKey(ZLinkMessageFlowResult result) =>
        result switch
        {
            ZLinkMessageFlowResult.Succeeded => "succeeded",
            ZLinkMessageFlowResult.Failed => "failed",
            ZLinkMessageFlowResult.Backpressured => "backpressured",
            ZLinkMessageFlowResult.Dropped => "dropped",
            ZLinkMessageFlowResult.Cancelled => "cancelled",
            ZLinkMessageFlowResult.Shutdown => "shutdown",
            _ => result.ToString().ToLowerInvariant(),
        };

    public static string? FlowIdKey(ZLinkMessageFlowEvent flow) =>
        !string.IsNullOrEmpty(flow.FlowId) && flow.FlowOrigin is not null ? flow.FlowId : null;

    public static string? FlowOriginKey(ZLinkMessageFlowEvent flow) =>
        FlowIdKey(flow) is not null ? flow.FlowOrigin!.Value.ToString().ToLowerInvariant() : null;

    public static string SurfaceKey(ZLinkDispatchErrorSurface surface) =>
        surface switch
        {
            ZLinkDispatchErrorSurface.Node => "node",
            ZLinkDispatchErrorSurface.Channel or ZLinkDispatchErrorSurface.RouteMeshChannel =>
                "channel",
            ZLinkDispatchErrorSurface.SpotRoute or ZLinkDispatchErrorSurface.SpotSubscription =>
                "spot",
            ZLinkDispatchErrorSurface.InstanceSpot => "instance_spot",
            ZLinkDispatchErrorSurface.SpotActor => "actor",
            ZLinkDispatchErrorSurface.StreamSession => "stream",
            ZLinkDispatchErrorSurface.ActorRelocation => "actor_relocation",
            ZLinkDispatchErrorSurface.ClassicFanout => "classic_fanout",
            _ => throw new ArgumentOutOfRangeException(nameof(surface)),
        };

    public static string MessageKindKey(ZLinkDispatchMessageKind kind) =>
        kind switch
        {
            ZLinkDispatchMessageKind.Request or ZLinkDispatchMessageKind.ActorRequest => "request",
            ZLinkDispatchMessageKind.Send or ZLinkDispatchMessageKind.ActorSend => "send",
            ZLinkDispatchMessageKind.Response => "response",
            ZLinkDispatchMessageKind.Error => "error",
            ZLinkDispatchMessageKind.Control => "control",
            ZLinkDispatchMessageKind.Publish => "send",
            _ => throw new ArgumentOutOfRangeException(nameof(kind)),
        };

    public static string? ChannelRouteKind(
        ZLinkDispatchErrorSurface surface,
        string? explicitKind = null
    ) =>
        surface switch
        {
            ZLinkDispatchErrorSurface.RouteMeshChannel => "route_mesh",
            ZLinkDispatchErrorSurface.Channel => explicitKind switch
            {
                null or "" => "client_server",
                "route_mesh" or "RouteMesh" or "route-mesh" => "route_mesh",
                "client_server" or "ClientServer" or "client-server" => "client_server",
                _ => throw new ArgumentOutOfRangeException(nameof(explicitKind)),
            },
            _ => null,
        };

    public static string? ActivationStateKey(string? activationState) =>
        activationState switch
        {
            null or "" => null,
            "activating" or "Activating" => "activating",
            "ready" or "Ready" => "ready",
            "closing" or "Closing" => "closing",
            _ => throw new ArgumentOutOfRangeException(nameof(activationState)),
        };

    public static string DispatchReasonKey(ZLinkDispatchErrorReason reason) =>
        reason switch
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
            _ => throw new ArgumentOutOfRangeException(nameof(reason)),
        };

    public static string DispatchActionKey(ZLinkDispatchErrorAction action) =>
        action switch
        {
            ZLinkDispatchErrorAction.ReplyError => "reply_error",
            ZLinkDispatchErrorAction.FailCaller => "fail_caller",
            ZLinkDispatchErrorAction.Drop => "drop",
            _ => throw new ArgumentOutOfRangeException(nameof(action)),
        };

    public static string? MessageReasonKey(ZLinkMessageFlowReason? reason) =>
        reason switch
        {
            null => null,
            ZLinkMessageFlowReason.Backpressure => "backpressure",
            ZLinkMessageFlowReason.StaleTarget => "stale_target",
            ZLinkMessageFlowReason.TargetClosed => "target_closed",
            ZLinkMessageFlowReason.Shutdown => "shutdown",
            ZLinkMessageFlowReason.LocationUnavailable => "location_unavailable",
            ZLinkMessageFlowReason.ActivationRejected => "activation_rejected",
            ZLinkMessageFlowReason.ActivationTimeout => "activation_timeout",
            _ => throw new ArgumentOutOfRangeException(nameof(reason)),
        };

    public static IReadOnlyList<KeyValuePair<string, object?>> StructuredFields(
        ZLinkMessageFlowEvent flow,
        long? size
    )
    {
        var fields = new List<KeyValuePair<string, object?>>(MessageFlowFieldCapacity);
        //  Structured log 본문의 key는 관찰 스펙의 "Structured log 대체 표기"가 고정한다 —
        //  첫 key는 `event`다. telemetry attribute 이름(`event_id`)과는 다른 집합이다.
        Add(fields, EventField, MessageFlowEventName);
        Add(fields, PhaseField, OutcomeKey(flow.Outcome));
        Add(fields, SurfaceField, SurfaceKey(flow.Surface));
        Add(fields, KindField, MessageKindKey(flow.MessageKind));
        Add(fields, MeshField, flow.MeshName);
        Add(fields, ChannelField, flow.ChannelName);
        Add(fields, ChannelRouteField, ChannelRouteKind(flow.Surface, flow.ChannelRouteKind));
        Add(fields, SourceRidField, flow.SourceRid);
        Add(fields, TargetRidField, flow.TargetRid ?? flow.PeerRid);
        Add(fields, ServerRidField, flow.ServerRid);
        Add(fields, PacketField, flow.PacketName);
        Add(fields, TopicField, flow.Topic);
        Add(fields, SpotField, flow.SpotId);
        Add(fields, InstanceTypeField, flow.InstanceSpotType);
        Add(fields, ActivationStateField, ActivationStateKey(flow.ActivationState));
        Add(fields, ActorField, flow.ActorId);
        Add(fields, SessionField, flow.StreamSessionId);
        Add(fields, CorrField, flow.CorrelationId);
        Add(fields, FlowField, FlowIdKey(flow));
        Add(fields, OriginField, FlowOriginKey(flow));
        Add(fields, OutcomeField, ResultKey(flow));
        Add(fields, ReasonField, MessageReasonKey(flow.Reason));
        Add(fields, SizeField, size);
        return fields;
    }

    public static IReadOnlyList<KeyValuePair<string, object?>> StructuredFields(
        ZLinkDispatchFailure error,
        ZLinkDispatchErrorDetails errorDetails,
        string? flowId,
        ZLinkFlowOrigin? flowOrigin
    )
    {
        var fields = new List<KeyValuePair<string, object?>>(DispatchErrorFieldCapacity);
        Add(fields, EventField, DispatchErrorEventName);
        Add(fields, SurfaceField, SurfaceKey(error.Surface));
        Add(fields, KindField, MessageKindKey(error.MessageKind));
        Add(fields, MeshField, error.MeshName);
        Add(fields, ChannelField, error.ChannelName);
        Add(fields, ChannelRouteField, ChannelRouteKind(error.Surface, error.ChannelRouteKind));
        Add(fields, SourceRidField, error.SourceRid);
        Add(fields, TargetRidField, error.TargetRid);
        Add(fields, ServerRidField, error.ServerRid);
        Add(fields, PacketField, error.PacketName);
        Add(fields, TopicField, error.Topic);
        Add(fields, SpotField, error.SpotId);
        Add(fields, InstanceTypeField, error.InstanceSpotType);
        Add(fields, ActivationStateField, ActivationStateKey(error.ActivationState));
        Add(fields, ActorField, error.ActorId);
        Add(fields, SessionField, error.StreamSessionId);
        Add(fields, CorrField, error.CorrelationId);
        Add(fields, FlowField, string.IsNullOrEmpty(flowId) || flowOrigin is null ? null : flowId);
        Add(
            fields,
            OriginField,
            string.IsNullOrEmpty(flowId) || flowOrigin is null
                ? null
                : flowOrigin.Value.ToString().ToLowerInvariant()
        );
        Add(fields, OutcomeField, ResultKey(ZLinkMessageFlowResult.Failed));
        Add(fields, ReasonField, DispatchReasonKey(error.Reason));
        Add(fields, ActionField, DispatchActionKey(error.Action));
        if (errorDetails.Type is not null)
        {
            fields.Add(new KeyValuePair<string, object?>(ErrorTypeField, errorDetails.Type));
            fields.Add(new KeyValuePair<string, object?>(ErrorMessageField, errorDetails.Message));
        }
        return fields;
    }

    public static ZLinkDispatchErrorDetails DispatchErrorDetails(Exception? error)
    {
        if (error is null)
            return default;

        var message = error.Message;
        if (!string.IsNullOrEmpty(message))
        {
            var lineEnd = message.IndexOfAny(['\r', '\n']);
            if (lineEnd >= 0)
                message = message[..lineEnd];
            foreach (var (pattern, replacement) in CredentialPatterns)
                message = pattern.Replace(message, replacement);
            if (message.Length > ErrorMessageMaxLength)
                message = message[..ErrorMessageMaxLength];
        }

        return new ZLinkDispatchErrorDetails(error.GetType().Name, message ?? string.Empty);
    }

    private static void Add(
        ICollection<KeyValuePair<string, object?>> fields,
        string key,
        object? value
    )
    {
        if (value is not null and not "")
            fields.Add(new KeyValuePair<string, object?>(key, value));
    }
}

internal readonly record struct ZLinkDispatchErrorDetails(string? Type, string? Message);

internal sealed class ZLinkStructuredLogState(IReadOnlyList<KeyValuePair<string, object?>> fields)
    : IReadOnlyList<KeyValuePair<string, object?>>
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
