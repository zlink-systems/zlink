package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.time.format.DateTimeFormatterBuilder;
import java.time.temporal.ChronoField;
import java.util.ArrayList;
import java.util.Base64;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

public final class ZLinkLocationDescriptorCodec {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final DateTimeFormatter INSTANT_WIRE_FORMAT =
        new DateTimeFormatterBuilder()
            .appendPattern("yyyy-MM-dd'T'HH:mm:ss")
            .appendFraction(ChronoField.NANO_OF_SECOND, 0, 9, true)
            .appendOffset("+HH:MM", "+00:00")
            .toFormatter()
            .withZone(ZoneOffset.UTC);

    private ZLinkLocationDescriptorCodec() {
    }

    public static String serializeMeshNode(ZLinkMeshNodeDescriptor row) {
        ObjectNode node = JSON.createObjectNode();
        node.put("MeshName", row.meshName());
        putRid(node, "Rid", row.rid());
        node.put("LifecycleGeneration", row.lifecycleGeneration());
        node.put("DescriptorRevision", row.descriptorRevision());
        node.put("Endpoint", row.endpoint());
        ObjectNode channels = JSON.createObjectNode();
        row.channelWeights().entrySet().stream()
            .sorted(Map.Entry.comparingByKey())
            .forEach(entry -> channels.put(
                entry.getKey(),
                entry.getValue()));
        node.set("ChannelWeights", channels);
        node.put("SecurityIdentity", row.securityIdentity());
        node.put("OwnerId", row.ownerId());
        node.put("LeaseGeneration", row.leaseGeneration());
        putInstant(node, "UpdatedAt", row.updatedAt());
        node.put("ApplicationVersion", row.applicationVersion());
        ArrayNode capabilities = JSON.createArrayNode();
        row.objectCapabilities().stream()
            .sorted(java.util.Comparator
                .<ZLinkObjectCapability>comparingInt(
                    capability -> capability.objectKind().value())
                .thenComparing(ZLinkObjectCapability::stableType))
            .forEach(capability -> {
                ObjectNode encoded = JSON.createObjectNode();
                encoded.put(
                    "ObjectKind",
                    capability.objectKind().value());
                encoded.put("StableType", capability.stableType());
                encoded.put("Policy", capability.policy().value());
                encoded.put(
                    "HasSnapshotAdapter",
                    capability.hasSnapshotAdapter());
                encoded.put("SpotLimit", capability.spotLimit());
                capabilities.add(encoded);
            });
        node.set("ObjectCapabilities", capabilities);
        if (row.maintenanceWave().isPresent()) {
            node.put(
                "MaintenanceWave",
                row.maintenanceWave().orElseThrow());
        } else {
            node.putNull("MaintenanceWave");
        }
        node.put("State", row.state().wireValue());
        node.put("ObjectRole", row.objectRole().value());
        if (row.entrySpotId().isPresent()) {
            node.put("EntrySpotId", row.entrySpotId().orElseThrow());
        } else {
            node.putNull("EntrySpotId");
        }
        node.put("PlacementWeight", row.placementWeight());
        ObjectNode capacity = JSON.createObjectNode();
        capacity.set("Actors", serializeCapacityUsage(
            row.capacity().actors()));
        capacity.set("Spots", serializeCapacityUsage(
            row.capacity().spots()));
        ArrayNode spotTypes = JSON.createArrayNode();
        row.capacity().spotTypes().stream()
            .sorted(java.util.Comparator
                .<ZLinkSpotTypeCapacity>comparingInt(
                    value -> value.objectKind().value())
                .thenComparing(ZLinkSpotTypeCapacity::stableType))
            .forEach(value -> {
                ObjectNode encoded = JSON.createObjectNode();
                encoded.put("ObjectKind", value.objectKind().value());
                encoded.put("StableType", value.stableType());
                encoded.set("Usage", serializeCapacityUsage(
                    value.usage()));
                spotTypes.add(encoded);
            });
        capacity.set("SpotTypes", spotTypes);
        node.set("Capacity", capacity);
        ObjectNode activation = JSON.createObjectNode();
        activation.put(
            "Active",
            row.activationConcurrency().active());
        activation.put(
            "Limit",
            row.activationConcurrency().limit());
        node.set("ActivationConcurrency", activation);
        return write(node);
    }

    private static ObjectNode serializeCapacityUsage(
        ZLinkCapacityUsage usage) {
        ObjectNode encoded = JSON.createObjectNode();
        encoded.put("Active", usage.active());
        encoded.put("Reserved", usage.reserved());
        encoded.put("Limit", usage.limit());
        return encoded;
    }

    public static String serializeClientServer(
        ZLinkClientServerServerDescriptor row) {
        ObjectNode node = JSON.createObjectNode();
        node.put("ChannelName", row.channelName());
        putRid(node, "ServerRid", row.serverRid());
        node.put("LifecycleGeneration", row.lifecycleGeneration());
        node.put("DescriptorRevision", row.descriptorRevision());
        node.put("Endpoint", row.endpoint());
        node.put("Weight", row.weight());
        node.put("State", runtimeStateName(row.state()));
        node.put("SecurityIdentity", row.securityIdentity());
        node.put("OwnerId", row.ownerId());
        node.put("OwnerLeaseGeneration", row.leaseGeneration());
        putInstant(node, "UpdatedAt", row.updatedAt());
        return write(node);
    }

    public static ZLinkClientServerServerDescriptor deserializeClientServer(
        String json,
        long storeGeneration,
        Instant updatedAt) {
        JsonNode node = read(json);
        long encodedLifecycle =
            node.path("LifecycleGeneration").asLong();
        return new ZLinkClientServerServerDescriptor(
            text(node, "ChannelName"),
            rid(node, "ServerRid"),
            encodedLifecycle,
            node.path("DescriptorRevision").asLong(),
            text(node, "Endpoint"),
            node.path("Weight").asInt(),
            runtimeState(text(node, "State")),
            text(node, "SecurityIdentity"),
            text(node, "OwnerId"),
            node.path("OwnerLeaseGeneration").asLong(),
            updatedAt);
    }

    public static String clientServerImmutableFingerprint(
        ZLinkClientServerServerDescriptor row) {
        StringBuilder preimage = new StringBuilder();
        appendImmutableSegment(
            preimage,
            "zlink-client-server-immutable-v1");
        appendImmutableSegment(preimage, row.channelName());
        appendImmutableSegment(
            preimage,
            row.serverRid().toHex().toLowerCase(
                java.util.Locale.ROOT));
        appendImmutableSegment(
            preimage,
            Long.toString(row.lifecycleGeneration()));
        appendImmutableSegment(preimage, row.endpoint());
        appendImmutableSegment(preimage, row.securityIdentity());
        try {
            return HexFormat.of().formatHex(
                MessageDigest.getInstance("SHA-256").digest(
                    preimage.toString().getBytes(
                        StandardCharsets.UTF_8)));
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(
                "SHA-256 is unavailable",
                impossible);
        }
    }

    public static String serializeFanoutPublisher(
        ZLinkFanoutPublisherDescriptor row) {
        ObjectNode node = JSON.createObjectNode();
        node.put("ChannelName", row.channelName());
        putRid(node, "PublisherRid", row.publisherRid());
        node.put("LifecycleGeneration", row.lifecycleGeneration());
        node.put("DescriptorRevision", row.descriptorRevision());
        node.put("Endpoint", row.endpoint());
        node.put("State", runtimeStateName(row.state()));
        node.put("SecurityIdentity", row.securityIdentity());
        node.put("OwnerId", row.ownerId());
        node.put("OwnerLeaseGeneration", row.leaseGeneration());
        putInstant(node, "UpdatedAt", row.updatedAt());
        return write(node);
    }

    public static ZLinkFanoutPublisherDescriptor deserializeFanoutPublisher(
        String json,
        long storeGeneration,
        Instant updatedAt) {
        JsonNode node = read(json);
        return new ZLinkFanoutPublisherDescriptor(
            text(node, "ChannelName"),
            rid(node, "PublisherRid"),
            node.path("LifecycleGeneration").asLong(),
            node.path("DescriptorRevision").asLong(),
            text(node, "Endpoint"),
            runtimeState(text(node, "State")),
            text(node, "SecurityIdentity"),
            text(node, "OwnerId"),
            node.path("OwnerLeaseGeneration").asLong(),
            updatedAt);
    }

    public static String fanoutPublisherImmutableFingerprint(
        ZLinkFanoutPublisherDescriptor row) {
        StringBuilder preimage = new StringBuilder();
        appendImmutableSegment(
            preimage,
            "zlink-fanout-publisher-immutable-v1");
        appendImmutableSegment(preimage, row.channelName());
        appendImmutableSegment(
            preimage,
            row.publisherRid().toHex().toLowerCase(
                java.util.Locale.ROOT));
        appendImmutableSegment(
            preimage,
            Long.toString(row.lifecycleGeneration()));
        appendImmutableSegment(preimage, row.endpoint());
        appendImmutableSegment(preimage, row.securityIdentity());
        try {
            return HexFormat.of().formatHex(
                MessageDigest.getInstance("SHA-256").digest(
                    preimage.toString().getBytes(
                        StandardCharsets.UTF_8)));
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(
                "SHA-256 is unavailable",
                impossible);
        }
    }

    public static String meshNodeImmutableFingerprint(
        ZLinkMeshNodeDescriptor row) {
        try {
            return HexFormat.of().formatHex(
                MessageDigest.getInstance("SHA-256").digest(
                    meshNodeImmutablePreimage(row).getBytes(
                        StandardCharsets.UTF_8)));
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(
                "SHA-256 is unavailable",
                impossible);
        }
    }

    public static String meshNodeImmutablePreimage(
        ZLinkMeshNodeDescriptor row) {
        StringBuilder preimage = new StringBuilder();
        appendImmutableSegment(
            preimage,
            "zlink-mesh-node-immutable-v2");
        appendImmutableSegment(preimage, row.meshName());
        appendImmutableSegment(
            preimage,
            row.rid().toHex().toLowerCase(java.util.Locale.ROOT));
        appendImmutableSegment(
            preimage,
            Long.toString(row.lifecycleGeneration()));
        appendImmutableSegment(preimage, row.endpoint());

        List<String> channelNames = new ArrayList<>(
            row.channelWeights().keySet());
        channelNames.sort(
            ZLinkLocationDescriptorCodec::compareUtf8);
        appendImmutableSegment(
            preimage,
            Integer.toString(channelNames.size()));
        channelNames.forEach(
            value -> appendImmutableSegment(preimage, value));

        appendImmutableSegment(preimage, row.securityIdentity());
        appendImmutableSegment(
            preimage,
            Long.toString(row.applicationVersion()));
        appendImmutableSegment(
            preimage,
            row.objectRole().name().toLowerCase(
                java.util.Locale.ROOT));
        appendImmutableSegment(
            preimage,
            row.entrySpotId().isPresent() ? "1" : "0");
        row.entrySpotId().ifPresent(
            value -> appendImmutableSegment(preimage, value));
        appendImmutableSegment(
            preimage,
            Integer.toString(row.capacity().actors().limit()));
        appendImmutableSegment(
            preimage,
            Integer.toString(row.capacity().spots().limit()));
        appendImmutableSegment(
            preimage,
            Integer.toString(
                row.activationConcurrency().limit()));

        List<ZLinkObjectCapability> capabilities = new ArrayList<>(
            row.objectCapabilities());
        capabilities.sort((left, right) -> {
            int kind = compareUtf8(
                objectKindToken(left.objectKind()),
                objectKindToken(right.objectKind()));
            return kind != 0
                ? kind
                : compareUtf8(left.stableType(), right.stableType());
        });
        appendImmutableSegment(
            preimage,
            Integer.toString(capabilities.size()));
        for (ZLinkObjectCapability capability : capabilities) {
            appendImmutableSegment(
                preimage,
                objectKindToken(capability.objectKind()));
            appendImmutableSegment(preimage, capability.stableType());
            appendImmutableSegment(
                preimage,
                capability.policy().name().toLowerCase(
                    java.util.Locale.ROOT));
            appendImmutableSegment(
                preimage,
                capability.hasSnapshotAdapter() ? "1" : "0");
            appendImmutableSegment(
                preimage,
                capability.objectKind()
                    == ZLinkPlacementObjectKind.ACTOR
                    ? ""
                    : Integer.toString(capability.spotLimit()));
        }
        return preimage.toString();
    }

    private static void appendImmutableSegment(
        StringBuilder target,
        String value) {
        target.append(value.getBytes(StandardCharsets.UTF_8).length)
            .append(':')
            .append(value);
    }

    private static int compareUtf8(String left, String right) {
        byte[] leftBytes = left.getBytes(StandardCharsets.UTF_8);
        byte[] rightBytes = right.getBytes(StandardCharsets.UTF_8);
        int size = Math.min(leftBytes.length, rightBytes.length);
        for (int index = 0; index < size; index++) {
            int compared = Integer.compare(
                Byte.toUnsignedInt(leftBytes[index]),
                Byte.toUnsignedInt(rightBytes[index]));
            if (compared != 0) {
                return compared;
            }
        }
        return Integer.compare(leftBytes.length, rightBytes.length);
    }

    public static String serializeMeshNodeAdmissionCapabilities(
        List<ZLinkObjectCapability> values) {
        ArrayNode capabilities = JSON.createArrayNode();
        values.stream()
            .sorted(java.util.Comparator
                .<ZLinkObjectCapability, String>comparing(
                    capability -> objectKindToken(
                        capability.objectKind()))
                .thenComparing(ZLinkObjectCapability::stableType))
            .forEach(capability -> {
                ObjectNode encoded = JSON.createObjectNode();
                encoded.put(
                    "objectKind",
                    objectKindToken(capability.objectKind()));
                encoded.put("stableType", capability.stableType());
                encoded.put(
                    "policy",
                    capability.policy().name()
                        .toLowerCase(java.util.Locale.ROOT));
                encoded.put(
                    "hasSnapshotAdapter",
                    capability.hasSnapshotAdapter());
                encoded.put("spotLimit", capability.spotLimit());
                capabilities.add(encoded);
            });
        return write(capabilities);
    }

    private static String objectKindToken(
        ZLinkPlacementObjectKind kind) {
        return switch (kind) {
            case ACTOR -> "actor";
            case USER_SPOT -> "user_spot";
            case INSTANCE_SPOT -> "instance_spot";
        };
    }

    public static ZLinkMeshNodeDescriptor deserializeMeshNode(
        String json,
        long lifecycleGeneration,
        Instant updatedAt) {
        JsonNode node = read(json);
        long encodedLifecycle =
            node.path("LifecycleGeneration").asLong();
        if (encodedLifecycle != lifecycleGeneration) {
            throw new IllegalArgumentException(
                "stored MeshNode lifecycle metadata does not match JSON");
        }
        Map<String, Integer> channels = new HashMap<>();
        node.path("ChannelWeights").fields().forEachRemaining(
            entry -> channels.put(
                entry.getKey(),
                entry.getValue().asInt()));
        List<ZLinkObjectCapability> capabilities = new ArrayList<>();
        node.path("ObjectCapabilities").forEach(capability -> {
            capabilities.add(new ZLinkObjectCapability(
                placementObjectKind(
                    capability.path("ObjectKind").asInt()),
                text(capability, "StableType"),
                maintenancePolicy(
                    capability.path("Policy").asInt()),
                capability.path("HasSnapshotAdapter").asBoolean(),
                capability.path("SpotLimit").asInt()));
        });
        JsonNode encodedCapacity = node.path("Capacity");
        List<ZLinkSpotTypeCapacity> spotTypes = new ArrayList<>();
        encodedCapacity.path("SpotTypes").forEach(value ->
            spotTypes.add(new ZLinkSpotTypeCapacity(
                placementObjectKind(
                    value.path("ObjectKind").asInt()),
                text(value, "StableType"),
                deserializeCapacityUsage(value.path("Usage")))));
        return new ZLinkMeshNodeDescriptor(
            text(node, "MeshName"),
            rid(node, "Rid"),
            encodedLifecycle,
            node.path("DescriptorRevision").asLong(),
            text(node, "Endpoint"),
            channels,
            node.path("ApplicationVersion").asLong(),
            capabilities,
            objectRole(node.path("ObjectRole").asInt()),
            Optional.ofNullable(nullableText(node, "EntrySpotId")),
            node.path("PlacementWeight").asInt(),
            new ZLinkPlacementCapacity(
                deserializeCapacityUsage(
                    encodedCapacity.path("Actors")),
                deserializeCapacityUsage(
                    encodedCapacity.path("Spots")),
                spotTypes),
            new systems.zlink.framework.locations.ZLinkActivationConcurrency(
                    node.path("ActivationConcurrency")
                        .path("Active").asInt(),
                    node.path("ActivationConcurrency")
                        .path("Limit").asInt()),
            Optional.ofNullable(
                nullableText(node, "MaintenanceWave")),
            runtimeState(node.path("State").asInt()),
            text(node, "SecurityIdentity"),
            text(node, "OwnerId"),
            node.path("LeaseGeneration").asLong(),
            updatedAt);
    }

    private static ZLinkCapacityUsage deserializeCapacityUsage(
        JsonNode encoded) {
        return new ZLinkCapacityUsage(
            encoded.path("Active").asInt(),
            encoded.path("Reserved").asInt(),
            encoded.path("Limit").asInt());
    }

    private static JsonNode read(String json) {
        try {
            return JSON.readTree(json);
        } catch (JsonProcessingException ex) {
            throw new IllegalArgumentException("invalid Redis location row JSON", ex);
        }
    }

    private static String write(JsonNode node) {
        try {
            return JSON.writeValueAsString(node);
        } catch (JsonProcessingException ex) {
            throw new IllegalStateException("failed to serialize Redis location row", ex);
        }
    }

    private static void putRid(ObjectNode node, String field, RoutingId rid) {
        if (rid == null) {
            node.putNull(field);
        } else {
            node.put(field, rid.size() == 0 ? "" : rid.toHex());
        }
    }

    private static RoutingId rid(JsonNode node, String field) {
        String value = nullableText(node, field);
        return value == null || value.isEmpty() ? null : RoutingId.fromHex(value);
    }

    private static void putInstant(ObjectNode node, String field, Instant value) {
        if (value == null) {
            node.put(field, Instant.EPOCH.toString());
        } else {
            node.put(field, INSTANT_WIRE_FORMAT.format(value));
        }
    }

    private static void putNullableText(ObjectNode node, String field, String value) {
        if (value == null) {
            node.putNull(field);
        } else {
            node.put(field, value);
        }
    }

    private static String text(JsonNode node, String field) {
        String value = nullableText(node, field);
        return value == null ? "" : value;
    }

    private static String nullableText(JsonNode node, String field) {
        JsonNode value = node.get(field);
        return value == null || value.isNull() ? null : value.asText();
    }

    private static String runtimeStateName(
        ZLinkFrameworkRuntimeState state) {
        return switch (state) {
            case PREPARING -> "Preparing";
            case SERVING -> "Serving";
            case RELOCATING -> "Relocating";
            case RELOCATED -> "Relocated";
            case DRAINING -> "Draining";
            case STOPPED -> "Stopped";
            case ERROR -> "Error";
        };
    }

    private static ZLinkFrameworkRuntimeState runtimeState(
        String state) {
        return switch (state) {
            case "Preparing" -> ZLinkFrameworkRuntimeState.PREPARING;
            case "Serving" -> ZLinkFrameworkRuntimeState.SERVING;
            case "Relocating" -> ZLinkFrameworkRuntimeState.RELOCATING;
            case "Relocated" -> ZLinkFrameworkRuntimeState.RELOCATED;
            case "Draining" -> ZLinkFrameworkRuntimeState.DRAINING;
            case "Stopped" -> ZLinkFrameworkRuntimeState.STOPPED;
            case "Error" -> ZLinkFrameworkRuntimeState.ERROR;
            default -> throw new IllegalArgumentException(
                "invalid Framework runtime state");
        };
    }

    private static ZLinkPlacementObjectKind placementObjectKind(
        int value) {
        for (ZLinkPlacementObjectKind kind :
            ZLinkPlacementObjectKind.values()) {
            if (kind.value() == value) {
                return kind;
            }
        }
        throw new IllegalArgumentException(
            "invalid placement object kind");
    }

    private static ZLinkObjectMaintenancePolicyKind maintenancePolicy(
        int value) {
        for (ZLinkObjectMaintenancePolicyKind policy :
            ZLinkObjectMaintenancePolicyKind.values()) {
            if (policy.value() == value) {
                return policy;
            }
        }
        throw new IllegalArgumentException(
            "invalid object maintenance policy");
    }

    private static ZLinkMeshNodeObjectRole objectRole(int value) {
        for (ZLinkMeshNodeObjectRole role :
            ZLinkMeshNodeObjectRole.values()) {
            if (role.value() == value) {
                return role;
            }
        }
        throw new IllegalArgumentException(
            "invalid MeshNode object role");
    }

    private static ZLinkFrameworkRuntimeState runtimeState(int value) {
        for (ZLinkFrameworkRuntimeState state :
            ZLinkFrameworkRuntimeState.values()) {
            if (state.wireValue() == value) {
                return state;
            }
        }
        throw new IllegalArgumentException(
            "invalid Framework runtime state");
    }

}
