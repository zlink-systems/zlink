package systems.zlink.framework.runtime.messaging;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;

/**
 * Immutable framework representation of the canonical Core application
 * metadata frame.
 */
public final class ZLinkApplicationMetadata {
    private static final int VERSION = 1;
    private static final int MAX_ENCODED_SIZE = 1024;
    private static final ZLinkApplicationMetadata EMPTY =
        new ZLinkApplicationMetadata(Map.of());

    private final Map<String, String> values;

    private ZLinkApplicationMetadata(Map<String, String> values) {
        this.values = values;
    }

    public static ZLinkApplicationMetadata empty() {
        return EMPTY;
    }

    public static ZLinkApplicationMetadata copyOf(Map<String, String> metadata) {
        Objects.requireNonNull(metadata, "metadata");
        if (metadata.isEmpty()) {
            return EMPTY;
        }
        LinkedHashMap<String, String> copy = new LinkedHashMap<>();
        metadata.forEach((key, value) ->
            copy.put(requireKey(key), Objects.requireNonNull(value, "metadata value")));
        return new ZLinkApplicationMetadata(
            Collections.unmodifiableMap(copy));
    }

    public ZLinkApplicationMetadata with(String key, String value) {
        LinkedHashMap<String, String> copy = new LinkedHashMap<>(values);
        copy.put(requireKey(key), Objects.requireNonNull(value, "metadata value"));
        return new ZLinkApplicationMetadata(Collections.unmodifiableMap(copy));
    }

    public ZLinkApplicationMetadata withAll(Map<String, String> metadata) {
        Objects.requireNonNull(metadata, "metadata");
        if (metadata.isEmpty()) {
            return this;
        }
        LinkedHashMap<String, String> copy = new LinkedHashMap<>(values);
        metadata.forEach((key, value) ->
            copy.put(requireKey(key), Objects.requireNonNull(value, "metadata value")));
        return new ZLinkApplicationMetadata(Collections.unmodifiableMap(copy));
    }

    public Map<String, String> values() {
        return values;
    }

    public byte[] encode() {
        if (values.isEmpty()) {
            return new byte[0];
        }
        if (values.size() > 255) {
            throw invalid("application metadata may contain at most 255 entries");
        }
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        output.write(VERSION);
        output.write(values.size());
        for (Map.Entry<String, String> entry : values.entrySet()) {
            byte[] key = utf8(entry.getKey(), "key");
            byte[] value = utf8(entry.getValue(), "value");
            if (key.length == 0 || key.length > 255) {
                throw invalid(
                    "application metadata key must encode to 1..255 UTF-8 bytes");
            }
            if (value.length > 65535) {
                throw invalid(
                    "application metadata value must encode to at most 65535 UTF-8 bytes");
            }
            output.write(key.length);
            output.writeBytes(key);
            output.write(value.length >>> 8);
            output.write(value.length);
            output.writeBytes(value);
            if (output.size() > MAX_ENCODED_SIZE) {
                throw invalid("encoded application metadata exceeds 1024 bytes");
            }
        }
        return output.toByteArray();
    }

    public static Map<String, String> decode(byte[] encoded) {
        if (encoded == null || encoded.length == 0) {
            return Map.of();
        }
        if (encoded.length > MAX_ENCODED_SIZE) {
            throw invalid("encoded application metadata exceeds 1024 bytes");
        }
        int offset = 0;
        int version = encoded[offset++] & 0xff;
        if (version != VERSION) {
            throw invalid("unsupported application metadata version: " + version);
        }
        if (offset >= encoded.length) {
            throw invalid("application metadata entry count is missing");
        }
        int count = encoded[offset++] & 0xff;
        LinkedHashMap<String, String> decoded = new LinkedHashMap<>();
        for (int index = 0; index < count; index++) {
            if (offset >= encoded.length) {
                throw invalid("application metadata key length is truncated");
            }
            int keyLength = encoded[offset++] & 0xff;
            if (keyLength == 0 || offset + keyLength > encoded.length) {
                throw invalid("application metadata key is empty or truncated");
            }
            String key = decodeUtf8(encoded, offset, keyLength, "key");
            offset += keyLength;
            if (offset + 2 > encoded.length) {
                throw invalid("application metadata value length is truncated");
            }
            int valueLength =
                ((encoded[offset] & 0xff) << 8) | (encoded[offset + 1] & 0xff);
            offset += 2;
            if (offset + valueLength > encoded.length) {
                throw invalid("application metadata value is truncated");
            }
            String value = decodeUtf8(encoded, offset, valueLength, "value");
            offset += valueLength;
            if (decoded.putIfAbsent(key, value) != null) {
                throw invalid("application metadata contains duplicate key: " + key);
            }
        }
        if (offset != encoded.length) {
            throw invalid("application metadata contains trailing bytes");
        }
        return Collections.unmodifiableMap(decoded);
    }

    private static String requireKey(String key) {
        Objects.requireNonNull(key, "metadata key");
        if (key.isEmpty()) {
            throw invalid("application metadata key must not be empty");
        }
        return key;
    }

    private static byte[] utf8(String value, String field) {
        if (value.indexOf('\0') >= 0) {
            throw invalid("application metadata " + field + " must not contain NUL");
        }
        return value.getBytes(StandardCharsets.UTF_8);
    }

    private static String decodeUtf8(
        byte[] encoded,
        int offset,
        int length,
        String field) {
        try {
            String value = StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(encoded, offset, length))
                .toString();
            if (value.indexOf('\0') >= 0) {
                throw invalid(
                    "application metadata " + field + " must not contain NUL");
            }
            return value;
        } catch (CharacterCodingException error) {
            throw invalid(
                "application metadata " + field + " is not valid UTF-8",
                error);
        }
    }

    private static IllegalArgumentException invalid(String message) {
        return new IllegalArgumentException(message);
    }

    private static IllegalArgumentException invalid(
        String message,
        Throwable cause) {
        return new IllegalArgumentException(message, cause);
    }
}
