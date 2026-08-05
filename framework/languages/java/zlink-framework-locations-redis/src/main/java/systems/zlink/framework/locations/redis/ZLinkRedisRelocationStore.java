package systems.zlink.framework.locations.redis;

import io.lettuce.core.ScriptOutputType;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.Base64;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.locationprovider.ZLinkBlobAlreadyStored;
import systems.zlink.framework.locationprovider.ZLinkBlobConflict;
import systems.zlink.framework.locationprovider.ZLinkBlobFound;
import systems.zlink.framework.locationprovider.ZLinkBlobMissing;
import systems.zlink.framework.locationprovider.ZLinkBlobPutResult;
import systems.zlink.framework.locationprovider.ZLinkBlobReadResult;
import systems.zlink.framework.locationprovider.ZLinkBlobReference;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewMissing;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewResult;
import systems.zlink.framework.locationprovider.ZLinkBlobRenewed;
import systems.zlink.framework.locationprovider.ZLinkBlobStored;
import systems.zlink.framework.locationprovider.ZLinkRelocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;

public final class ZLinkRedisRelocationStore
    implements ZLinkRelocationStore, AutoCloseable {
    private static final int MAX_COMPONENT_BYTES = 64 * 1024 * 1024;
    private static final String PUT = """
        if redis.replicate_commands then redis.replicate_commands() end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        local current = redis.call('GET', KEYS[1])
        if current == ARGV[1] then
            local ttl = redis.call('PTTL', KEYS[1])
            return {'already', nowMs, ttl}
        elseif current then
            return {'collision', nowMs}
        end
        redis.call('SET', KEYS[1], ARGV[1], 'PX', ARGV[2])
        return {'stored', nowMs, tonumber(ARGV[2])}
        """;
    private static final String READ = """
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        local value = redis.call('GET', KEYS[1])
        if not value then return {'missing', nowMs} end
        return {'found', nowMs, redis.call('PTTL', KEYS[1]), value}
        """;
    private static final String RENEW = """
        if redis.replicate_commands then redis.replicate_commands() end
        local time = redis.call('TIME')
        local nowMs = tonumber(time[1]) * 1000 + math.floor(tonumber(time[2]) / 1000)
        if redis.call('EXISTS', KEYS[1]) == 0 then
            return {'missing', nowMs}
        end
        redis.call('PEXPIRE', KEYS[1], ARGV[1])
        return {'renewed', nowMs}
        """;

    private final ZLinkRedisLocationConnection connection;
    private final String keyPrefix;

    public ZLinkRedisRelocationStore(ZLinkRedisRelocationOptions options) {
        ZLinkRedisRelocationOptions validated = Objects.requireNonNull(options, "options");
        validated.validate();
        connection = new ZLinkRedisLocationConnection(validated.redisUri());
        keyPrefix = validated.keyPrefix() + ":{relocation}:root:";
    }

    @Override
    public CompletionStage<ZLinkBlobPutResult> put(
        ZLinkBlobReference reference,
        byte[] payload,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        String normalized = requireReference(reference);
        byte[] snapshot = requirePayload(payload);
        long retentionMs = retentionMillis(retention);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        String encoded = Base64.getEncoder().encodeToString(snapshot);
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                PUT,
                ScriptOutputType.MULTI,
                new String[] {key(normalized)},
                encoded,
                Long.toString(retentionMs)))
            .thenApply(raw -> {
                String status = string(raw.getFirst());
                Instant storeNow = Instant.ofEpochMilli(number(raw.get(1)));
                if ("collision".equals(status)) {
                    return new ZLinkBlobConflict(storeNow);
                }
                Instant expiresAt =
                    storeNow.plusMillis(number(raw.get(2)));
                return "already".equals(status)
                    ? new ZLinkBlobAlreadyStored(expiresAt, storeNow)
                    : new ZLinkBlobStored(expiresAt, storeNow);
            });
    }

    @Override
    public CompletionStage<ZLinkBlobReadResult> read(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation) {
        String normalized = requireReference(reference);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                READ,
                ScriptOutputType.MULTI,
                new String[] {key(normalized)}))
            .thenApply(raw -> {
                Instant now = Instant.ofEpochMilli(number(raw.get(1)));
                if ("missing".equals(string(raw.getFirst()))) {
                    return new ZLinkBlobMissing(now);
                }
                return new ZLinkBlobFound(
                    Base64.getDecoder().decode(string(raw.get(3))),
                    now.plusMillis(number(raw.get(2))),
                    now);
            });
    }

    @Override
    public CompletionStage<ZLinkBlobRenewResult> renew(
        ZLinkBlobReference reference,
        Duration retention,
        ZLinkStoreCancellation cancellation) {
        String normalized = requireReference(reference);
        long retentionMs = retentionMillis(retention);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(redis -> redis.<List<Object>>eval(
                RENEW,
                ScriptOutputType.MULTI,
                new String[] {key(normalized)},
                Long.toString(retentionMs)))
            .thenApply(raw -> {
                Instant storeNow = Instant.ofEpochMilli(number(raw.get(1)));
                return "missing".equals(string(raw.getFirst()))
                    ? new ZLinkBlobRenewMissing(storeNow)
                    : new ZLinkBlobRenewed(
                        storeNow.plusMillis(retentionMs),
                        storeNow);
            });
    }

    @Override
    public CompletionStage<Void> delete(
        ZLinkBlobReference reference,
        ZLinkStoreCancellation cancellation) {
        String normalized = requireReference(reference);
        if (cancelled(cancellation)) {
            return cancelledStage();
        }
        return connection.commands()
            .thenCompose(redis -> redis.del(key(normalized)))
            .thenApply(count -> null);
    }

    @Override
    public void close() {
        connection.closeAsync().toCompletableFuture().join();
    }

    private String key(String reference) {
        return keyPrefix + reference;
    }

    private static byte[] requirePayload(byte[] payload) {
        byte[] snapshot = Objects.requireNonNull(payload, "payload").clone();
        if (snapshot.length > MAX_COMPONENT_BYTES) {
            throw new IllegalArgumentException(
                "relocation Store component exceeds 64 MiB");
        }
        return snapshot;
    }

    private static String requireReference(ZLinkBlobReference reference) {
        String value = Objects.requireNonNull(
            Objects.requireNonNull(reference, "reference").value(),
            "reference.value");
        int bytes = value.getBytes(StandardCharsets.UTF_8).length;
        if (bytes < 1 || bytes > 4096 || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("invalid relocation reference");
        }
        return value;
    }

    private static long retentionMillis(Duration retention) {
        Objects.requireNonNull(retention, "retention");
        if (retention.isZero() || retention.isNegative()) {
            throw new IllegalArgumentException("retention must be positive");
        }
        try {
            return Math.max(1L, retention.toMillis());
        } catch (ArithmeticException error) {
            throw new IllegalArgumentException("retention is too large", error);
        }
    }

    private static boolean cancelled(ZLinkStoreCancellation cancellation) {
        return Objects.requireNonNull(cancellation, "cancellation")
            .isCancellationRequested();
    }

    private static <T> CompletionStage<T> cancelledStage() {
        return CompletableFuture.failedFuture(
            new java.util.concurrent.CancellationException(
                "store operation was cancelled before I/O"));
    }

    private static long number(Object value) {
        if (value instanceof Number number) {
            return number.longValue();
        }
        return Long.parseLong(string(value));
    }

    private static String string(Object value) {
        if (value instanceof byte[] bytes) {
            return new String(bytes, StandardCharsets.UTF_8);
        }
        return value == null ? "" : value.toString();
    }
}
