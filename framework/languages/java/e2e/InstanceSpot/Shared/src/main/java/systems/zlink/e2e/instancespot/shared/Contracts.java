package systems.zlink.e2e.instancespot.shared;

import java.util.List;

public final class Contracts {
    public static final String MESH = "instance-spot";
    public static final String HANDLER_GROUP = "instance-spot";
    public static final String STABLE_TYPE = "instance-echo";

    private Contracts() {
    }

    public record InstanceRequest(
        String spotId,
        String operationId,
        String payload,
        long timeoutMilliseconds) {
        public InstanceRequest {
            required(spotId, "spotId");
            required(operationId, "operationId");
            payload = payload == null ? "" : payload;
            if (timeoutMilliseconds <= 0) {
                timeoutMilliseconds = 5_000;
            }
        }
    }

    public record InstanceSend(
        String spotId,
        String operationId,
        String payload) {
        public InstanceSend {
            required(spotId, "spotId");
            required(operationId, "operationId");
            payload = payload == null ? "" : payload;
        }
    }

    public record CloseRequest(
        String spotId,
        String operationId,
        String gateId) {
        public CloseRequest {
            required(spotId, "spotId");
            required(operationId, "operationId");
            gateId = gateId == null ? "" : gateId;
        }
    }

    public record GateRequest(
        String gateId,
        boolean open) {
        public GateRequest {
            required(gateId, "gateId");
        }
    }

    public record InstanceReply(
        String spotId,
        String operationId,
        String payload,
        String ownerRid,
        String ownerLifecycle,
        long objectGeneration,
        long handlerSequence) {
    }

    public record RequestOutcome(
        boolean succeeded,
        InstanceReply reply,
        String errorKind,
        String errorMessage) {
    }

    public record SendOutcome(
        boolean succeeded,
        String errorKind,
        String errorMessage) {
    }

    public record ConcurrentRequest(
        String spotId,
        int count,
        String operationPrefix,
        long timeoutMilliseconds) {
        public ConcurrentRequest {
            required(spotId, "spotId");
            if (count <= 0 || count > 128) {
                throw new IllegalArgumentException("count must be between 1 and 128");
            }
            required(operationPrefix, "operationPrefix");
            if (timeoutMilliseconds <= 0) {
                timeoutMilliseconds = 5_000;
            }
        }
    }

    public record ConcurrentOutcome(
        List<RequestOutcome> outcomes) {
        public ConcurrentOutcome {
            outcomes = List.copyOf(outcomes);
        }
    }

    public record EvidenceEvent(
        long sequence,
        String kind,
        String spotId,
        String operationId,
        String payload,
        String ownerRid,
        String lifecycleId,
        long objectGeneration,
        int activeHandlers,
        String detail) {
    }

    public record EvidenceSnapshot(
        String rid,
        String lifecycleId,
        List<EvidenceEvent> events) {
        public EvidenceSnapshot {
            events = List.copyOf(events);
        }
    }

    public record EvidenceWaitRequest(
        String kind,
        String operationId,
        long timeoutMilliseconds) {
        public EvidenceWaitRequest {
            kind = kind == null ? "" : kind;
            operationId = operationId == null ? "" : operationId;
            if (timeoutMilliseconds <= 0) {
                timeoutMilliseconds = 5_000;
            }
        }
    }

    public record EvidenceWaitResult(
        boolean found,
        EvidenceSnapshot snapshot) {
    }

    public record LookupOutcome(
        boolean found,
        String spotId,
        long objectGeneration,
        String meshName,
        String nodeRid,
        String errorKind,
        String errorMessage) {
    }

    private static void required(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required");
        }
    }
}
