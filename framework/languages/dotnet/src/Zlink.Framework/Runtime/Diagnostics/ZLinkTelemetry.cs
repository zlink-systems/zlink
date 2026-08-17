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

    public static bool IsSubmitAdmissionTracingEnabled(string operationId) =>
        !string.IsNullOrEmpty(operationId)
        && Volatile.Read(ref _diagnosticsLevel) != (int)ZLinkDiagnosticsLevel.Off
        && ActivitySource.HasListeners();

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

    public static void TraceMessageFlow(ZLinkMessageFlowEvent flow)
    {
        if (!ActivitySource.HasListeners())
            return;

        using var activity = ActivitySource.StartActivity(
            "zlink.message_flow",
            ActivityKind.Internal);
        if (activity is null) return;

        activity.SetTag("event_id", "zlink.message_flow");
        activity.SetTag("phase", ZLinkTraceFormat.OutcomeKey(flow.Outcome));
        activity.SetTag("surface", ZLinkTraceFormat.SurfaceKey(flow.Surface));
        activity.SetTag("message_kind", ZLinkTraceFormat.MessageKindKey(flow.MessageKind));
        activity.SetTag("outcome", ZLinkTraceFormat.ResultKey(flow));
        activity.SetTag("packet_name", flow.PacketName);
        activity.SetTag("channel_name", flow.ChannelName);
        activity.SetTag("channel_route_kind",
            ZLinkTraceFormat.ChannelRouteKind(flow.Surface, flow.ChannelRouteKind));
        activity.SetTag("mesh_name", flow.MeshName);
        activity.SetTag("topic", flow.Topic);
        activity.SetTag("spot_id", flow.SpotId);
        activity.SetTag("instance_spot_type", flow.InstanceSpotType);
        activity.SetTag("activation_state", ZLinkTraceFormat.ActivationStateKey(flow.ActivationState));
        activity.SetTag("actor_id", flow.ActorId);
        activity.SetTag("source_rid", flow.SourceRid);
        activity.SetTag("target_rid", flow.TargetRid ?? flow.PeerRid);
        activity.SetTag("server_rid", flow.ServerRid);
        activity.SetTag("correlation_id", flow.CorrelationId);
        activity.SetTag("flow_id", ZLinkTraceFormat.FlowIdKey(flow));
        activity.SetTag("flow_origin", ZLinkTraceFormat.FlowOriginKey(flow));
        activity.SetTag("reason", ZLinkTraceFormat.MessageReasonKey(flow.Reason));
        activity.SetTag("message_size_bytes", flow.MessageSize);
        activity.SetTag("duration_seconds", flow.DurationSeconds);
        activity.AddEvent(new ActivityEvent("zlink.message_flow"));
    }

    public static void TraceDispatchError(
        ZLinkDispatchFailure error,
        string? flowId,
        ZLinkFlowOrigin? flowOrigin)
    {
        if (!ActivitySource.HasListeners())
            return;

        using var activity = ActivitySource.StartActivity(
            "zlink.dispatch_error",
            ActivityKind.Consumer);
        if (activity is null) return;

        activity.SetTag("event_id", "zlink.dispatch_error");
        activity.SetTag("outcome", "failed");
        activity.SetTag("surface", ZLinkTraceFormat.SurfaceKey(error.Surface));
        activity.SetTag("message_kind", ZLinkTraceFormat.MessageKindKey(error.MessageKind));
        activity.SetTag("reason", ZLinkTraceFormat.DispatchReasonKey(error.Reason));
        activity.SetTag("action", ZLinkTraceFormat.DispatchActionKey(error.Action));
        activity.SetTag("packet_name", error.PacketName);
        activity.SetTag("channel_name", error.ChannelName);
        activity.SetTag("channel_route_kind",
            ZLinkTraceFormat.ChannelRouteKind(error.Surface, error.ChannelRouteKind));
        activity.SetTag("mesh_name", error.MeshName);
        activity.SetTag("topic", error.Topic);
        activity.SetTag("spot_id", error.SpotId);
        activity.SetTag("instance_spot_type", error.InstanceSpotType);
        activity.SetTag("activation_state", ZLinkTraceFormat.ActivationStateKey(error.ActivationState));
        activity.SetTag("actor_id", error.ActorId);
        activity.SetTag("source_rid", error.SourceRid);
        activity.SetTag("target_rid", error.TargetRid);
        activity.SetTag("server_rid", error.ServerRid);
        activity.SetTag("correlation_id", error.CorrelationId);
        if (!string.IsNullOrEmpty(flowId) && flowOrigin is not null)
        {
            activity.SetTag("flow_id", flowId);
            activity.SetTag("flow_origin", flowOrigin.Value.ToString().ToLowerInvariant());
        }
        activity.AddEvent(new ActivityEvent("zlink.dispatch_error"));
    }

}
