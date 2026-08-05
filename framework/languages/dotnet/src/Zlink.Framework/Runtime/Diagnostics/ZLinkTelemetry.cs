using System.Diagnostics;
using System.Diagnostics.Metrics;

namespace Zlink.Framework.Runtime.Diagnostics;

internal static class ZLinkTelemetry
{
    public const string ActivitySourceName = "Zlink.Framework";
    public const string MeterName = ZLinkMeters.Framework;

    public static readonly ActivitySource ActivitySource = new(ActivitySourceName);

    private static int _diagnosticsLevel;

    public static void SetDiagnosticsLevel(ZLinkDiagnosticsLevel level) =>
        Volatile.Write(ref _diagnosticsLevel, (int)level);

    public static string CaptureSubmitOperationId()
    {
        if (Volatile.Read(ref _diagnosticsLevel) == (int)ZLinkDiagnosticsLevel.Off
            || !ActivitySource.HasListeners())
            return string.Empty;
        return Activity.Current?.Id ?? Guid.NewGuid().ToString("N");
    }

    public static void TraceSubmitAdmission(
        string operationId,
        string eventName,
        int pendingWaiterCount,
        bool retry = false)
    {
        if (string.IsNullOrEmpty(operationId)
            || Volatile.Read(ref _diagnosticsLevel) == (int)ZLinkDiagnosticsLevel.Off
            || !ActivitySource.HasListeners())
            return;

        using var activity = ActivitySource.StartActivity(
            "zlink.submit.admission",
            ActivityKind.Internal);
        if (activity is null) return;

        activity.SetTag("zlink.submit.operation_id", operationId);
        activity.SetTag("zlink.submit.event", eventName);
        activity.SetTag("zlink.submit.retry", retry);
        activity.SetTag("zlink.submit.pending_waiters", pendingWaiterCount);
        activity.SetTag("zlink.submit.reservations", pendingWaiterCount);
        activity.SetTag("zlink.submit.callbacks", 0);
        activity.AddEvent(new ActivityEvent(eventName));
    }

    public static void RecordHandlerMissing(
        string surface,
        string kind,
        string action,
        string reason)
    {
        _ = surface;
        _ = kind;
        _ = action;
        _ = reason;
    }

    public static void RecordDropped(
        string surface,
        string kind,
        string reason)
    {
        _ = surface;
        _ = kind;
        _ = reason;
    }

    public static void RecordReplyError(
        string surface,
        string kind,
        string reason)
    {
        _ = surface;
        _ = kind;
        _ = reason;
    }

    public static void TraceFlowEvent(
        string eventName,
        ZLinkMessageFlowEvent flow,
        string action,
        string reason,
        string? surfaceName = null,
        string? kindName = null,
        string? actorType = null)
    {
        if (Volatile.Read(ref _diagnosticsLevel) == (int)ZLinkDiagnosticsLevel.Off
            || !ActivitySource.HasListeners())
            return;

        surfaceName ??= flow.Surface.ToString();
        kindName ??= flow.MessageKind.ToString();
        using var activity = ActivitySource.StartActivity(
            ResolveSpanName(flow.Surface),
            ActivityKind.Consumer);
        if (activity is null) return;

        activity.SetTag("event_id", "zlink.dispatch_error");
        activity.SetTag("outcome", "failed");
        activity.SetTag("zlink.surface", surfaceName);
        activity.SetTag("zlink.kind", kindName);
        activity.SetTag("zlink.packet.name", flow.PacketName);
        activity.SetTag("zlink.action", NormalizeDispatchAction(action, reason));
        activity.SetTag("zlink.reason", NormalizeDispatchReason(reason));
        if (!string.IsNullOrEmpty(flow.ChannelName)) activity.SetTag("zlink.channel.name", flow.ChannelName);
        if (!string.IsNullOrEmpty(flow.ActorId)) activity.SetTag("zlink.actor.id", flow.ActorId);
        if (!string.IsNullOrEmpty(actorType)) activity.SetTag("zlink.actor.type", actorType);
        if (!string.IsNullOrEmpty(flow.SpotId)) activity.SetTag("spot_id", flow.SpotId);
        activity.AddEvent(new ActivityEvent(eventName));
    }

    private static string NormalizeDispatchAction(string action, string reason) =>
        action switch
        {
            "reply-error" => "reply_error",
            "fail-caller" => "fail_caller",
            "rejected" when reason == "reply-path-missing" => "fail_caller",
            "rejected" => "reply_error",
            _ => action.Replace('-', '_')
        };

    private static string NormalizeDispatchReason(string reason) =>
        reason switch
        {
            "handler-missing" or "no-handler" or "no-join-handler" => "no_handler",
            "payload-decode-failed" => "decode_error",
            "invalid-frame" => "invalid_frame",
            "reply-path-missing" => "reply_path_missing",
            "unexpected-reply" => "unexpected_reply",
            "stale-route" => "stale_target",
            _ => reason.Replace('-', '_')
        };

    public static void TraceMessageFlow(ZLinkMessageFlowEvent flow)
    {
        if (Volatile.Read(ref _diagnosticsLevel) == (int)ZLinkDiagnosticsLevel.Off
            || !ActivitySource.HasListeners())
            return;

        using var activity = ActivitySource.StartActivity(
            "zlink.message_flow",
            ActivityKind.Internal);
        if (activity is null) return;

        activity.SetTag("event_id",
            flow.Outcome == ZLinkMessageFlowOutcome.Error
                ? "zlink.dispatch_error"
                : "zlink.message_flow");
        activity.SetTag("phase", ZLinkTraceFormat.OutcomeKey(flow.Outcome));
        activity.SetTag("surface", flow.Surface.ToString());
        activity.SetTag("message_kind", flow.MessageKind.ToString());
        activity.SetTag("packet_name", flow.PacketName);
        activity.SetTag("channel_name", flow.ChannelName);
        activity.SetTag("topic", flow.Topic);
        activity.SetTag("spot_id", flow.SpotId);
        activity.SetTag("actor_id", flow.ActorId);
        activity.SetTag("source_rid", flow.SourceRid);
        activity.SetTag("target_rid", flow.PeerRid);
        activity.SetTag("correlation_id", flow.CorrelationId);
        activity.SetTag("flow_id", ZLinkTraceFormat.FlowIdKey(flow));
        activity.SetTag("flow_origin", ZLinkTraceFormat.FlowOriginKey(flow));
        activity.SetTag("reason", flow.ErrorReason?.ToString());
        activity.SetTag("action", flow.ErrorAction?.ToString());
        activity.AddEvent(new ActivityEvent("zlink.message_flow"));
    }

    private static string ResolveSpanName(ZLinkDispatchErrorSurface surface)
    {
        return surface switch
        {
            ZLinkDispatchErrorSurface.StreamSession => "zlink.session.dispatch",
            ZLinkDispatchErrorSurface.Channel => "zlink.channel.dispatch",
            ZLinkDispatchErrorSurface.RouteMeshChannel => "zlink.route.dispatch",
            ZLinkDispatchErrorSurface.SpotRoute or ZLinkDispatchErrorSurface.SpotSubscription => "zlink.spot.dispatch",
            ZLinkDispatchErrorSurface.SpotActor => "zlink.actor.dispatch",
            _ => "zlink.message.dispatch"
        };
    }
}
