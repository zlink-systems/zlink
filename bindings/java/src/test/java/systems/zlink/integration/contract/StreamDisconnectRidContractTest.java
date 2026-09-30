/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ConnectResult;
import systems.zlink.contracts.errors.ZlinkConnectException;
import systems.zlink.contracts.messaging.StreamPacket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.StreamRecvMode;
import systems.zlink.contracts.sockets.StreamSocket;

class StreamDisconnectRidContractTest {
    @Test
    void disconnectRidClosesAcceptedClientAndReturnsConnectNotFound() throws Exception {
        TestSupport.assumeNative();

        String endpoint = TestSupport.tcpEndpoint();
        int port = Integer.parseInt(endpoint.substring(endpoint.lastIndexOf(':') + 1));
        byte[] payload = "disconnect-peer".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] frame = ByteBuffer.allocate(6 + payload.length)
            .putShort((short) 0)
            .putInt(payload.length)
            .put(payload)
            .array();

        try (Context context = Zlink.createContext();
             StreamSocket stream = context.createStreamSocket();
             java.net.Socket client = new java.net.Socket();
             StreamPacket packet = new StreamPacket()) {
            stream.options().recvMode(StreamRecvMode.PACKET);
            stream.bind(endpoint);
            client.connect(new InetSocketAddress("127.0.0.1", port),
                TestSupport.DEFAULT_TIMEOUT_MS);
            client.setSoTimeout(TestSupport.DEFAULT_TIMEOUT_MS);
            client.getOutputStream().write(frame);
            client.getOutputStream().flush();

            assertTrue(stream.recvPacket(packet, RecvFlags.NONE));
            RoutingId peerRid = packet.routingId().orElseThrow();
            stream.disconnectRid(peerRid);
            assertEquals(-1, client.getInputStream().read(), "client did not observe EOF");

            ZlinkConnectException error = assertThrows(ZlinkConnectException.class,
                () -> stream.disconnectRid(peerRid));
            assertEquals(ConnectResult.NOT_FOUND, error.getResult());
            assertEquals(605, error.getResult().value());
        }
    }
}
