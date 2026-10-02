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

    public static void TraceMessageFlow(ZLinkMessageFlowEvent flow)
    {
        if (!ActivitySource.HasListeners())
            return;

        using var activity = ActivitySource.StartActivity(
            ZLinkTraceFormat.MessageFlowEventName,
            ActivityKind.Internal
        );
        if (activity is null)
            return;

        activity.SetTag(ZLinkTraceFormat.EventIdField, ZLinkTraceFormat.MessageFlowEventName);
        activity.SetTag(ZLinkTraceFormat.PhaseField, ZLinkTraceFormat.OutcomeKey(flow.Outcome));
        activity.SetTag(ZLinkTraceFormat.SurfaceField, ZLinkTraceFormat.SurfaceKey(flow.Surface));
        activity.SetTag(
            ZLinkTraceFormat.MessageKindField,
            ZLinkTraceFormat.MessageKindKey(flow.MessageKind)
        );
        activity.SetTag(ZLinkTraceFormat.OutcomeField, ZLinkTraceFormat.ResultKey(flow));
        activity.SetTag(ZLinkTraceFormat.PacketNameField, flow.PacketName);
        activity.SetTag(ZLinkTraceFormat.ChannelNameField, flow.ChannelName);
        activity.SetTag(
            ZLinkTraceFormat.ChannelRouteKindField,
            ZLinkTraceFormat.ChannelRouteKind(flow.Surface, flow.ChannelRouteKind)
        );
        activity.SetTag(ZLinkTraceFormat.MeshNameField, flow.MeshName);
        activity.SetTag(ZLinkTraceFormat.TopicField, flow.Topic);
        activity.SetTag(ZLinkTraceFormat.SpotIdField, flow.SpotId);
        activity.SetTag(ZLinkTraceFormat.InstanceSpotTypeField, flow.InstanceSpotType);
        activity.SetTag(
            ZLinkTraceFormat.ActivationStateField,
            ZLinkTraceFormat.ActivationStateKey(flow.ActivationState)
        );
        activity.SetTag(ZLinkTraceFormat.ActorIdField, flow.ActorId);
        activity.SetTag(ZLinkTraceFormat.StreamSessionIdField, flow.StreamSessionId);
        activity.SetTag(ZLinkTraceFormat.SourceRidField, flow.SourceRid);
        activity.SetTag(ZLinkTraceFormat.TargetRidField, flow.TargetRid ?? flow.PeerRid);
        activity.SetTag(ZLinkTraceFormat.ServerRidField, flow.ServerRid);
        activity.SetTag(ZLinkTraceFormat.CorrelationIdField, flow.CorrelationId);
        activity.SetTag(ZLinkTraceFormat.FlowIdField, ZLinkTraceFormat.FlowIdKey(flow));
        activity.SetTag(ZLinkTraceFormat.FlowOriginField, ZLinkTraceFormat.FlowOriginKey(flow));
        activity.SetTag(
            ZLinkTraceFormat.ReasonField,
            ZLinkTraceFormat.MessageReasonKey(flow.Reason)
        );
        activity.SetTag(ZLinkTraceFormat.MessageSizeBytesField, flow.MessageSize);
        activity.SetTag(ZLinkTraceFormat.DurationSecondsField, flow.DurationSeconds);
        activity.AddEvent(new ActivityEvent(ZLinkTraceFormat.MessageFlowEventName));
    }

    public static void TraceDispatchError(
        ZLinkDispatchFailure error,
        ZLinkDispatchErrorDetails errorDetails,
        string? flowId,
        ZLinkFlowOrigin? flowOrigin
    )
    {
        if (!ActivitySource.HasListeners())
            return;

        using var activity = ActivitySource.StartActivity(
            ZLinkTraceFormat.DispatchErrorEventName,
            ActivityKind.Consumer
        );
        if (activity is null)
            return;

        activity.SetTag(ZLinkTraceFormat.EventIdField, ZLinkTraceFormat.DispatchErrorEventName);
        activity.SetTag(
            ZLinkTraceFormat.OutcomeField,
            ZLinkTraceFormat.ResultKey(ZLinkMessageFlowResult.Failed)
        );
        activity.SetTag(ZLinkTraceFormat.SurfaceField, ZLinkTraceFormat.SurfaceKey(error.Surface));
        activity.SetTag(
            ZLinkTraceFormat.MessageKindField,
            ZLinkTraceFormat.MessageKindKey(error.MessageKind)
        );
        activity.SetTag(
            ZLinkTraceFormat.ReasonField,
            ZLinkTraceFormat.DispatchReasonKey(error.Reason)
        );
        activity.SetTag(
            ZLinkTraceFormat.ActionField,
            ZLinkTraceFormat.DispatchActionKey(error.Action)
        );
        activity.SetTag(ZLinkTraceFormat.ErrorTypeField, errorDetails.Type);
        activity.SetTag(ZLinkTraceFormat.ErrorMessageField, errorDetails.Message);
        activity.SetTag(ZLinkTraceFormat.PacketNameField, error.PacketName);
        activity.SetTag(ZLinkTraceFormat.ChannelNameField, error.ChannelName);
        activity.SetTag(
            ZLinkTraceFormat.ChannelRouteKindField,
            ZLinkTraceFormat.ChannelRouteKind(error.Surface, error.ChannelRouteKind)
        );
        activity.SetTag(ZLinkTraceFormat.MeshNameField, error.MeshName);
        activity.SetTag(ZLinkTraceFormat.TopicField, error.Topic);
        activity.SetTag(ZLinkTraceFormat.SpotIdField, error.SpotId);
        activity.SetTag(ZLinkTraceFormat.InstanceSpotTypeField, error.InstanceSpotType);
        activity.SetTag(
            ZLinkTraceFormat.ActivationStateField,
            ZLinkTraceFormat.ActivationStateKey(error.ActivationState)
        );
        activity.SetTag(ZLinkTraceFormat.ActorIdField, error.ActorId);
        activity.SetTag(ZLinkTraceFormat.StreamSessionIdField, error.StreamSessionId);
        activity.SetTag(ZLinkTraceFormat.SourceRidField, error.SourceRid);
        activity.SetTag(ZLinkTraceFormat.TargetRidField, error.TargetRid);
        activity.SetTag(ZLinkTraceFormat.ServerRidField, error.ServerRid);
        activity.SetTag(ZLinkTraceFormat.CorrelationIdField, error.CorrelationId);
        if (!string.IsNullOrEmpty(flowId) && flowOrigin is not null)
        {
            activity.SetTag(ZLinkTraceFormat.FlowIdField, flowId);
            activity.SetTag(
                ZLinkTraceFormat.FlowOriginField,
                flowOrigin.Value.ToString().ToLowerInvariant()
            );
        }
        activity.AddEvent(new ActivityEvent(ZLinkTraceFormat.DispatchErrorEventName));
    }
}
