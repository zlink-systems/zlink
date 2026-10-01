package systems.zlink.framework.locations.redis;

import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationIdentity;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.HexFormat;
import java.util.Locale;

final class ZLinkRedisLocationKeys {
    private final String prefix;

    ZLinkRedisLocationKeys(String prefix) {
        this.prefix = prefix;
    }

    String leaseKey(String ownerId) {
        return domainBase() + ":owner-lease:" + sha256Hex(ownerId);
    }

    String authorityRowKey(String authorityKey) {
        return domainBase() + ":authority:current:" + sha256Hex(authorityKey);
    }

    String authorityIndexKey() {
        return domainBase() + ":authority:key-index";
    }

    String creationTerminalKey(ZLinkCreationOperationIdentity operation) {
        byte[] sourceRid = operation.sourceNodeRid().toBytes();
        return prefix
                + ":{zlink-location-v3}:creation-terminal:"
                + sourceRid.length
                + ":"
                + HexFormat.of().formatHex(sourceRid)
                + ":"
                + operation.sourceLifecycleGeneration()
                + ":"
                + String.format(
                        Locale.ROOT,
                        "%016x%016x",
                        operation.operationIdHigh(),
                        operation.operationIdLow());
    }

    String authorityMembershipsKey() {
        return domainBase() + ":membership:current";
    }

    String schemaKey() {
        return domainBase() + ":schema";
    }

    /**
     * The opaque record key: {@code {prefix}:{zlink-location-v3}:opaque: {sha256hex(preimage)}}
     * exactly, per 21-location-runtime.md#2.4 / 22-location-store-redis.md#7 and the
     * store-record-v1 golden fixture (owner ruling: the Redis Cluster hashtag braces are canonical
     * and win over an earlier brace-less reading; the spec/golden text is being corrected to
     * match). Shares {@link #domainBase()} with the dedicated Lua paths so the opaque store's
     * multi-key EVAL scripts (record + index + map + cleanup + sequence + snapshot keys) keep
     * Cluster same-slot atomicity.
     */
    String opaqueRecordKey(String key) {
        return domainBase() + ":opaque:" + sha256Hex(key);
    }

    String opaqueIndexKey() {
        return domainBase() + ":opaque:index";
    }

    String opaqueMapKey() {
        return domainBase() + ":opaque:map";
    }

    String opaqueCleanupKey() {
        return domainBase() + ":opaque:cleanup";
    }

    String opaqueSequenceKey() {
        return domainBase() + ":opaque:sequence";
    }

    String opaqueSnapshotExpiryKey() {
        return domainBase() + ":opaque:snapshot-expiry";
    }

    String opaqueSnapshotBoundaryKey() {
        return domainBase() + ":opaque:snapshot-boundary";
    }

    String opaqueScanKey(String scanId) {
        return domainBase() + ":opaque:scan:" + scanId.replace("-", "").toLowerCase(Locale.ROOT);
    }

    private String domainBase() {
        return prefix + ":{zlink-location-v3}";
    }

    private static String sha256Hex(String value) {
        try {
            return HexFormat.of()
                    .formatHex(
                            MessageDigest.getInstance("SHA-256")
                                    .digest(value.getBytes(StandardCharsets.UTF_8)));
        } catch (NoSuchAlgorithmException exception) {
            throw new IllegalStateException(
                    "SHA-256 is required by the Redis location schema", exception);
        }
    }
}
