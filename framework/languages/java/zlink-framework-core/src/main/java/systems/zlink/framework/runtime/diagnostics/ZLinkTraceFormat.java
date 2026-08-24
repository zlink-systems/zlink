package systems.zlink.framework.runtime.diagnostics;

import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;

/** Spec 26 structured-log projection. */
final class ZLinkTraceFormat {
    private ZLinkTraceFormat() {
    }

    static String flowLine(ZLinkMessageFlowEvent flow, Long size) {
        StringBuilder builder = new StringBuilder("zlink flow:");
        append(builder, "event_id", flow.eventId().traceName());
        append(builder, "phase", token(flow.phase()));
        append(builder, "surface", flow.surface().traceName());
        append(builder, "kind", flow.messageKind().traceName());
        append(builder, "mesh", flow.meshName());
        append(builder, "channel", flow.channelName());
        append(builder, "channel_route", token(flow.channelRouteKind()));
        append(builder, "source_rid", flow.sourceRid());
        append(builder, "target_rid", flow.targetRid());
        append(builder, "server_rid", flow.serverRid());
        append(builder, "packet", flow.packetName());
        append(builder, "topic", flow.topic());
        append(builder, "spot", flow.spotId());
        append(builder, "instance_type", flow.instanceSpotType());
        append(builder, "activation_state", token(flow.activationState()));
        append(builder, "actor", flow.actorId());
        append(builder, "corr", flow.correlationId());
        append(builder, "flow", flow.flowId());
        append(builder, "origin", token(flow.flowOrigin()));
        append(builder, "outcome", flow.outcome().traceName());
        append(builder, "reason", flow.errorReason() == null
            ? null : flow.errorReason().traceName());
        append(builder, "action", flow.errorAction() == null
            ? null : flow.errorAction().traceName());
        if (size != null) {
            append(builder, "size", String.valueOf(size.longValue()));
        }
        return builder.toString();
    }

    private static void append(StringBuilder builder, String key, String value) {
        if (value != null && !value.isEmpty()) {
            builder.append(' ').append(key).append('=').append(value);
        }
    }

    private static String token(Enum<?> value) {
        return value == null ? null : value.name().toLowerCase(java.util.Locale.ROOT);
    }
}
