package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;

import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Reads both v11 capacity layouts for settlement and conversion. */
final class ZLinkLegacyCapacity {
    static final String PREFIX = "zlink:v11:capacity:";
    private static final ObjectMapper JSON = new ObjectMapper();

    private final ObjectNode root;
    private final Map<String, Field> active = new LinkedHashMap<>();
    private final Map<String, Field> pending = new LinkedHashMap<>();

    private ZLinkLegacyCapacity(ObjectNode root) {
        this.root = root;
        if (root.has("active") || root.has("pending")) {
            readNested(object(root.required("active")), active);
            readNested(object(root.required("pending")), pending);
        } else {
            readTotals(root, "Active", active);
            readTotals(root, "Pending", pending);
            for (var type : object(root.required("spotTypes")).properties()) {
                String dimension = typeDimension(type.getKey(), ':');
                var counts = object(type.getValue());
                active.put(dimension, new Field(counts, "active"));
                pending.put(dimension, new Field(counts, "pending"));
            }
        }
    }

    static ZLinkLegacyCapacity decode(ZLinkStoreKey key, byte[] bytes) {
        try {
            return new ZLinkLegacyCapacity(object(JSON.readTree(bytes)));
        } catch (IOException | IllegalArgumentException failure) {
            throw new ZLinkConfigurationException(
                    "Invalid v11 capacity record " + key.value(), failure);
        }
    }

    static ZLinkStoreKey key(ZLinkMeshNodeDescriptorKey descriptor) {
        return new ZLinkStoreKey(
                PREFIX
                        + encodeComponent(descriptor.meshName())
                        + ":"
                        + encodeComponent(descriptor.rid().toString()));
    }

    List<ZLinkStorePut> counterPuts(ZLinkMeshNodeDescriptorKey target) {
        var puts = new ArrayList<ZLinkStorePut>();
        appendPuts(puts, target, active, "active");
        appendPuts(puts, target, pending, "pending");
        return puts;
    }

    byte[] release(ZLinkPlacementCapacityBundle bundle) {
        subtract("actors", bundle.actorSlots());
        subtract("spots", bundle.spotSlots());
        bundle.spotType()
                .ifPresent(
                        type ->
                                subtract(
                                        ZLinkCapacityCounters.spotType(
                                                type.objectKind(), type.stableType()),
                                        type.slots()));
        return root.toString().getBytes(StandardCharsets.UTF_8);
    }

    private void subtract(String dimension, int delta) {
        if (delta == 0) return;
        Field field = pending.get(dimension);
        if (field == null || field.count() < delta)
            throw new ZLinkConfigurationException(
                    "Insufficient v11 pending capacity for " + dimension);
        field.object().put(field.name(), field.count() - delta);
    }

    private static void appendPuts(
            List<ZLinkStorePut> puts,
            ZLinkMeshNodeDescriptorKey target,
            Map<String, Field> fields,
            String state) {
        fields.forEach(
                (dimension, field) -> {
                    long count = field.count();
                    if (count != 0) {
                        puts.add(
                                new ZLinkStorePut(
                                        ZLinkCapacityCounters.key(target, dimension, state),
                                        ZLinkLocationStoreFormat.encodeCounter(count),
                                        null));
                    }
                });
    }

    private static void readNested(ObjectNode usage, Map<String, Field> fields) {
        readTotals(usage, "", fields);
        var types = object(usage.required("spotTypes"));
        for (var type : types.properties()) {
            fields.put(typeDimension(type.getKey(), '\0'), new Field(types, type.getKey()));
        }
    }

    private static void readTotals(ObjectNode usage, String suffix, Map<String, Field> fields) {
        fields.put("actors", new Field(usage, "actors" + suffix));
        fields.put("spots", new Field(usage, "spots" + suffix));
    }

    private static String typeDimension(String key, char delimiter) {
        int separator = key.indexOf(delimiter);
        if (separator < 0) throw new IllegalArgumentException("Invalid v11 capacity spot type");
        var kind =
                switch (key.substring(0, separator)) {
                    case "1", "actor" -> ZLinkPlacementObjectKind.ACTOR;
                    case "2", "user_spot" -> ZLinkPlacementObjectKind.USER_SPOT;
                    case "3", "instance_spot" -> ZLinkPlacementObjectKind.INSTANCE_SPOT;
                    default -> throw new IllegalArgumentException("Invalid v11 capacity spot kind");
                };
        return ZLinkCapacityCounters.spotType(kind, key.substring(separator + 1));
    }

    private static ObjectNode object(JsonNode node) {
        if (node instanceof ObjectNode object) return object;
        throw new IllegalArgumentException("Invalid v11 capacity object");
    }

    private static String encodeComponent(String value) {
        var encoded = new StringBuilder();
        for (byte b : value.getBytes(StandardCharsets.UTF_8)) {
            int c = b & 0xff;
            if (c >= 'A' && c <= 'Z'
                    || c >= 'a' && c <= 'z'
                    || c >= '0' && c <= '9'
                    || "-_.!~*'()".indexOf(c) >= 0) {
                encoded.append((char) c);
            } else {
                encoded.append('%').append(HexFormat.of().withUpperCase().toHexDigits(b));
            }
        }
        return encoded.toString();
    }

    private record Field(ObjectNode object, String name) {
        Field {
            count(object, name);
        }

        long count() {
            return count(object, name);
        }

        private static long count(ObjectNode object, String name) {
            var value = object.required(name);
            if (!value.isIntegralNumber() || !value.canConvertToLong() || value.longValue() < 0)
                throw new IllegalArgumentException("Invalid v11 capacity count: " + name);
            return value.longValue();
        }
    }
}
