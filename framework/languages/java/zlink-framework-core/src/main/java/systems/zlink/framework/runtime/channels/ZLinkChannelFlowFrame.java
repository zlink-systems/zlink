package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

final class ZLinkChannelFlowFrame {

    private ZLinkChannelFlowFrame() {}

    static ZLinkFlowContext.State fromEnvelopeHeader(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header header) {
        return header.flowId() == null
                ? null
                : new ZLinkFlowContext.State(header.flowId(), header.flowOrigin(), null);
    }
}
