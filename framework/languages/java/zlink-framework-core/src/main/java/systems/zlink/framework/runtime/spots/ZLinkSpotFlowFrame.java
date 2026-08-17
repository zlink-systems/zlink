package systems.zlink.framework.runtime.spots;

import java.nio.charset.StandardCharsets;
import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

final class ZLinkSpotFlowFrame {
    private static final String PREFIX = "__zlink.flow\n";
    //  A well-formed flow frame is prefix(13) + UUIDv7(36) + '\n' + origin
    //  name; larger route parts are payload frames and are never stringified.
    private static final int MAX_FRAME_BYTES = 96;

    private ZLinkSpotFlowFrame() { }

    /**
     * Encodes an explicitly passed flow state (R1 value-passing): outbound
     * entry points capture the state as a value and hand it to the encoder
     * instead of installing a scope, so the stages they return stay bare.
     */
    static Message encode(ZLinkFlowContext.State state) {
        return state == null ? null : Message.from((PREFIX + state.flowId() + "\n" + state.origin().name())
            .getBytes(StandardCharsets.UTF_8));
    }

    /**
     * Reads the inbound flow pair from a SPOT route envelope. The frame sits
     * behind the packet-name and payload parts, after the optional
     * content-type frame (spec 27 §5). A frame that carries the flow prefix
     * but not a valid UUIDv7 pair is a protocol error (spec 27 §3).
     */
    static ZLinkFlowContext.State decode(List<Message> parts) {
        //  The encoder places the flow frame at index 2, or at index 3 when a
        //  content-type frame precedes it (ZLinkSpotRouteMessages.encode).
        int limit = Math.min(parts.size(), 4);
        for (int index = 2; index < limit; index++) {
            Message part = parts.get(index);
            if (part.size() > MAX_FRAME_BYTES) {
                continue;
            }
            String value = part.toUtf8String();
            if (!value.startsWith(PREFIX)) {
                continue;
            }
            String[] fields = value.split("\n", -1);
            if (fields.length != 3 || fields[1].isBlank()) {
                throw invalidFlow("SPOT route flow fields are malformed", null);
            }
            if (!ZLinkFlowContext.isValidFlowId(fields[1])) {
                throw invalidFlow("SPOT route flow id must be UUIDv7", null);
            }
            try {
                return new ZLinkFlowContext.State(
                    fields[1], ZLinkFlowOrigin.valueOf(fields[2]));
            } catch (IllegalArgumentException invalidOrigin) {
                throw invalidFlow("SPOT route flow origin is invalid", invalidOrigin);
            }
        }
        return null;
    }

    private static ZLinkFrameworkException invalidFlow(
        String message,
        Throwable cause) {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message, cause);
    }
}
