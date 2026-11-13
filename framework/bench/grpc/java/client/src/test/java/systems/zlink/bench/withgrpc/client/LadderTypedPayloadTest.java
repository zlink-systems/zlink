/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.bench.withgrpc.client;

import java.util.Arrays;
import systems.zlink.bench.withgrpc.shared.BenchTypedPayload;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;

/** Exercises the production typed JSON encode/decode helpers used by ladder L4. */
public final class LadderTypedPayloadTest {
    private LadderTypedPayloadTest() {}

    public static void main(String[] args) {
        byte[] expected = new byte[] {0x5a, 0x4c, 0x4e, 0x4b, 1, 2, 3};
        var serializer = new ZLinkJsonMessageSerializer();
        try (ZLinkFrameworkMultipartView encoded = ZLinkPayloadEncoding.encodeMessage(
                serializer,
                new BenchTypedPayload(expected),
                "application/json")) {
            var decoded = ZLinkMessagePayloads.deserialize(
                    serializer, encoded.getFirst().dataBuffer(), BenchTypedPayload.class);
            if (!Arrays.equals(expected, decoded.body())) {
                throw new AssertionError("typed JSON payload did not round trip");
            }
        }
    }
}
