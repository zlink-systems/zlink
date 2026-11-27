package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.core.Base64Variants;
import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.StreamReadConstraints;
import com.fasterxml.jackson.core.StreamReadFeature;
import com.fasterxml.jackson.core.json.JsonWriteFeature;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.OutputStreamWriter;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.List;
import java.util.Set;
import java.util.function.Supplier;
import java.util.regex.Pattern;

/** Canonical JSON for the Location runtime §9.3 completion record. */
public final class ZLinkRelocationCompletionRecordCodec {
    private static final JsonFactory JSON =
            JsonFactory.builder()
                    .enable(StreamReadFeature.STRICT_DUPLICATE_DETECTION)
                    .disable(JsonWriteFeature.WRITE_HEX_UPPER_CASE)
                    .streamReadConstraints(
                            StreamReadConstraints.builder()
                                    .maxStringLength(Integer.MAX_VALUE)
                                    .build())
                    .build();
    private static final ObjectMapper READER =
            new ObjectMapper(JSON).enable(DeserializationFeature.FAIL_ON_TRAILING_TOKENS);
    private static final Set<String> DELIVERIES =
            Set.of("pending", "terminalReceived", "alreadyTerminal", "sourceLeaseExpired");
    private static final String LONGEST_DELIVERY = "sourceLeaseExpired";
    private static final Pattern RELOCATION_ID =
            Pattern.compile("[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}");
    private static final Pattern KEY = Pattern.compile("zla1:[aus]:.+");
    private static final Pattern RID = Pattern.compile("[0-9a-f]{2,}");
    private static final Pattern OPERATION = Pattern.compile("[0-9a-f]{32}");

    private ZLinkRelocationCompletionRecordCodec() {}

    public record Source(
            String ownerId, String leaseGeneration, String nodeRid, String nodeGeneration) {}

    record Identity(Source source, String operationId) {}

    public record Entry(
            String objectKey,
            Source source,
            String operationId,
            String replyRouteId,
            byte[] reply,
            String delivery) {
        public Entry {
            reply = reply.clone();
        }

        @Override
        public byte[] reply() {
            return reply.clone();
        }

        Identity identity() {
            return new Identity(source, operationId);
        }
    }

    public record Record(String relocationId, List<Entry> entries) {
        public Record {
            entries = List.copyOf(entries);
        }

        public long pendingDeliveries() {
            return entries.stream().filter(entry -> entry.delivery().equals("pending")).count();
        }
    }

    static byte[] encode(Record record) {
        var output = new ByteArrayOutputStream();
        write(record, output);
        return output.toByteArray();
    }

    static void write(Record record, OutputStream output) {
        validate(record);
        try (var writer = generator(output)) {
            writer.writeStartObject();
            writer.writeArrayFieldStart("entries");
            for (var entry : record.entries()) {
                writeEntry(writer, entry, entry.delivery(), true);
            }
            writer.writeEndArray();
            writer.writeStringField("formatVersion", "1");
            writer.writeStringField("relocationId", record.relocationId());
            writer.writeEndObject();
        } catch (IOException failure) {
            throw new UncheckedIOException(failure);
        }
    }

    static Record decode(byte[] bytes) {
        return decode(() -> new ByteArrayInputStream(bytes));
    }

    static Record decode(Supplier<InputStream> bytes) {
        try (var input = bytes.get()) {
            JsonNode root = READER.readTree(input);
            fields(root, "entries", "formatVersion", "relocationId");
            if (!"1".equals(text(root, "formatVersion")) || !root.get("entries").isArray()) {
                throw invalid("Invalid completion version or entries.");
            }
            List<Entry> entries = new ArrayList<>();
            for (JsonNode item : root.get("entries")) {
                fields(
                        item,
                        "delivery",
                        "objectKey",
                        "operationId",
                        "reply",
                        "replyRouteId",
                        "source");
                JsonNode source = item.get("source");
                fields(source, "leaseGeneration", "nodeGeneration", "nodeRid", "ownerId");
                entries.add(
                        new Entry(
                                text(item, "objectKey"),
                                new Source(
                                        text(source, "ownerId"),
                                        text(source, "leaseGeneration"),
                                        text(source, "nodeRid"),
                                        text(source, "nodeGeneration")),
                                text(item, "operationId"),
                                text(item, "replyRouteId"),
                                Base64.getDecoder().decode(text(item, "reply")),
                                text(item, "delivery")));
            }
            var result = new Record(text(root, "relocationId"), entries);
            try (var expected = bytes.get()) {
                write(
                        result,
                        new OutputStream() {
                            private final byte[] block = new byte[8192];

                            @Override
                            public void write(int value) throws IOException {
                                if (expected.read() != value)
                                    throw invalid("Completion JSON is not canonical.");
                            }

                            @Override
                            public void write(byte[] actual, int offset, int length)
                                    throws IOException {
                                while (length > 0) {
                                    int count = Math.min(block.length, length);
                                    if (expected.readNBytes(block, 0, count) != count
                                            || !Arrays.equals(
                                                    block,
                                                    0,
                                                    count,
                                                    actual,
                                                    offset,
                                                    offset + count)) {
                                        throw invalid("Completion JSON is not canonical.");
                                    }
                                    offset += count;
                                    length -= count;
                                }
                            }
                        });
                if (expected.read() != -1) throw invalid("Completion JSON is not canonical.");
            }
            return result;
        } catch (IOException | IllegalArgumentException failure) {
            throw new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(
                    "Invalid completion JSON.", failure);
        }
    }

    /** Includes the longest delivery value and the failure reply for each unfinished request. */
    static long finalSize(Record record, List<Entry> remainingFailures) {
        var seen = validate(record);
        long size = encode(new Record(record.relocationId(), List.of())).length;
        for (var entry : record.entries()) {
            size += finalEntrySize(entry) + 1;
        }
        for (var entry : remainingFailures) {
            validateEntry(entry);
            if (!seen.add(entry.identity())) throw invalid("Duplicate accepted operation.");
            size += finalEntrySize(entry) + 1;
        }
        return seen.isEmpty() ? size : size - 1;
    }

    private static long finalEntrySize(Entry entry) {
        var output = new ByteArrayOutputStream();
        try (var writer = generator(output)) {
            writeEntry(writer, entry, LONGEST_DELIVERY, false);
        } catch (IOException failure) {
            throw new UncheckedIOException(failure);
        }
        return output.size() + 4L * ((entry.reply.length + 2L) / 3L);
    }

    private static JsonGenerator generator(OutputStream output) throws IOException {
        return JSON.createGenerator(new OutputStreamWriter(output, StandardCharsets.UTF_8));
    }

    // The field names below are in RFC 8785 UTF-16 order. Entry order instead uses UTF-8 keys.
    private static void writeEntry(
            JsonGenerator writer, Entry entry, String delivery, boolean reply) throws IOException {
        writer.writeStartObject();
        writer.writeStringField("delivery", delivery);
        writer.writeStringField("objectKey", entry.objectKey());
        writer.writeStringField("operationId", entry.operationId());
        writer.writeFieldName("reply");
        if (reply) {
            writer.writeBinary(
                    Base64Variants.MIME_NO_LINEFEEDS, entry.reply, 0, entry.reply.length);
        } else {
            writer.writeString("");
        }
        writer.writeStringField("replyRouteId", entry.replyRouteId());
        writer.writeObjectFieldStart("source");
        writer.writeStringField("leaseGeneration", entry.source().leaseGeneration());
        writer.writeStringField("nodeGeneration", entry.source().nodeGeneration());
        writer.writeStringField("nodeRid", entry.source().nodeRid());
        writer.writeStringField("ownerId", entry.source().ownerId());
        writer.writeEndObject();
        writer.writeEndObject();
    }

    static String sha256(byte[] bytes) {
        return HexFormat.of().formatHex(sha256().digest(bytes));
    }

    static MessageDigest sha256() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static Set<Identity> validate(Record record) {
        if (!RELOCATION_ID.matcher(record.relocationId()).matches()) {
            throw invalid("Invalid completion relocation identity.");
        }
        Set<Identity> seen = new HashSet<>();
        byte[] previous = new byte[0];
        for (var entry : record.entries()) {
            validateEntry(entry);
            byte[] key = entry.objectKey().getBytes(StandardCharsets.UTF_8);
            if (Arrays.compareUnsigned(previous, key) > 0 || !seen.add(entry.identity())) {
                throw invalid("Completion entry order or identity differs.");
            }
            previous = key;
        }
        return seen;
    }

    private static void validateEntry(Entry entry) {
        var source = entry.source();
        if (!KEY.matcher(entry.objectKey()).matches()
                || !validText(entry.objectKey())
                || !validText(source.ownerId())
                || !RID.matcher(source.nodeRid()).matches()
                || (source.nodeRid().length() & 1) != 0
                || !OPERATION.matcher(entry.operationId()).matches()
                || entry.operationId().equals("00000000000000000000000000000000")
                || !DELIVERIES.contains(entry.delivery())) {
            throw invalid("Invalid completion entry.");
        }
        nonzeroU64(entry.replyRouteId());
        nonzeroU64(source.leaseGeneration());
        nonzeroU64(source.nodeGeneration());
    }

    private static boolean validText(String text) {
        return !text.isEmpty() && StandardCharsets.UTF_8.newEncoder().canEncode(text);
    }

    private static void nonzeroU64(String value) {
        try {
            long number = Long.parseUnsignedLong(value);
            if (number != 0 && Long.toUnsignedString(number).equals(value)) return;
        } catch (NumberFormatException failure) {
            throw invalid("Invalid completion uint64.");
        }
        throw invalid("Invalid completion uint64.");
    }

    private static void fields(JsonNode node, String... expected) {
        if (node == null || !node.isObject() || node.size() != expected.length) {
            throw invalid("Invalid completion fields.");
        }
        for (String field : expected) {
            if (!node.has(field)) throw invalid("Invalid completion fields.");
        }
    }

    private static String text(JsonNode node, String name) {
        var value = node.get(name);
        if (!value.isTextual()) throw invalid("Invalid completion text field.");
        return value.textValue();
    }

    private static ZLinkAggregateRelocationCoordinator.RelocationDataLostException invalid(
            String message) {
        return new ZLinkAggregateRelocationCoordinator.RelocationDataLostException(message);
    }
}
