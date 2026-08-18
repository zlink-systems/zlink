package systems.zlink.framework.locations.redis;

import io.lettuce.core.codec.RedisCodec;
import io.lettuce.core.codec.StringCodec;
import java.nio.ByteBuffer;

/**
 * Redis codec pairing UTF-8 string keys with 8-bit-clean byte[] values.
 *
 * <p>Used by the stores that persist the normative {@code zlink-location-v3}
 * opaque record layer (21-location-runtime.md#2.4) and the relocation blob
 * store (23-relocation-store-redis.md#8): both require raw bytes to reach
 * Lua's {@code cmsgpack}/{@code redis.call} boundary untouched by any
 * text-safe sub-encoding such as base64.</p>
 */
final class ZLinkRedisStringByteArrayCodec implements RedisCodec<String, byte[]> {
    static final ZLinkRedisStringByteArrayCodec INSTANCE =
        new ZLinkRedisStringByteArrayCodec();

    private ZLinkRedisStringByteArrayCodec() {
    }

    @Override
    public String decodeKey(ByteBuffer bytes) {
        return StringCodec.UTF8.decodeKey(bytes);
    }

    @Override
    public byte[] decodeValue(ByteBuffer bytes) {
        byte[] value = new byte[bytes.remaining()];
        bytes.get(value);
        return value;
    }

    @Override
    public ByteBuffer encodeKey(String key) {
        return StringCodec.UTF8.encodeKey(key);
    }

    @Override
    public ByteBuffer encodeValue(byte[] value) {
        return ByteBuffer.wrap(value == null ? new byte[0] : value);
    }
}
