package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

import java.util.List;

final class ZLinkSpotFlowFrame {
    private static final int MAX_SEARCHED_HEADER_PARTS = 4;
    //  A well-formed flow frame is prefix(13) + UUIDv7(36) + '\n' + origin
    //  name; larger route parts are payload frames and are never stringified.
    private static final int MAX_FRAME_BYTES = 96;

    private ZLinkSpotFlowFrame() {}

    /**
     * Encodes an explicitly passed flow state (R1 value-passing): outbound entry points capture the
     * state as a value and hand it to the encoder instead of installing a scope, so the stages they
     * return stay bare.
     */
    static Message encode(ZLinkFlowContext.State state) {
        return ZLinkFlowContext.encodeLegacyFrame(state);
    }

    static ZLinkFlowContext.State fromEnvelopeHeader(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header header) {
        return header.flowId() == null
                ? null
                : new ZLinkFlowContext.State(header.flowId(), header.flowOrigin(), null);
    }

    /**
     * Reads the inbound flow pair from a SPOT route message. A shared cross-language envelope
     * carries the pair in its JSON header (spec 27 §4); legacy internal raw-parts packets may still
     * carry the standalone flow frame behind the packet-name and payload parts. A malformed
     * envelope header or a frame with the flow prefix but not a valid UUIDv7 pair is a protocol
     * error (spec 27 §3).
     */
    static ZLinkFlowContext.State decode(List<Message> parts) {
        if (systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.looksLikeEnvelope(
                parts)) {
            var header =
                    systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader(
                            parts.get(0), true);
            return header.flowId() == null
                    ? null
                    : new ZLinkFlowContext.State(header.flowId(), header.flowOrigin(), null);
        }
        //  The legacy encoder placed the flow frame at index 2, or at index 3
        //  when a content-type frame preceded it.
        int limit = Math.min(parts.size(), MAX_SEARCHED_HEADER_PARTS);
        for (int index = 2; index < limit; index++) {
            Message part = parts.get(index);
            if (part.size() > MAX_FRAME_BYTES) {
                continue;
            }
            String value = part.toUtf8String();
            ZLinkFlowContext.State state =
                    ZLinkFlowContext.decodeLegacyFrame(
                            value, "SPOT route", ZLinkSpotFlowFrame::invalidFlow);
            if (state != null) {
                return state;
            }
        }
        return null;
    }

    private static ZLinkFrameworkException invalidFlow(String message, Throwable cause) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message, cause);
    }
}
