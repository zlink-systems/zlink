package systems.zlink.framework.locations.etcd;

import com.google.protobuf.ByteString;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.CompletionException;

final class EtcdEncoding {
    static final long MAX_TIME = 9_223_372_036_854L;
    static final int MAX_PAYLOAD = 1_048_576;
    static final int MAX_PAGE_BYTES = 4 * 1024 * 1024;

    record Metadata(boolean hasExpiry, long expiresAt, int payloadLength) {
        boolean expired(long now) {
            return hasExpiry && expiresAt <= now;
        }
    }

    record Version(long cluster, long revision) {}

    static CompletionException failure() {
        return new CompletionException(
                new IOException(
                        "The Location Store operation failed; its write outcome may be unknown."));
    }

    static ByteString utf8(String value) {
        if (value == null) throw new IllegalArgumentException("A UTF-8 value is required.");
        try {
            return ByteString.copyFrom(
                    StandardCharsets.UTF_8
                            .newEncoder()
                            .onMalformedInput(CodingErrorAction.REPORT)
                            .onUnmappableCharacter(CodingErrorAction.REPORT)
                            .encode(java.nio.CharBuffer.wrap(value)));
        } catch (CharacterCodingException error) {
            throw new IllegalArgumentException("Invalid UTF-8 value.");
        }
    }

    static ByteString validateKey(String value, boolean prefix) {
        ByteString bytes = utf8(value);
        if (bytes.size() > 1024 || (!prefix && bytes.isEmpty()))
            throw new IllegalArgumentException("Invalid Store key length.");
        return bytes;
    }

    static String storedKey(ByteString bytes) {
        if (!bytes.isValidUtf8() || bytes.isEmpty() || bytes.size() > 1024) throw failure();
        return bytes.toStringUtf8();
    }

    static ByteString key(String namespace, char kind, ByteString key) {
        return utf8(namespace)
                .concat(ByteString.copyFrom(new byte[] {0, (byte) kind}))
                .concat(
                        kind == 'c'
                                ? ByteString.EMPTY
                                : ByteString.copyFrom(new byte[] {0}).concat(key));
    }

    static ByteString prefixEnd(ByteString prefix) {
        byte[] bytes = prefix.toByteArray();
        for (int i = bytes.length - 1; i >= 0; i--)
            if ((bytes[i] & 255) < 255) {
                bytes[i]++;
                return ByteString.copyFrom(bytes, 0, i + 1);
            }
        return ByteString.copyFrom(new byte[] {0});
    }

    static ByteString clock(long value) {
        if (value < 0 || value > MAX_TIME)
            throw new IllegalArgumentException("Store time exceeds its bound.");
        return ByteString.copyFrom(ByteBuffer.allocate(8).putLong(value).array());
    }

    static long decodeClock(ByteString bytes) {
        if (bytes.size() != 8) throw failure();
        long value = bytes.asReadOnlyByteBuffer().getLong();
        if (value < 0 || value > MAX_TIME) throw failure();
        return value;
    }

    static ByteString metadata(boolean hasExpiry, long expiresAt, int payloadLength) {
        clock(expiresAt);
        if ((!hasExpiry && expiresAt != 0) || payloadLength < 0 || payloadLength > MAX_PAYLOAD)
            throw new IllegalArgumentException("Invalid metadata.");
        return ByteString.copyFrom(
                ByteBuffer.allocate(14)
                        .put((byte) 1)
                        .put((byte) (hasExpiry ? 1 : 0))
                        .putLong(expiresAt)
                        .putInt(payloadLength)
                        .array());
    }

    static Metadata decodeMetadata(ByteString bytes) {
        if (bytes.size() != 14) throw failure();
        ByteBuffer buffer = bytes.asReadOnlyByteBuffer();
        if (buffer.get() != 1) throw failure();
        int flags = buffer.get() & 255;
        long expires = decodeClock(bytes.substring(2, 10));
        buffer.position(10);
        int length = buffer.getInt();
        if (flags > 1 || (flags == 0 && expires != 0) || length < 0 || length > MAX_PAYLOAD)
            throw failure();
        return new Metadata(flags == 1, expires, length);
    }

    static String version(long cluster, long revision) {
        if (revision <= 0) throw failure();
        return "e1:" + String.format(java.util.Locale.ROOT, "%016x", cluster) + ":" + revision;
    }

    static Version decodeVersion(String value) {
        if (value == null || !value.matches("e1:[0-9a-f]{16}:[1-9][0-9]*")) return null;
        try {
            return new Version(
                    Long.parseUnsignedLong(value.substring(3, 19), 16),
                    Long.parseLong(value.substring(20)));
        } catch (NumberFormatException error) {
            return null;
        }
    }

    static long retention(Duration value) {
        if (value == null) return 0;
        if (value.isZero()
                || value.isNegative()
                || value.compareTo(Duration.ofMillis(MAX_TIME)) > 0)
            throw new IllegalArgumentException("Invalid Store retention.");
        return value.toMillis() + (value.getNano() % 1_000_000 == 0 ? 0 : 1);
    }
}
