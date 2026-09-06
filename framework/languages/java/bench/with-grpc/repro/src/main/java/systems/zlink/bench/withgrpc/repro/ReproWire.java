/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.repro;

import java.nio.charset.StandardCharsets;

/** The two-part wire the bench raw row uses, duplicated so the repro imports no bench code. */
final class ReproWire {
    static final byte[] REQUEST_ENVELOPE = (
        "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\","
        + "\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,"
        + "\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}")
        .getBytes(StandardCharsets.UTF_8);

    private ReproWire() {
    }

    /** {@code BenchPayload { bytes body = 1 }}, hand-encoded to avoid a protobuf dependency. */
    static byte[] encodeBody(byte[] payload) {
        int length = payload.length;
        int varintSize = 1;
        for (int remaining = length; remaining >= 0x80; remaining >>>= 7) {
            varintSize++;
        }
        byte[] encoded = new byte[1 + varintSize + length];
        encoded[0] = 0x0a;
        int offset = 1;
        int remaining = length;
        while (remaining >= 0x80) {
            encoded[offset++] = (byte) ((remaining & 0x7f) | 0x80);
            remaining >>>= 7;
        }
        encoded[offset++] = (byte) remaining;
        System.arraycopy(payload, 0, encoded, offset, length);
        return encoded;
    }
}
