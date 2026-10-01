package systems.zlink.framework.runtime.channels;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

import java.util.List;

final class ZLinkChannelFlowFrame {

    private ZLinkChannelFlowFrame() {}

    static Message current() {
        ZLinkFlowContext.State state = ZLinkFlowContext.current();
        return ZLinkFlowContext.encodeLegacyFrame(state);
    }

    static ZLinkFlowContext.State fromEnvelopeHeader(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header header) {
        return header.flowId() == null
                ? null
                : new ZLinkFlowContext.State(header.flowId(), header.flowOrigin(), null);
    }

    static ZLinkFlowContext.State decode(List<Message> parts) {
        if (systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.looksLikeEnvelope(
                parts)) {
            try {
                var header =
                        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeHeader(
                                parts.get(0), true);
                return header.flowId() == null
                        ? null
                        : new ZLinkFlowContext.State(header.flowId(), header.flowOrigin(), null);
            } catch (ZLinkFrameworkException invalidEnvelope) {
                throw new PayloadDecodeDispatchException(
                        invalidEnvelope.getMessage(), invalidEnvelope);
            }
        }
        for (int index = 2; index < parts.size(); index++) {
            String value = parts.get(index).toUtf8String();
            ZLinkFlowContext.State state =
                    ZLinkFlowContext.decodeLegacyFrame(
                            value, "Channel", ZLinkChannelFlowFrame::invalidFlow);
            if (state != null) {
                return state;
            }
        }
        return null;
    }

    private static PayloadDecodeDispatchException invalidFlow(String message, Throwable cause) {
        return new PayloadDecodeDispatchException(
                message,
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message, cause));
    }
}
