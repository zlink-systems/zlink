/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.bench.withgrpc.shared;

import com.google.protobuf.ByteString;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.HexFormat;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.contracts.messaging.Message;

/** Executable wire regression test; no benchmark timing or socket traffic. */
public final class RawWireTest {
    private static final int RUN_ID = 0x01020304;
    private static final long SEQUENCE = 0x0102030405060708L;
    private static final long TIMESTAMP = 0x1112131415161718L;
    private static final String GOLDEN =
        "0a1d4b4e4c5a04030201011d00000008070605040302011817161514131211";

    public static void main(String[] args) throws Exception {
        for (int size : new int[] {29, 127, 128, 1024, 4096, 16384}) {
            try (Message raw = RawWire.encodeBenchPayloadMessage(size, RUN_ID, (byte) 1, SEQUENCE)) {
                ByteBuffer view = raw.dataBuffer();
                byte[] actual = new byte[view.remaining()];
                view.get(actual);
                // Capture the clock once from the real encoder; serialize exactly that body.
                byte[] body = BenchPayload.parseFrom(actual).getBody().toByteArray();
                byte[] expected = BenchPayload.newBuilder().setBody(ByteString.copyFrom(body))
                    .build().toByteArray();
                require(Arrays.equals(actual, expected), "raw/protobuf mismatch at " + size);
                require(BenchMetricHeader.isExpected(BenchMetricHeader.decode(body), RUN_ID,
                    (byte) 1, size, SEQUENCE), "metric header mismatch");
                for (int i = 29; i < body.length; i++) {
                    require(body[i] == (byte) 0xab, "payload fill mismatch");
                }
                require(Arrays.equals(body, toBytes(RawWire.decodeBenchPayloadBody(
                    ByteBuffer.wrap(expected)))), "decode mismatch");
                try (Message reply = RawWire.encodeBenchPayloadMessage(ByteBuffer.wrap(body))) {
                    require(Arrays.equals(actual, toBytes(reply.dataBuffer())), "reply wire mismatch");
                }
                if (size == 29) {
                    // Only the nondeterministic clock is normalized in the saved wire dump.
                    ByteBuffer.wrap(actual).order(ByteOrder.LITTLE_ENDIAN).putLong(2 + 21, TIMESTAMP);
                    require(HexFormat.of().formatHex(actual).equals(GOLDEN), "golden dump mismatch");
                    System.out.println("java raw/protobuf golden=" + HexFormat.of().formatHex(actual));
                }
            }
        }
        require(RawWire.decodeBenchPayloadBody(ByteBuffer.wrap(new byte[] {0x0a, 0x1d, 1}))
            == null, "truncated protobuf must fail");
        byte[] unknownThenBody = HexFormat.of().parseHex("1007" + GOLDEN);
        require(toBytes(RawWire.decodeBenchPayloadBody(ByteBuffer.wrap(unknownThenBody))).length == 29,
            "protobuf unknown fields must be accepted");
        System.out.println("JAVA_RAW_WIRE_OK");
    }

    private static byte[] toBytes(ByteBuffer view) {
        require(view != null, "missing decoded body");
        byte[] bytes = new byte[view.remaining()];
        view.get(bytes);
        return bytes;
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
}
