package systems.zlink.framework.runtime.internal.locations;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

/**
 * Keeps owner generation allocation and lease fencing inside the Framework.
 */
final class ZLinkProviderOwnerLeaseRepository {
    private static final String PREFIX = "zlink:v11:";
    private static final ZLinkStoreKey OWNER_COUNTER =
        new ZLinkStoreKey(PREFIX + "owner-counter");
    private final systems.zlink.framework.locationprovider.ZLinkLocationStore
        provider;

    ZLinkProviderOwnerLeaseRepository(
        systems.zlink.framework.locationprovider.ZLinkLocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
    }

    CompletionStage<ZLinkOwnerLeaseClaimResult> claim(
        String ownerId,
        Duration leaseTtl) {
        requireOwner(ownerId);
        requireRetention(leaseTtl);
        return claimLoop(ownerId, leaseTtl);
    }

    CompletionStage<ZLinkOwnerLeaseReadResult> read(String ownerId) {
        requireOwner(ownerId);
        return provider.read(ownerKey(ownerId), active())
            .thenApply(result -> {
                if (result instanceof ZLinkStoreReadMissing) {
                    return new ZLinkOwnerLeaseMissing();
                }
                ZLinkStoreValue value =
                    ((ZLinkStoreReadFound) result).value();
                OwnerRecord record = decodeOwner(value.bytes());
                if (!ownerId.equals(record.ownerId())
                    || value.expiresAt() == null) {
                    throw new IllegalStateException(
                        "Location Store owner lease record is invalid");
                }
                return new ZLinkOwnerLeaseFound(
                    new ZLinkLocationOwnerToken(
                        record.ownerId(),
                        record.generation()),
                    value.expiresAt(),
                    value.storeNow());
            });
    }

    CompletionStage<ZLinkOwnerLeaseRenewResult> renew(
        ZLinkLocationOwnerToken token,
        Duration leaseTtl) {
        Objects.requireNonNull(token, "token");
        requireRetention(leaseTtl);
        ZLinkStoreKey key = ownerKey(token.ownerId());
        return provider.read(key, active()).thenCompose(current -> {
            if (!(current instanceof ZLinkStoreReadFound found)
                || decodeOwner(found.value().bytes()).generation()
                    != token.leaseGeneration()) {
                return completed(new ZLinkOwnerLeaseRenewStale());
            }
            return provider.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(
                            key,
                            found.value().version())),
                        List.of(new ZLinkStorePut(
                            key,
                            found.value().bytes(),
                            leaseTtl))),
                    active())
                .thenApply(result -> result
                    instanceof ZLinkStoreWriteApplied applied
                    ? new ZLinkOwnerLeaseRenewed(
                        applied.storeNow().plus(leaseTtl),
                        applied.storeNow())
                    : new ZLinkOwnerLeaseRenewStale());
        });
    }

    CompletionStage<ZLinkOwnerLeaseReleaseResult> release(
        ZLinkLocationOwnerToken token) {
        Objects.requireNonNull(token, "token");
        ZLinkStoreKey key = ownerKey(token.ownerId());
        return provider.read(key, active()).thenCompose(current -> {
            if (!(current instanceof ZLinkStoreReadFound found)
                || decodeOwner(found.value().bytes()).generation()
                    != token.leaseGeneration()) {
                return completed(ZLinkOwnerLeaseReleaseResult.STALE);
            }
            return provider.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(
                            key,
                            found.value().version())),
                        List.of(new ZLinkStoreDelete(key))),
                    active())
                .thenApply(result -> result
                    instanceof ZLinkStoreWriteApplied
                    ? ZLinkOwnerLeaseReleaseResult.RELEASED
                    : ZLinkOwnerLeaseReleaseResult.STALE);
        });
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> claimLoop(
        String ownerId,
        Duration leaseTtl) {
        ZLinkStoreKey ownerKey = ownerKey(ownerId);
        return provider.read(ownerKey, active()).thenCompose(owner -> {
            if (owner instanceof ZLinkStoreReadFound) {
                return completed(new ZLinkOwnerLeaseClaimConflict());
            }
            return provider.read(OWNER_COUNTER, active())
                .thenCompose(counter -> {
                    long generation = counter instanceof ZLinkStoreReadFound found
                        ? decodeCounter(found.value().bytes())
                        : 1;
                    if (generation == Long.MAX_VALUE) {
                        return completed(
                            new ZLinkOwnerLeaseGenerationExhausted());
                    }
                    var token = new ZLinkLocationOwnerToken(
                        ownerId, generation);
                    var counterCondition =
                        counter instanceof ZLinkStoreReadFound found
                            ? new ZLinkStoreVersionCondition(
                                OWNER_COUNTER,
                                found.value().version())
                            : new ZLinkStoreMissingCondition(OWNER_COUNTER);
                    var request = new ZLinkStoreWriteRequest(
                        List.of(
                            new ZLinkStoreMissingCondition(ownerKey),
                            counterCondition),
                        List.of(
                            new ZLinkStorePut(
                                ownerKey,
                                encodeOwner(ownerId, generation),
                                leaseTtl),
                            new ZLinkStorePut(
                                OWNER_COUNTER,
                                encodeCounter(generation + 1),
                                null)));
                    CompletionStage<ZLinkStoreWriteResult> write;
                    try {
                        write = provider.write(request, active());
                    } catch (Throwable failure) {
                        return reconcileClaim(
                            ownerKey,
                            token,
                            failure);
                    }
                    CompletionStage<
                        CompletionStage<ZLinkOwnerLeaseClaimResult>>
                        decision = write.handle((result, failure) -> {
                            if (failure != null) {
                                return reconcileClaim(
                                    ownerKey,
                                    token,
                                    unwrap(failure));
                            }
                            if (result instanceof ZLinkStoreWriteConflict) {
                                return claimLoop(ownerId, leaseTtl);
                            }
                            var applied = (ZLinkStoreWriteApplied) result;
                            return completed(
                                (ZLinkOwnerLeaseClaimResult)
                                    new ZLinkOwnerLeaseClaimed(
                                        token,
                                        applied.storeNow().plus(leaseTtl),
                                        applied.storeNow()));
                        });
                    return decision.thenCompose(stage -> stage);
                });
        });
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> reconcileClaim(
        ZLinkStoreKey key,
        ZLinkLocationOwnerToken expected,
        Throwable originalFailure) {
        return provider.read(key, active())
            .toCompletableFuture()
            .orTimeout(5, TimeUnit.SECONDS)
            .handle((result, failure) -> {
                if (failure == null
                    && result instanceof ZLinkStoreReadFound found) {
                    OwnerRecord record = decodeOwner(
                        found.value().bytes());
                    if (record.ownerId().equals(expected.ownerId())
                        && record.generation()
                            == expected.leaseGeneration()
                        && found.value().expiresAt() != null) {
                        return (ZLinkOwnerLeaseClaimResult)
                            new ZLinkOwnerLeaseClaimed(
                                expected,
                                found.value().expiresAt(),
                                found.value().storeNow());
                    }
                }
                throw new CompletionException(originalFailure);
            });
    }

    private static byte[] encodeOwner(String ownerId, long generation) {
        byte[] owner = ownerId.getBytes(StandardCharsets.UTF_8);
        return ByteBuffer.allocate(4 + owner.length + Long.BYTES)
            .putInt(owner.length)
            .put(owner)
            .putLong(generation)
            .array();
    }

    private static OwnerRecord decodeOwner(byte[] bytes) {
        try {
            ByteBuffer input = ByteBuffer.wrap(bytes);
            int length = input.getInt();
            if (length < 1 || length > input.remaining() - Long.BYTES) {
                throw new IllegalStateException();
            }
            byte[] owner = new byte[length];
            input.get(owner);
            long generation = input.getLong();
            if (input.hasRemaining() || generation <= 0) {
                throw new IllegalStateException();
            }
            return new OwnerRecord(
                new String(owner, StandardCharsets.UTF_8),
                generation);
        } catch (RuntimeException failure) {
            throw new IllegalStateException(
                "Location Store owner lease record is invalid",
                failure);
        }
    }

    private static byte[] encodeCounter(long value) {
        return ByteBuffer.allocate(Long.BYTES).putLong(value).array();
    }

    private static long decodeCounter(byte[] bytes) {
        if (bytes.length != Long.BYTES) {
            throw new IllegalStateException(
                "Location Store owner counter is invalid");
        }
        long value = ByteBuffer.wrap(bytes).getLong();
        if (value <= 0) {
            throw new IllegalStateException(
                "Location Store owner counter is invalid");
        }
        return value;
    }

    private static ZLinkStoreKey ownerKey(String ownerId) {
        byte[] bytes = ownerId.getBytes(StandardCharsets.UTF_8);
        return new ZLinkStoreKey(
            PREFIX + "owner:" + bytes.length + ":" + ownerId + ":");
    }

    private static void requireOwner(String ownerId) {
        if (ownerId == null || ownerId.isBlank()) {
            throw new IllegalArgumentException("ownerId is required");
        }
    }

    private static void requireRetention(Duration retention) {
        Objects.requireNonNull(retention, "retention");
        if (retention.isZero() || retention.isNegative()) {
            throw new IllegalArgumentException(
                "lease retention must be positive");
        }
    }

    private static systems.zlink.framework.locationprovider
        .ZLinkStoreCancellation active() {
        return () -> false;
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException completion
            && completion.getCause() != null
            ? completion.getCause()
            : failure;
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private record OwnerRecord(String ownerId, long generation) {}

}
