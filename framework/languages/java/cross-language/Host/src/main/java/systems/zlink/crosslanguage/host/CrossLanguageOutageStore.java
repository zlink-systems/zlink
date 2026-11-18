package systems.zlink.crosslanguage.host;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Base64;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.zip.CRC32C;

/** Test-only fault injection at the opaque Store boundary; Redis owns all data. */
public final class CrossLanguageOutageStore implements ZLinkLocationStore {
    private Consumer<String> events = ignored -> {};
    private volatile long cutoffNanos;
    private volatile String timingFailure;
    private volatile long casSubmittedNanos, descriptorConfirmedNanos, casConfirmedNanos;

    public void timing(Consumer<String> events, long t0, long cutoff) {
        this.events = events;
        cutoffNanos = cutoff;
        event("t0", t0);
        event("cutoff", cutoff);
    }

    private void event(String stage, long nanos) {
        events.accept("outage-timing|event=" + stage + "|monotonicMs=" + (nanos / 1_000_000.0));
    }

    public void newClaim() {
        event("new-claim", System.nanoTime());
    }

    public void checkTiming() {
        if (timingFailure != null) throw new IllegalStateException(timingFailure);
        if (recovering
                && (descriptorConfirmedNanos == 0 || casConfirmedNanos == 0)
                && System.nanoTime() >= cutoffNanos)
            throw new IllegalStateException("deadline-exceeded|confirmation wait reached cutoff");
    }

    private void confirm(boolean descriptor) {
        if (descriptor ? descriptorConfirmedNanos != 0 : casConfirmedNanos != 0) return;
        long now = System.nanoTime();
        event(descriptor ? "descriptor-confirmed" : "cas-confirmed", now);
        if (now >= cutoffNanos) {
            timingFailure =
                    "deadline-exceeded|"
                            + (descriptor ? "descriptor" : "cas")
                            + " confirmation reached cutoff";
            throw new IllegalStateException(timingFailure);
        }
        if (descriptor) descriptorConfirmedNanos = now;
        else casConfirmedNanos = now;
    }

    public void assertConfirmed() {
        if (timingFailure != null) throw new IllegalStateException(timingFailure);
        if (casSubmittedNanos == 0 || descriptorConfirmedNanos == 0 || casConfirmedNanos == 0)
            throw new IllegalStateException("Preserve confirmations missing");
    }

    private final ZLinkLocationStore redis;
    private final ZLinkStoreKey descriptorKey;
    private final ZLinkStoreKey actorKey;
    private volatile long lastLeaseWriteStartedNanos;
    private volatile boolean unavailable;
    private volatile boolean recovering;
    private volatile long descriptorLease;
    private volatile long preserveLease;
    private final CompletableFuture<Void> failedOperation = new CompletableFuture<>();
    private final CompletableFuture<Void> descriptorSeen = new CompletableFuture<>();
    private final CompletableFuture<Void> casSeen = new CompletableFuture<>();
    private final CompletableFuture<Void> descriptorRelease = new CompletableFuture<>();
    private final CompletableFuture<Void> casRelease = new CompletableFuture<>();

    public CrossLanguageOutageStore(
            ZLinkLocationStore redis, String meshName, String nodeRid, String actorId) {
        this.redis = redis;
        descriptorKey = key("mesh-node", meshName, RoutingId.from(nodeRid).toHex());
        actorKey = key("authority", "actor", actorId);
    }

    public CompletionStage<Authority> readAuthority() {
        return redis.read(actorKey, () -> false)
                .thenApply(
                        result -> {
                            if (!(result instanceof ZLinkStoreReadFound found))
                                throw new IllegalStateException("actor authority missing");
                            return authority(found.value().bytes());
                        });
    }

    public CompletionStage<Long> readOwnerLease(String ownerId) {
        return redis.read(key("owner-lease", ownerId), () -> false)
                .thenApply(
                        result ->
                                result instanceof ZLinkStoreReadFound found
                                        ? integer(json(found.value().bytes()), "leaseGeneration")
                                        : null);
    }

    public void observeTiming(Consumer<String> events) {
        if (lastLeaseWriteStartedNanos == 0)
            throw new IllegalStateException("last successful owner lease Store write missing");
        // The public status omits the runtime request start. Bound it by the successful
        // Store request start minus the maximum duration of its enclosing renewal.
        long lowerBound =
                lastLeaseWriteStartedNanos
                        - OutageLeaseProfile.duration("renewTimeoutMs").toNanos();
        timing(events, lowerBound, lowerBound + OutageLeaseProfile.lifetimeNanos());
        events.accept(
                "outage-observation|runtimeCutoff=unavailable|cutoffSource=store-request-conservative-bound");
    }

    public void outage() {
        unavailable = true;
    }

    public void recover() {
        recovering = true;
        unavailable = false;
    }

    public CompletionStage<Void> failedOperation() {
        return failedOperation;
    }

    public long descriptorLease() {
        return descriptorLease;
    }

    public long preserveLease() {
        return preserveLease;
    }

    public CompletionStage<Void> bothConfirmationsSeen() {
        return CompletableFuture.allOf(descriptorSeen, casSeen);
    }

    public CompletionStage<Void> releaseDescriptor() {
        return descriptorSeen.thenRun(() -> descriptorRelease.complete(null));
    }

    public CompletionStage<Void> releaseCas() {
        return casSeen.thenRun(() -> casRelease.complete(null));
    }

    private <T> CompletionStage<T> failure() {
        failedOperation.complete(null);
        return CompletableFuture.failedFuture(
                new IllegalStateException("injected Location Store outage"));
    }

    @Override
    public CompletionStage<ZLinkStoreReadResult> read(
            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
        if (unavailable) return failure();
        return redis.read(key, cancellation);
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
        return unavailable ? failure() : redis.scan(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
        long receivedNanos = System.nanoTime();
        if (unavailable) return failure();
        if (recovering
                && (descriptorConfirmedNanos == 0 || casConfirmedNanos == 0)
                && request.mutations().stream()
                        .anyMatch(
                                mutation ->
                                        mutation instanceof ZLinkStoreDelete delete
                                                && actorKey.equals(delete.key()))) {
            timingFailure = "discarded|actor authority Delete during recovery";
            return CompletableFuture.failedFuture(new IllegalStateException(timingFailure));
        }
        boolean descriptor =
                recovering
                        && descriptorConfirmedNanos == 0
                        && request.mutations().stream()
                                .anyMatch(
                                        mutation ->
                                                mutation instanceof ZLinkStorePut put
                                                        && descriptorKey.equals(put.key()));
        boolean cas =
                recovering
                        && casConfirmedNanos == 0
                        && request.mutations().stream()
                                .anyMatch(
                                        mutation ->
                                                mutation instanceof ZLinkStorePut put
                                                        && actorKey.equals(put.key()));
        if (cas) {
            casSubmittedNanos = receivedNanos;
            event("cas-submit", casSubmittedNanos);
            if (casSubmittedNanos + OutageLeaseProfile.duration("renewTimeoutMs").toNanos()
                    > cutoffNanos) {
                timingFailure =
                        "deadline-exceeded|CAS submission plus renew timeout exceeds cutoff";
                return CompletableFuture.failedFuture(new IllegalStateException(timingFailure));
            }
        }
        return writeRedis(request, cancellation)
                .thenCompose(
                        result -> {
                            if (!(result instanceof ZLinkStoreWriteApplied))
                                return CompletableFuture.completedFuture(result);
                            if (descriptor) {
                                descriptorLease =
                                        request.mutations().stream()
                                                .filter(
                                                        mutation ->
                                                                mutation
                                                                                instanceof
                                                                                ZLinkStorePut put
                                                                        && descriptorKey.equals(
                                                                                put.key()))
                                                .map(
                                                        mutation ->
                                                                integer(
                                                                        json(
                                                                                ((ZLinkStorePut)
                                                                                                mutation)
                                                                                        .bytes()),
                                                                        "leaseGeneration"))
                                                .findFirst()
                                                .orElseThrow();
                                descriptorSeen.complete(null);
                                return descriptorRelease.thenApply(
                                        ignored -> {
                                            confirm(true);
                                            return result;
                                        });
                            }
                            if (cas) {
                                preserveLease =
                                        request.mutations().stream()
                                                .filter(
                                                        mutation ->
                                                                mutation
                                                                                instanceof
                                                                                ZLinkStorePut put
                                                                        && actorKey.equals(
                                                                                put.key()))
                                                .map(
                                                        mutation ->
                                                                authority(
                                                                        ((ZLinkStorePut) mutation)
                                                                                .bytes()))
                                                .findFirst()
                                                .orElseThrow()
                                                .ownerLeaseGeneration();
                                casSeen.complete(null);
                                return casRelease.thenApply(
                                        ignored -> {
                                            confirm(false);
                                            return result;
                                        });
                            }
                            return CompletableFuture.completedFuture(result);
                        });
    }

    private CompletionStage<ZLinkStoreWriteResult> writeRedis(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
        long started = System.nanoTime();
        boolean leaseWrite =
                request.mutations().stream()
                        .anyMatch(
                                mutation ->
                                        mutation instanceof ZLinkStorePut put
                                                && put.key().value().startsWith("owner-lease\0"));
        return redis.write(request, cancellation)
                .thenApply(
                        result -> {
                            if (leaseWrite && result instanceof ZLinkStoreWriteApplied)
                                lastLeaseWriteStartedNanos =
                                        Math.max(lastLeaseWriteStartedNanos, started);
                            return result;
                        });
    }

    /** Minimal observations of Location runtime section 2.4 and authority-payload-v1. */
    public record Authority(
            long objectGeneration,
            long authorityOwnerGeneration,
            String ownerId,
            long ownerLeaseGeneration,
            String meshName,
            String nodeRidHex,
            String lifecycleGeneration,
            boolean steadyReady) {
        public boolean sameIncarnation(Authority other) {
            return objectGeneration == other.objectGeneration
                    && authorityOwnerGeneration == other.authorityOwnerGeneration
                    && ownerId.equals(other.ownerId)
                    && meshName.equals(other.meshName)
                    && nodeRidHex.equals(other.nodeRidHex)
                    && lifecycleGeneration.equals(other.lifecycleGeneration);
        }
    }

    private static final ObjectMapper JSON = new ObjectMapper();

    private static ZLinkStoreKey key(String... segments) {
        for (String segment : segments) {
            if (segment.indexOf('\0') >= 0)
                throw new IllegalArgumentException("canonical key segment contains NUL");
        }
        return new ZLinkStoreKey(String.join("\0", segments));
    }

    private static JsonNode json(byte[] bytes) {
        try {
            JsonNode root = JSON.readTree(bytes);
            if (!root.path("recordVersion").isInt() || root.path("recordVersion").intValue() != 1)
                throw new IllegalStateException("unsupported canonical Store record version");
            return root;
        } catch (java.io.IOException error) {
            throw new IllegalStateException("invalid canonical Store JSON", error);
        }
    }

    private static String text(JsonNode node, String field) {
        JsonNode value = node.path(field);
        if (!value.isTextual())
            throw new IllegalStateException("canonical string missing: " + field);
        return value.textValue();
    }

    private static long integer(JsonNode node, String field) {
        return Long.parseLong(text(node, field));
    }

    private static Authority authority(byte[] bytes) {
        JsonNode root = json(bytes);
        JsonNode allocation = root.path("allocation");
        JsonNode descriptor = allocation.path("descriptor");
        boolean ready =
                root.path("pendingCreation").isNull()
                        && "active".equals(text(allocation, "state"))
                        && "actor".equals(text(allocation, "objectKind"))
                        && readyActor(Base64.getDecoder().decode(text(root, "payload")));
        return new Authority(
                integer(root, "objectGeneration"),
                integer(root, "authorityOwnerGeneration"),
                text(root, "ownerId"),
                integer(root, "ownerLeaseGeneration"),
                text(descriptor, "meshName"),
                text(descriptor, "routingIdHex"),
                text(allocation, "descriptorLifecycleGeneration"),
                ready);
    }

    private static boolean readyActor(byte[] payload) {
        ByteBuffer envelope = ByteBuffer.wrap(payload).order(ByteOrder.BIG_ENDIAN);
        if (envelope.remaining() < 15
                || envelope.getInt() != 0x5a4c4155
                || envelope.get() != 1
                || envelope.getShort() != 0)
            throw new IllegalStateException("invalid authority-payload-v1 header");
        int bodySize = envelope.getInt();
        if (bodySize != envelope.remaining() - 4)
            throw new IllegalStateException("invalid authority body length");
        CRC32C checksum = new CRC32C();
        checksum.update(payload, 0, payload.length - 4);
        if ((int) checksum.getValue() != envelope.getInt(payload.length - 4))
            throw new IllegalStateException("invalid authority CRC32C");
        int operation = Byte.toUnsignedInt(envelope.get());
        if (envelope.get() != 1) throw new IllegalStateException("authority is not an actor");
        int identitySize = Short.toUnsignedInt(envelope.getShort());
        int identityEnd = envelope.position() + identitySize;
        skipText(envelope); // Stable type.
        skipText(envelope); // Actor ID.
        int state = Byte.toUnsignedInt(envelope.get());
        envelope.position(identityEnd);
        skipText(envelope); // Owner ID.
        envelope.getLong(); // Owner lease generation.
        skipText(envelope); // Mesh name.
        skipText(envelope); // RID bytes use the same u8 length prefix.
        envelope.getLong(); // Node lifecycle generation.
        boolean activationAbsent = emptySlot(envelope);
        boolean relocationAbsent = emptySlot(envelope);
        if (envelope.position() != payload.length - 4)
            throw new IllegalStateException("trailing authority bytes");
        return operation == 0 && state == 1 && activationAbsent && relocationAbsent;
    }

    private static void skipText(ByteBuffer bytes) {
        int length = Byte.toUnsignedInt(bytes.get());
        bytes.position(bytes.position() + length);
    }

    private static boolean emptySlot(ByteBuffer bytes) {
        int present = Byte.toUnsignedInt(bytes.get());
        long size = Integer.toUnsignedLong(bytes.getInt());
        if (present > 1 || size > bytes.remaining() || (present == 0 && size != 0))
            throw new IllegalStateException("invalid authority optional slot");
        bytes.position(Math.addExact(bytes.position(), Math.toIntExact(size)));
        return present == 0;
    }
}
