package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.IOException;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;

/**
 * Canonical owner-lease opaque record codec (21-location-runtime.md#2.4):
 * NUL-preimage key {@code owner-lease\0{OwnerId}}, value
 * {@code {recordVersion:1, ownerId, leaseGeneration}} (leaseGeneration a
 * JSON string, since it's a 64-bit value).
 *
 * <p>Three independent readers of this same Redis row must agree
 * byte-for-byte on this encoding: {@link ZLinkProviderOwnerLeaseRepository}
 * (the canonical writer), {@link ZLinkProviderDescriptorRepository} (fences
 * MeshNode/ClientServer/fanout writes against the current lease), and
 * {@link ZLinkProviderAuthorityRepository} (fences authority operations
 * against the current lease). This class is the single shared
 * implementation so those three can't drift apart.</p>
 */
final class ZLinkOwnerLeaseRecordCodec {
    private static final ObjectMapper JSON = new ObjectMapper();

    private ZLinkOwnerLeaseRecordCodec() {
    }

    static ZLinkStoreKey key(String ownerId) {
        if (ownerId.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(
                "ownerId must not contain a NUL byte");
        }
        return new ZLinkStoreKey("owner-lease\0" + ownerId);
    }

    static byte[] encode(String ownerId, long leaseGeneration) {
        ObjectNode root = JSON.createObjectNode();
        root.put("recordVersion", 1);
        root.put("ownerId", ownerId);
        root.put("leaseGeneration", Long.toString(leaseGeneration));
        try {
            return JSON.writeValueAsBytes(root);
        } catch (JsonProcessingException error) {
            throw new IllegalStateException(
                "Failed to encode owner lease record", error);
        }
    }

    static Record decode(byte[] bytes) {
        try {
            JsonNode root = JSON.readTree(bytes);
            if (root.path("recordVersion").asInt(-1) != 1) {
                throw new IllegalStateException(
                    "Location Store owner lease record has an"
                        + " unrecognized recordVersion");
            }
            if (!root.hasNonNull("ownerId")) {
                throw new IllegalStateException(
                    "Location Store owner lease record is missing"
                        + " ownerId");
            }
            long leaseGeneration = Long.parseLong(
                root.path("leaseGeneration").asText());
            if (leaseGeneration <= 0) {
                throw new IllegalStateException(
                    "Location Store owner lease generation is invalid");
            }
            return new Record(root.path("ownerId").asText(), leaseGeneration);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException(
                "Location Store owner lease record is invalid", failure);
        }
    }

    record Record(String ownerId, long leaseGeneration) {
    }
}
