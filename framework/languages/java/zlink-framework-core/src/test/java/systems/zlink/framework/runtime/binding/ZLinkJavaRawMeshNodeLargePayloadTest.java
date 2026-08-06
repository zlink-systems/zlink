package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.ServerSocket;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;

final class ZLinkJavaRawMeshNodeLargePayloadTest {
    @Test
    void oneMebibyteRequestCrossesTheTcpRouteMesh() throws Exception {
        String endpoint = "tcp://127.0.0.1:" + availableTcpPort();
        RoutingId sourceRid = RoutingId.from("large-payload-source");
        RoutingId targetRid = RoutingId.from("large-payload-target");
        byte[] payloadBytes = new byte[1024 * 1024];
        java.util.Arrays.fill(payloadBytes, (byte) 'x');

        try (var context = Zlink.createContext();
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://large-payload-source-" + System.nanoTime());
            source.setRouterHighWaterMark(4_096_000L);
            source.setRouterReceiveHighWaterMark(4_096_000L);
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setRouterHighWaterMark(4_096_000L);
            target.setRouterReceiveHighWaterMark(4_096_000L);
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            target.startDispatch(record -> {
                try (record;
                     Message packet = Message.from("reply");
                     Message payload = Message.from(record.parts().get(1).toByteArray())) {
                    assertEquals(RecordKind.NODE_REQUEST, record.receive().kind());
                    record.reply(List.of(packet, payload));
                }
            });

            CompletableFuture<ZLinkBackendReceived> completed =
                new CompletableFuture<>();
            try (Message packet = Message.from("request");
                 Message payload = Message.from(payloadBytes)) {
                boolean submitted = source.spotNode().requestToNode(
                    targetRid,
                    List.of(packet, payload),
                    completed::complete,
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(5));
                assertTrue(submitted);
            }

            try (ZLinkBackendReceived reply =
                completed.get(10, TimeUnit.SECONDS)) {
                assertEquals(ZLinkBackendRequestResult.OK, reply.result());
                assertEquals("reply", reply.parts().getFirst().toUtf8String());
                assertArrayEquals(payloadBytes, reply.parts().get(1).toByteArray());
            }
        }
    }

    private static void awaitAdmitted(ZLinkJavaRawMeshNode node)
        throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        while (System.nanoTime() < deadline) {
            if (node.peers().stream().anyMatch(
                    peer -> peer.state() == MeshPeerState.ADMITTED)) {
                return;
            }
            Thread.sleep(5);
        }
        throw new AssertionError("RouteMesh peer did not become admitted");
    }

    private static int availableTcpPort() throws Exception {
        try (var socket = new ServerSocket(0)) {
            socket.setReuseAddress(true);
            return socket.getLocalPort();
        }
    }
}
