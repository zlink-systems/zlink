package systems.zlink.framework.runtime.channels;

import java.nio.charset.StandardCharsets;
import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

final class ZLinkChannelFlowFrame {
    private static final String PREFIX = "__zlink.flow\n";

    private ZLinkChannelFlowFrame() { }

    static Message current() {
        ZLinkFlowContext.State state = ZLinkFlowContext.current();
        return state == null ? null : Message.from((PREFIX + state.flowId() + "\n" + state.origin().name())
            .getBytes(StandardCharsets.UTF_8));
    }

    static ZLinkFlowContext.State decode(List<Message> parts) {
        for (int index = 2; index < parts.size(); index++) {
            String value = parts.get(index).toUtf8String();
            if (!value.startsWith(PREFIX)) {
                continue;
            }
            String[] fields = value.split("\n", -1);
            if (fields.length != 3 || fields[1].isBlank()) return null;
            return new ZLinkFlowContext.State(fields[1], ZLinkFlowOrigin.valueOf(fields[2]));
        }
        return null;
    }
}
