package systems.zlink;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubSocket;
import systems.zlink.contracts.sockets.XPubSocket;
import java.nio.charset.StandardCharsets;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

public class NativeContractTest {
    @Test
    public void testRawMultipartSendRecvSinglePart() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PairSocket left = ctx.createPairSocket();
             PairSocket right = ctx.createPairSocket()) {
            String endpoint = TestSupport.inprocEndpoint("native-send");
            left.bind(endpoint);
            right.connect(endpoint);

            byte[] payload = "native".getBytes(StandardCharsets.UTF_8);
            try (Message outbound = Message.from(payload)) {
                assertTrue(right.send()
                    .message(outbound)
                    .flags(SendFlags.NONE)
                    .submit());
            }

            try (systems.zlink.contracts.messaging.Received inbound = new systems.zlink.contracts.messaging.Received()) {


                left.recv(inbound, systems.zlink.contracts.sockets.RecvFlags.NONE);
                assertArrayEquals(payload,
                    inbound.singlePartOrThrow().toByteArray());
            }
        }
    }

    @Test
    public void testDedicatedOptionFamilyDowncalls() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             RouterSocket router = ctx.createRouterSocket();
             XPubSocket xpub = ctx.createXPubSocket();
             SubSocket sub = ctx.createSubSocket();
             StreamSocket stream = ctx.createStreamSocket()) {
            router.options().mandatory(true);
            assertTrue(router.options().mandatory());

            xpub.options().verbose(true);
            assertEquals(0, xpub.options().topicsCount());

            sub.setSubscription("native-contract");
            assertEquals(1, sub.options().topicsCount());

            stream.options().notify(true);
            assertTrue(stream.options().notifyEnabled());
        }
    }
}
