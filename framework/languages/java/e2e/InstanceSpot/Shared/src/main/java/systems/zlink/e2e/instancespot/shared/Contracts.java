package systems.zlink.e2e.instancespot.shared;

import java.util.List;

public final class Contracts {
    public static final String MESH = "instance-spot";
    public static final String HANDLER_GROUP = "instance-spot";
    public static final String STABLE_TYPE = "instance-echo";

    private Contracts() {
    }

    public record InstanceReq(
        String spotId,
        String operationId,
        String payload,
        long timeoutMilliseconds) {
        public InstanceReq {
            required(spotId, "spotId");
            required(operationId, "operationId");
            payload = payload == null ? "" : payload;
            if (timeoutMilliseconds <= 0) {
                timeoutMilliseconds = 5_000;
            }
        }
    }

    public record InstanceMsg(
        String spotId,
        String operationId,
        String payload) {
        public InstanceMsg {
            required(spotId, "spotId");
            required(operationId, "operationId");
            payload = payload == null ? "" : payload;
        }
    }

    public record CloseMsg(
        String spotId,
        String operationId,
        String gateId) {
        public CloseMsg {
            required(spotId, "spotId");
            required(operationId, "operationId");
            gateId = gateId == null ? "" : gateId;
        }
    }

    public record GateReq(
        String gateId,
        boolean open) {
        public GateReq {
            required(gateId, "gateId");
        }
    }

    public record InstanceRes(
        String spotId,
        String operationId,
        String payload,
        String ownerRid,
        String ownerLifecycle,
        long objectGeneration,
        long handlerSequence) {
    }

    public record InstanceCallRes(
        boolean succeeded,
        InstanceRes reply,
        String errorKind,
        String errorMessage) {
    }

    public record SendSubmitRes(
        boolean succeeded,
        String errorKind,
        String errorMessage) {
    }

    public record ConcurrentReq(
        String spotId,
        int count,
        String operationPrefix,
        long timeoutMilliseconds) {
        public ConcurrentReq {
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

    public record ConcurrentRes(
        List<InstanceCallRes> outcomes) {
        public ConcurrentRes {
            outcomes = List.copyOf(outcomes);
        }
    }

    public record EvidenceEntry(
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
        List<EvidenceEntry> events) {
        public EvidenceSnapshot {
            events = List.copyOf(events);
        }
    }

    public record EvidenceWaitReq(
        String kind,
        String operationId,
        long timeoutMilliseconds) {
        public EvidenceWaitReq {
            kind = kind == null ? "" : kind;
            operationId = operationId == null ? "" : operationId;
            if (timeoutMilliseconds <= 0) {
                timeoutMilliseconds = 5_000;
            }
        }
    }

    public record EvidenceWaitRes(
        boolean found,
        EvidenceSnapshot snapshot) {
    }

    public record LookupRes(
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
