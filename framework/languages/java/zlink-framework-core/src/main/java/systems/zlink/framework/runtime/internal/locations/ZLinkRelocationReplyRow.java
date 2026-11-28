package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.StreamReadFeature;
import com.fasterxml.jackson.core.json.JsonWriteFeature;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.UUID;
import java.util.function.Supplier;
import java.util.regex.Pattern;

/** The independently fenced Location Store record for one captured request. */
public record ZLinkRelocationReplyRow(
        UUID relocationId,
        String objectKey,
        Source source,
        String operationId,
        String replyRouteId,
        ZLinkLocationOwnerToken writer,
        String state,
        byte[] reply) {
    public static final Duration ACTIVE_RETENTION = Duration.ofHours(24);
    public static final Duration RENEWAL_THRESHOLD = Duration.ofHours(12);
    static final Duration TERMINAL_RETENTION = Duration.ofMinutes(5);
    static final int MAX_VALUE_BYTES = 1024 * 1024;
    public static final String ACCEPTED = "accepted";
    public static final String PENDING = "pending";
    public static final String TERMINAL_RECEIVED = "terminalReceived";
    public static final String ALREADY_TERMINAL = "alreadyTerminal";
    public static final String SOURCE_LEASE_EXPIRED = "sourceLeaseExpired";
    private static final List<String> STATES =
            List.of(ACCEPTED, PENDING, TERMINAL_RECEIVED, ALREADY_TERMINAL, SOURCE_LEASE_EXPIRED);
    private static final Pattern OPERATION = Pattern.compile("[0-9a-f]{32}");
    private static final Pattern RID = Pattern.compile("(?:[0-9a-f]{2})+");
    private static final JsonFactory JSON =
            JsonFactory.builder()
                    .enable(StreamReadFeature.STRICT_DUPLICATE_DETECTION)
                    .disable(JsonWriteFeature.WRITE_HEX_UPPER_CASE)
                    .build();
    private static final ObjectMapper MAPPER = new ObjectMapper(JSON);

    public record Source(
            String ownerId, String leaseGeneration, String nodeRid, String nodeGeneration) {
        public Source {
            if (ownerId.isEmpty() || ownerId.indexOf(0) >= 0 || !RID.matcher(nodeRid).matches()) {
                throw new IllegalArgumentException("Invalid relocation reply source.");
            }
            positiveToken(leaseGeneration);
            positiveToken(nodeGeneration);
        }
    }

    public ZLinkRelocationReplyRow {
        Objects.requireNonNull(relocationId, "relocationId");
        ZLinkAuthorityKeyCodec.decode(objectKey);
        Objects.requireNonNull(source, "source");
        Objects.requireNonNull(writer, "writer");
        if (writer.ownerId().isEmpty() || writer.leaseGeneration() == 0) {
            throw new IllegalArgumentException("Invalid relocation reply writer.");
        }
        if (!OPERATION.matcher(operationId).matches()
                || operationId.equals("00000000000000000000000000000000")
                || !STATES.contains(state)
                || ACCEPTED.equals(state) != (reply == null)) {
            throw new IllegalArgumentException("Invalid relocation reply row.");
        }
        positiveToken(replyRouteId);
        reply = reply == null ? null : reply.clone();
    }

    @Override
    public byte[] reply() {
        return reply == null ? null : reply.clone();
    }

    public ZLinkRelocationReplyRow accepted() {
        return withState(ACCEPTED);
    }

    public ZLinkRelocationReplyRow withState(String next) {
        return new ZLinkRelocationReplyRow(
                relocationId,
                objectKey,
                source,
                operationId,
                replyRouteId,
                writer,
                next,
                ACCEPTED.equals(next) ? null : reply);
    }

    boolean sameRequest(ZLinkRelocationReplyRow other) {
        return key().equals(other.key())
                && objectKey.equals(other.objectKey())
                && replyRouteId.equals(other.replyRouteId())
                && writer.equals(other.writer());
    }

    static String prefix(UUID relocationId) {
        return "$zlink/relocation-reply/" + relocationId + "/";
    }

    public ZLinkStoreKey key() {
        return key(relocationId, source, operationId);
    }

    public static ZLinkStoreKey key(UUID relocationId, Source source, String operationId) {
        return new ZLinkStoreKey(
                prefix(relocationId)
                        + source.ownerId
                        + "."
                        + source.leaseGeneration
                        + "."
                        + source.nodeRid
                        + "."
                        + source.nodeGeneration
                        + "/"
                        + operationId);
    }

    byte[] encode() {
        var output = new ByteArrayOutputStream();
        try (var json =
                JSON.createGenerator(new OutputStreamWriter(output, StandardCharsets.UTF_8))) {
            json.writeStartObject();
            json.writeStringField("formatVersion", "1");
            json.writeStringField("objectKey", objectKey);
            json.writeStringField("operationId", operationId);
            json.writeStringField("relocationId", relocationId.toString());
            if (reply != null) json.writeBinaryField("reply", reply);
            json.writeStringField("replyRouteId", replyRouteId);
            json.writeObjectFieldStart("source");
            json.writeStringField("leaseGeneration", source.leaseGeneration);
            json.writeStringField("nodeGeneration", source.nodeGeneration);
            json.writeStringField("nodeRid", source.nodeRid);
            json.writeStringField("ownerId", source.ownerId);
            json.writeEndObject();
            json.writeStringField("state", state);
            json.writeObjectFieldStart("writer");
            json.writeStringField(
                    "leaseGeneration", Long.toUnsignedString(writer.leaseGeneration()));
            json.writeStringField("ownerId", writer.ownerId());
            json.writeEndObject();
            json.writeEndObject();
        } catch (IOException failure) {
            throw new UncheckedIOException(failure);
        }
        return output.toByteArray();
    }

    static ZLinkRelocationReplyRow decode(ZLinkStoreKey key, byte[] bytes) {
        try {
            var json = MAPPER.readTree(bytes);
            var source = json.path("source");
            var writer = json.path("writer");
            var row =
                    new ZLinkRelocationReplyRow(
                            UUID.fromString(json.path("relocationId").asText()),
                            json.path("objectKey").asText(),
                            new Source(
                                    source.path("ownerId").asText(),
                                    source.path("leaseGeneration").asText(),
                                    source.path("nodeRid").asText(),
                                    source.path("nodeGeneration").asText()),
                            json.path("operationId").asText(),
                            json.path("replyRouteId").asText(),
                            new ZLinkLocationOwnerToken(
                                    writer.path("ownerId").asText(),
                                    positiveToken(writer.path("leaseGeneration").asText())),
                            json.path("state").asText(),
                            json.has("reply") ? json.path("reply").binaryValue() : null);
            if (!key.equals(row.key()) || !Arrays.equals(bytes, row.encode())) {
                throw new IllegalArgumentException(
                        "Relocation reply key or canonical value differs.");
            }
            return row;
        } catch (IOException | IllegalArgumentException failure) {
            throw new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(
                    "Invalid relocation reply row.", failure);
        }
    }

    public ZLinkRelocationReplyRow boundedResult(Supplier<ZLinkRelocationReplyRow> failure) {
        if (encodedTerminalSize() <= MAX_VALUE_BYTES) return this;
        var fallback = failure.get();
        if (!sameRequest(fallback) || fallback.encodedTerminalSize() > MAX_VALUE_BYTES) {
            throw new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(
                    "Relocation reply fallback differs or exceeds the Store value bound.");
        }
        return fallback;
    }

    /** The row to write, or null when the requested transition has already settled. */
    public ZLinkRelocationReplyRow transition(
            ZLinkRelocationReplyRow current, Instant storeNow, Instant expiresAt) {
        if (!sameRequest(current)) {
            throw new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(
                    "Relocation reply request or writer differs.");
        }
        if (current.terminal()) return null;
        return switch (state) {
            case ACCEPTED -> !expiresAt.isAfter(storeNow.plus(RENEWAL_THRESHOLD)) ? current : null;
            case PENDING -> current.state.equals(ACCEPTED) ? this : null;
            default -> {
                if (!current.state.equals(PENDING)) {
                    throw new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(
                            "Cannot acknowledge a result before it is persisted.");
                }
                yield current.withState(state);
            }
        };
    }

    public record Stored(ZLinkRelocationReplyRow row, Instant expiresAt) {
        public Instant renewAfter() {
            return expiresAt.minus(RENEWAL_THRESHOLD);
        }
    }

    public boolean terminal() {
        return !ACCEPTED.equals(state) && !PENDING.equals(state);
    }

    public Duration retention() {
        return terminal() ? TERMINAL_RETENTION : ACTIVE_RETENTION;
    }

    private long encodedTerminalSize() {
        byte[] empty =
                new ZLinkRelocationReplyRow(
                                relocationId,
                                objectKey,
                                source,
                                operationId,
                                replyRouteId,
                                writer,
                                SOURCE_LEASE_EXPIRED,
                                new byte[0])
                        .encode();
        return empty.length + 4L * ((reply.length + 2L) / 3L);
    }

    private static long positiveToken(String value) {
        long parsed = Long.parseUnsignedLong(value);
        if (parsed == 0 || !value.equals(Long.toUnsignedString(parsed))) {
            throw new IllegalArgumentException("Invalid relocation reply token.");
        }
        return parsed;
    }
}
