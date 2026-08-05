/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class RawSocketIntegrationTest {
    @Test
    public void installedCorePackageSupportsRawMessageRoundTrip() {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket()) {
            String endpoint = TestSupport.inprocEndpoint("raw-integration");
            sender.bind(endpoint);
            receiver.connect(endpoint);

            byte[] payload = "raw-jvm-11".getBytes(StandardCharsets.UTF_8);
            try (Message message = Message.from(payload)) {
                sender.send().message(message).submit();
            }

            try (Received received = new Received()) {
                assertTrue(receiver.recv(received, RecvFlags.NONE));
                assertArrayEquals(payload,
                    received.singlePartOrThrow().toByteArray());
            }
        }
    }
}
