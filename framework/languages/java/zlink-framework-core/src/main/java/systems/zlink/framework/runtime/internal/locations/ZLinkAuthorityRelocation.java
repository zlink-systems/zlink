package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.JsonNodeFactory;
import com.fasterxml.jackson.databind.node.ObjectNode;

import systems.zlink.contracts.core.RoutingId;

import java.util.List;
import java.util.UUID;

/** Immutable Location runtime §3.4 relocation value; extensions survive authority CAS. */
public final class ZLinkAuthorityRelocation {
    private static final List<String> PHASES =
            List.of(
                    "preparing",
                    "captured",
                    "prepared",
                    "committed",
                    "activating",
                    "activated",
                    "cleaning",
                    "completed",
                    "aborted");
    private final ObjectNode value;

    public ZLinkAuthorityRelocation(JsonNode value) {
        object(value);
        uuid(value.get("relocationId"));
        String phase = text(value.get("phase"));
        if (!PHASES.contains(phase)) throw invalid();
        fence(value.get("source"));
        JsonNode target = value.get("target");
        if (target == null) throw invalid();
        boolean beforeTarget = phase.equals("preparing") || phase.equals("captured");
        if (target.isNull()) {
            if (!beforeTarget && !phase.equals("aborted")) throw invalid();
        } else {
            if (beforeTarget) throw invalid();
            fence(target);
            integer(target.get("attemptGeneration"));
        }
        fence(value.get("coordinator"));
        text(value.get("coordinator").get("expectedStoreVersion"));
        sha(value.get("sourceInventorySha256"));
        integer(value.get("applicationVersion"));
        if (value.has("completion")) {
            JsonNode completion = value.get("completion");
            object(completion);
            long accepted = integer(completion.get("acceptedRequests"));
            long completed = integer(completion.get("completedResults"));
            long pending = integer(completion.get("pendingDeliveries"));
            if (Long.compareUnsigned(completed, accepted) > 0
                    || Long.compareUnsigned(pending, completed) > 0) throw invalid();
            JsonNode reference = completion.get("reference");
            JsonNode checksum = completion.get("sha256");
            if (reference == null || checksum == null || reference.isNull() != checksum.isNull())
                throw invalid();
            if (!reference.isNull()) {
                text(reference);
                sha(checksum);
            }
        }
        this.value = ((ObjectNode) value).deepCopy();
    }

    public ObjectNode json() {
        return value.deepCopy();
    }

    public UUID relocationId() {
        return UUID.fromString(value.get("relocationId").textValue());
    }

    public int phaseOrdinal() {
        return PHASES.indexOf(phase()) + 1;
    }

    /**
     * Ordered progress check. {@code aborted} is an unordered terminal: an aborted relocation has
     * reached only {@code aborted}, and no ordered phase reaches {@code aborted}.
     */
    public boolean hasReached(String phase) {
        if (!PHASES.contains(phase)) throw new IllegalArgumentException("Unknown relocation phase");
        if (aborted() || phase.equals("aborted")) return aborted() && phase.equals("aborted");
        return phaseOrdinal() >= PHASES.indexOf(phase) + 1;
    }

    public boolean aborted() {
        return phase().equals("aborted");
    }

    public String phase() {
        return value.get("phase").textValue();
    }

    public JsonNode source() {
        return value.get("source").deepCopy();
    }

    public JsonNode target() {
        return value.get("target").deepCopy();
    }

    public JsonNode coordinator() {
        return value.get("coordinator").deepCopy();
    }

    public long applicationVersion() {
        return integer(value.get("applicationVersion"));
    }

    public ZLinkAuthorityRelocation phase(String phase) {
        ObjectNode next = json();
        next.put("phase", phase);
        return new ZLinkAuthorityRelocation(next);
    }

    public String coordinatorExpectedStoreVersion() {
        return value.get("coordinator").get("expectedStoreVersion").textValue();
    }

    public static ZLinkCanonicalRelocationAuthorityStateCodec.Published publication(
            ZLinkAuthoritySnapshot snapshot, long requestAggregateGeneration) {
        ZLinkAuthorityRelocation relocation = snapshot.relocation();
        if (relocation == null)
            return ZLinkCanonicalRelocationAuthorityStateCodec.decode(snapshot.payload());
        var source = relocation.source();
        var target = relocation.target();
        var coordinator = relocation.coordinator();
        long generation =
                snapshot.aggregateGeneration() == 0
                        ? requestAggregateGeneration
                        : snapshot.aggregateGeneration();
        return new ZLinkCanonicalRelocationAuthorityStateCodec.Published(
                relocation.relocationId(),
                generation,
                target.isNull()
                        ? 0
                        : ZLinkAuthorityRelocation.integer(target.get("attemptGeneration")),
                null,
                0,
                source.get("ownerId").asText(),
                ZLinkAuthorityRelocation.integer(source.get("ownerLeaseGeneration")),
                RoutingId.fromHex(source.get("nodeRid").asText()),
                ZLinkAuthorityRelocation.integer(source.get("nodeGeneration")),
                target.isNull()
                        ? RoutingId.from(new byte[0])
                        : RoutingId.fromHex(target.get("nodeRid").asText()),
                target.isNull()
                        ? 0
                        : ZLinkAuthorityRelocation.integer(target.get("nodeGeneration")),
                target.isNull() ? "" : target.get("ownerId").asText(),
                target.isNull()
                        ? 0
                        : ZLinkAuthorityRelocation.integer(target.get("ownerLeaseGeneration")),
                coordinator.get("ownerId").asText(),
                ZLinkAuthorityRelocation.integer(coordinator.get("ownerLeaseGeneration")),
                RoutingId.fromHex(coordinator.get("nodeRid").asText()),
                ZLinkAuthorityRelocation.integer(coordinator.get("nodeGeneration")),
                relocation.coordinatorExpectedStoreVersion(),
                relocation.phaseOrdinal(),
                relocation.applicationVersion(),
                0,
                snapshot.payload());
    }

    static ZLinkAuthorityRelocation preparing(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkAuthoritySnapshot primary,
            UUID id,
            String digest,
            long applicationVersion) {
        ObjectNode value = JsonNodeFactory.instance.objectNode();
        value.put("relocationId", id.toString());
        value.put("phase", "preparing");
        value.set("source", fence(snapshot));
        value.putNull("target");
        ObjectNode coordinator = fence(primary);
        coordinator.put("expectedStoreVersion", primary.storeVersion());
        value.set("coordinator", coordinator);
        value.put("sourceInventorySha256", digest);
        value.put("applicationVersion", Long.toUnsignedString(applicationVersion));
        return new ZLinkAuthorityRelocation(value);
    }

    static ZLinkAuthorityRelocation prepared(
            ZLinkAuthoritySnapshot snapshot,
            ZLinkAggregateRelocationCoordinator.Request request,
            byte[] digest) {
        var existing = snapshot.relocation();
        var root = ZLinkServiceRelocationEnvelopeCodec.decode(request.root());
        if (root.relocationHigh() != request.aggregateId().getMostSignificantBits()
                || root.relocationLow() != request.aggregateId().getLeastSignificantBits())
            throw new IllegalArgumentException("Relocation root identity differs");
        var legacy =
                existing == null
                        ? ZLinkCanonicalRelocationAuthorityStateCodec.decode(snapshot.payload())
                        : null;
        if (legacy != null
                && (!legacy.aggregateId().equals(request.aggregateId())
                        || (legacy.phase() != 2
                                && (!legacy.targetNodeRid().equals(request.targetDescriptor().rid())
                                        || legacy.targetNodeGeneration()
                                                != request.targetDescriptorLifecycleGeneration()
                                        || !legacy.targetOwnerId()
                                                .equals(request.targetOwner().ownerId())
                                        || legacy.targetOwnerLeaseGeneration()
                                                != request.targetOwner().leaseGeneration()))))
            throw new IllegalArgumentException("Legacy relocation successor fence differs");
        var value =
                existing == null
                        ? com.fasterxml.jackson.databind.node.JsonNodeFactory.instance.objectNode()
                        : existing.json();
        if (existing != null && !existing.relocationId().equals(request.aggregateId()))
            throw new IllegalArgumentException("Relocation identity differs");
        value.put("relocationId", request.aggregateId().toString());
        value.put("phase", "prepared");
        if (existing == null) {
            var source = fence(snapshot);
            var coordinator = fence(snapshot);
            if (legacy != null) {
                source.put("nodeRid", legacy.sourceNodeRid().toHex());
                source.put("nodeGeneration", Long.toUnsignedString(legacy.sourceNodeGeneration()));
                source.put("ownerId", legacy.sourceOwnerId());
                source.put(
                        "ownerLeaseGeneration",
                        Long.toUnsignedString(legacy.sourceOwnerLeaseGeneration()));
                coordinator.put("nodeRid", legacy.coordinatorNodeRid().toHex());
                coordinator.put(
                        "nodeGeneration",
                        Long.toUnsignedString(legacy.coordinatorNodeGeneration()));
                coordinator.put("ownerId", legacy.coordinatorOwnerId());
                coordinator.put(
                        "ownerLeaseGeneration",
                        Long.toUnsignedString(legacy.coordinatorLeaseGeneration()));
            }
            value.set("source", source);
            coordinator.put(
                    "expectedStoreVersion",
                    legacy == null
                            ? request.coordinatorExpectedStoreVersion()
                            : legacy.coordinatorExpectedStoreVersion());
            value.set("coordinator", coordinator);
            value.put("sourceInventorySha256", java.util.HexFormat.of().formatHex(digest));
            value.put("applicationVersion", Long.toUnsignedString(root.applicationVersion()));
        }
        var target = com.fasterxml.jackson.databind.node.JsonNodeFactory.instance.objectNode();
        target.put("nodeRid", request.targetDescriptor().rid().toHex());
        target.put(
                "nodeGeneration",
                Long.toUnsignedString(request.targetDescriptorLifecycleGeneration()));
        target.put("ownerId", request.targetOwner().ownerId());
        target.put(
                "ownerLeaseGeneration",
                Long.toUnsignedString(request.targetOwner().leaseGeneration()));
        target.put("attemptGeneration", Long.toUnsignedString(request.targetAttemptGeneration()));
        if (existing != null && !existing.target().isNull() && !existing.target().equals(target))
            throw new IllegalArgumentException("Relocation successor target fence differs");
        value.set("target", target);
        return new ZLinkAuthorityRelocation(value);
    }

    static ObjectNode fence(ZLinkAuthoritySnapshot snapshot) {
        ObjectNode result = JsonNodeFactory.instance.objectNode();
        result.put("nodeRid", snapshot.allocation().descriptor().rid().toHex());
        result.put(
                "nodeGeneration",
                Long.toUnsignedString(snapshot.allocation().descriptorLifecycleGeneration()));
        result.put("ownerId", snapshot.ownerId());
        result.put("ownerLeaseGeneration", Long.toUnsignedString(snapshot.ownerLeaseGeneration()));
        return result;
    }

    static void object(JsonNode value) {
        if (value == null || !value.isObject()) throw invalid();
    }

    static String text(JsonNode value) {
        if (value == null || !value.isTextual() || value.textValue().isEmpty()) throw invalid();
        return value.textValue();
    }

    static long integer(JsonNode value) {
        String text = text(value);
        if (!text.matches("0|[1-9][0-9]*")) throw invalid();
        try {
            return Long.parseUnsignedLong(text);
        } catch (NumberFormatException invalid) {
            throw invalid();
        }
    }

    static void uuid(JsonNode value) {
        if (!text(value).matches("[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}")) throw invalid();
    }

    static void sha(JsonNode value) {
        if (!text(value).matches("[0-9a-f]{64}")) throw invalid();
    }

    private static void fence(JsonNode value) {
        object(value);
        if (!text(value.get("nodeRid")).matches("(?:[0-9a-f]{2})+")) throw invalid();
        integer(value.get("nodeGeneration"));
        text(value.get("ownerId"));
        integer(value.get("ownerLeaseGeneration"));
    }

    private static IllegalArgumentException invalid() {
        return new IllegalArgumentException("Invalid authority aggregate relocation");
    }

    @Override
    public boolean equals(Object other) {
        return other instanceof ZLinkAuthorityRelocation relocation
                && value.equals(relocation.value);
    }

    @Override
    public int hashCode() {
        return value.hashCode();
    }
}
