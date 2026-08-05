package systems.zlink.framework.runtime.diagnostics;

import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;

// Shared formatter for the file/JUL key=value lines (present fields only) used by both
// the flow tracer and the dispatch error reporter. Mirrors the C++/.NET trace format.
final class ZLinkTraceFormat {
    private ZLinkTraceFormat() {
    }

    static String flowLine(ZLinkMessageFlowEvent flow, String nodeId, Long size) {
        StringBuilder builder = new StringBuilder("message flow");
        append(builder, "outcome", String.valueOf(flow.outcome()));
        append(builder, "surface", String.valueOf(flow.surface()));
        append(builder, "kind", String.valueOf(flow.messageKind()));
        append(builder, "label", nodeId);
        append(builder, "reason", enumName(flow.errorReason()));
        append(builder, "action", enumName(flow.errorAction()));
        append(builder, "packet", flow.packetName());
        append(builder, "channel", flow.channelName());
        append(builder, "topic", flow.topic());
        append(builder, "corr", flow.correlationId());
        append(builder, "flow", flow.flowId());
        append(builder, "origin", enumName(flow.flowOrigin()));
        append(builder, "src", flow.sourceRid());
        append(builder, "spot", flow.spotId());
        append(builder, "actor", flow.actorId());
        append(builder, "errorType", flow.errorType());
        append(builder, "errorMessage", flow.errorMessage());
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

    private static String enumName(Enum<?> value) {
        return value == null ? null : String.valueOf(value);
    }
}
