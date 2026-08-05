package systems.zlink.framework.runtime.spots;

import java.nio.charset.StandardCharsets;
import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

final class ZLinkSpotFlowFrame {
    private static final String PREFIX = "__zlink.flow\n";

    private ZLinkSpotFlowFrame() { }

    static Message current() {
        ZLinkFlowContext.State state = ZLinkFlowContext.current();
        return state == null ? null : Message.from((PREFIX + state.flowId() + "\n" + state.origin().name())
            .getBytes(StandardCharsets.UTF_8));
    }

    static ZLinkFlowContext.State decode(List<Message> parts) {
        if (parts.size() < 3) return null;
        String[] fields = parts.get(2).toUtf8String().split("\n", -1);
        if (fields.length != 3 || !"__zlink.flow".equals(fields[0]) || fields[1].isBlank()) return null;
        return new ZLinkFlowContext.State(fields[1], ZLinkFlowOrigin.valueOf(fields[2]));
    }
}
