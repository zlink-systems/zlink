/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.bench.withgrpc.shared;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;

import com.google.protobuf.UnsafeByteOperations;
import java.nio.ByteBuffer;
import org.junit.jupiter.api.Test;
import systems.zlink.bench.withgrpc.proto.BenchPayload;
import systems.zlink.contracts.messaging.Message;

final class RawWireNativeMessageTest {
    @Test
    void serializesIdenticalBytesDirectlyIntoNativeMessage() {
        for (int size : new int[] {0, 1, 1024, 4096}) {
            byte[] body = new byte[size];
            java.util.Arrays.fill(body, (byte) 0xab);
            BenchPayload payload = BenchPayload.newBuilder()
                .setBody(UnsafeByteOperations.unsafeWrap(body))
                .build();
            byte[] temporaryEncoding = payload.toByteArray();

            try (Message encoded = RawWire.encodeBenchPayloadMessage(ByteBuffer.wrap(body))) {
                ByteBuffer view = encoded.dataBuffer();
                byte[] actual = new byte[view.remaining()];
                view.get(actual);
                assertArrayEquals(temporaryEncoding, actual, "payload size " + size);
            }
        }
    }
}
