package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

public final class ZLinkSpotFlowFrame {
    private ZLinkSpotFlowFrame() {}

    public static ZLinkFlowContext.State fromEnvelopeHeader(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header header) {
        return header.flowId() == null
                ? null
                : new ZLinkFlowContext.State(header.flowId(), header.flowOrigin(), null);
    }
}
