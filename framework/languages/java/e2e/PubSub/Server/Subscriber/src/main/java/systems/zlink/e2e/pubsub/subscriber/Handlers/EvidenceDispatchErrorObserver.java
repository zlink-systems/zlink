package systems.zlink.e2e.pubsub.subscriber.Handlers;

import java.util.HashMap;
import java.util.Map;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import systems.zlink.e2e.pubsub.subscriber.Infrastructure.EvidenceStore;

public final class EvidenceDispatchErrorObserver extends Handler {
    private final EvidenceStore evidence;

    public EvidenceDispatchErrorObserver(EvidenceStore evidence) {
        this.evidence = evidence;
    }

    public void install() {
        Logger.getLogger("systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
            .addHandler(this);
    }

    @Override
    public void publish(LogRecord record) {
        Map<String, String> fields = diagnosticsFields(record.getMessage());
        if (fields == null || !"ERROR".equals(fields.get("outcome"))) {
            return;
        }
        evidence.record(
            "DispatchError",
            fields.get("topic"),
            "observer",
            -1,
            fields.get("reason") + "/" + fields.get("action") + "/" + fields.get("packet"));
    }

    @Override public void flush() { }
    @Override public void close() { }

    private static Map<String, String> diagnosticsFields(String message) {
        if (message == null || !message.startsWith("message flow ")) {
            return null;
        }
        Map<String, String> fields = new HashMap<>();
        for (String field : message.substring("message flow ".length()).split(" ")) {
            String[] pair = field.split("=", 2);
            if (pair.length == 2) {
                fields.put(pair[0], pair[1]);
            }
        }
        return fields;
    }
}
