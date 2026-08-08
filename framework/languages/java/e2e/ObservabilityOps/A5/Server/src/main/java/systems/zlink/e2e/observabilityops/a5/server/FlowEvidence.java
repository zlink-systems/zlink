package systems.zlink.e2e.observabilityops.a5.server;

import java.util.List;
import java.util.Map;
import java.util.HashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

public final class FlowEvidence extends Handler {
    private static final String LOGGER_NAME =
        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer";
    private final CopyOnWriteArrayList<FlowEvent> events =
        new CopyOnWriteArrayList<>();

    public void install() {
        Logger.getLogger(LOGGER_NAME).addHandler(this);
    }

    @Override
    public void publish(LogRecord record) {
        FlowEvent event = FlowEvent.parse(record.getMessage());
        if (event != null) {
            events.add(event);
        }
    }

    @Override public void flush() {
    }

    @Override public void close() {
        Logger.getLogger(LOGGER_NAME).removeHandler(this);
    }

    public List<FlowEvent> snapshot() {
        return List.copyOf(events);
    }

    public record FlowEvent(
        String outcome,
        String surface,
        String messageKind,
        String packetName,
        String channelName,
        String errorReason,
        String errorType) {
        static FlowEvent parse(String message) {
            if (message == null || !message.startsWith("message flow ")) {
                return null;
            }
            Map<String, String> fields = new HashMap<>();
            for (String token : message.split(" ")) {
                int separator = token.indexOf('=');
                if (separator > 0) {
                    fields.put(token.substring(0, separator), token.substring(separator + 1));
                }
            }
            return new FlowEvent(
                fields.get("outcome"),
                fields.get("surface"),
                fields.get("kind"),
                fields.get("packet"),
                fields.get("channel"),
                fields.get("reason"),
                fields.get("errorType"));
        }
    }
}
