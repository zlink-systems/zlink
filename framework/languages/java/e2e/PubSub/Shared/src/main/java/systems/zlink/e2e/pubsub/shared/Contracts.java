package systems.zlink.e2e.pubsub.shared;

import java.util.List;

public final class Contracts {
    public static final String EVENT_CHANNEL = "pubsub.events";
    public static final String HANDLER_GROUP = "pubsub";
    public static final String EVENT_PACKET = "EventMsg";
    public static final String MISSING_PACKET = "MissingEventMsg";

    private Contracts() {
    }

    public record EventMsg(
        String scenario,
        int sequence,
        String value) {
    }

    public record MissingEventMsg(
        String scenario,
        int sequence,
        String value) {
    }

    public record EvidenceEntry(
        String marker,
        String subscriberRid,
        String topic,
        String scenario,
        int sequence,
        String value) {
    }

    public record EvidenceSnapshot(
        String subscriberRid,
        List<EvidenceEntry> entries) {
    }
}
