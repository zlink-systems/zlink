/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.shared;

import com.google.protobuf.ByteString;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import systems.zlink.bench.withgrpc.proto.BenchPayload;

/**
 * Wire shape of the {@code zlink-<lang>} raw row.
 *
 * <p>The raw row is measured against {@code zlink-c} (spec section 7.2 formula 1), so it
 * must put the same bytes on the wire as {@code bindings/c/bench/with_grpc}. That
 * bench sends a two-part message: an envelope header part and a protobuf-encoded
 * BenchPayload part (bench_zlink_client.cpp:14-16 and :130-140). The .NET and node
 * raw rows do the same. Sending a bare payload here would make formula 1 divide
 * two different experiments (FB-024).
 */
public final class RawWire {
    public static final byte[] REQUEST_ENVELOPE = (
        "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\","
        + "\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,"
        + "\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}")
        .getBytes(StandardCharsets.UTF_8);

    public static final byte[] RESPONSE_ENVELOPE = (
        "{\"kind\":2,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\","
        + "\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,"
        + "\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}")
        .getBytes(StandardCharsets.UTF_8);

    public static final String RAW_REQUEST_SERVER_ID = "bench-raw-request-server";
    public static final String RAW_COMMAND_SERVER_ID = "bench-raw-command-server";

    private RawWire() {
    }

    /** {@code BenchPayload { bytes body = 1 }} around an already-stamped payload. */
    public static byte[] encodeBenchPayload(byte[] payload) {
        return BenchPayload.newBuilder()
            .setBody(ByteString.copyFrom(payload))
            .build()
            .toByteArray();
    }

    /** Field 1 of the encoded BenchPayload, or {@code null} when it is absent. */
    public static ByteBuffer decodeBenchPayloadBody(ByteBuffer encoded) {
        if (encoded == null) {
            return null;
        }
        ByteBuffer view = encoded.duplicate();
        while (view.hasRemaining()) {
            long key = readVarint(view);
            if (key < 0) {
                return null;
            }
            int field = (int) (key >>> 3);
            int wireType = (int) (key & 0x07);
            if (wireType != 2) {
                return null;
            }
            long length = readVarint(view);
            if (length < 0 || length > view.remaining()) {
                return null;
            }
            if (field == 1) {
                ByteBuffer body = view.duplicate();
                body.limit(body.position() + (int) length);
                return body.slice();
            }
            view.position(view.position() + (int) length);
        }
        return null;
    }

    private static long readVarint(ByteBuffer buffer) {
        long value = 0;
        int shift = 0;
        while (shift < 64) {
            if (!buffer.hasRemaining()) {
                return -1;
            }
            int b = buffer.get() & 0xff;
            value |= ((long) (b & 0x7f)) << shift;
            if ((b & 0x80) == 0) {
                return value;
            }
            shift += 7;
        }
        return -1;
    }
}
