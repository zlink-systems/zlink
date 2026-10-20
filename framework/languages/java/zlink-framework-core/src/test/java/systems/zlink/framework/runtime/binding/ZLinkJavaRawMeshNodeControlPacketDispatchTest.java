package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaRawMeshNodeControlPacketDispatchTest {
    private static final byte[] CONTROL_PACKET =
            "zlink.internal.spot.relocation.control.v1".getBytes(StandardCharsets.UTF_8);
    private static final byte[] APPLICATION_BODY = new byte[] {1, 2, 3};

    @Test
    void onlyExactRelocationControlPacketUsesTheRelocationHandler() throws Exception {
        RoutingId sourceRid = RoutingId.from("control-dispatch-source");
        RoutingId targetRid = RoutingId.from("control-dispatch-target");
        AtomicInteger applicationDispatches = new AtomicInteger();
        AtomicInteger controlDispatches = new AtomicInteger();

        try (var context = Zlink.createContext();
                var source = meshNode(context);
                var target = meshNode(context)) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://control-dispatch-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind("tcp://127.0.0.1:0");
            target.startDispatch(
                    record -> {
                        try (record;
                                Message replyPacket = Message.from("reply");
                                Message replyBody =
                                        Message.from(record.parts().get(1).toByteArray())) {
                            applicationDispatches.incrementAndGet();
                            record.reply(List.of(replyPacket, replyBody));
                        }
                    });
            target.setRelocationControlHandler(
                    (sender, command) -> {
                        controlDispatches.incrementAndGet();
                        return CompletableFuture.completedFuture(command);
                    });

            source.start();
            target.start();
            CompletableFuture<Void> admitted = new CompletableFuture<>();
            try (AutoCloseable observation =
                    source.onStateChanged(
                            () -> {
                                if (source.peers().stream()
                                        .anyMatch(
                                                peer ->
                                                        peer.routingId().equals(targetRid)
                                                                && peer.state()
                                                                        == MeshPeerState
                                                                                .ADMITTED)) {
                                    admitted.complete(null);
                                }
                            })) {
                source.connectPeer(target.status().localEndpoint(), targetRid);
                admitted.get(10, TimeUnit.SECONDS);
            }

            byte[] command = new byte[] {7, 8, 9};
            assertArrayEquals(
                    command,
                    source.requestRelocationControl(targetRid, command, Duration.ofSeconds(5))
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS));
            assertEquals(1, controlDispatches.get());
            assertEquals(0, applicationDispatches.get());

            byte[] nearMiss = CONTROL_PACKET.clone();
            nearMiss[nearMiss.length - 1] = '2';
            assertEquals(CONTROL_PACKET.length, nearMiss.length);
            assertApplicationDispatch(source, targetRid, nearMiss);
            assertEquals(1, applicationDispatches.get());
            assertEquals(1, controlDispatches.get());

            byte[] invalidUtf8 = CONTROL_PACKET.clone();
            invalidUtf8[0] = (byte) 0xff;
            assertEquals(CONTROL_PACKET.length, invalidUtf8.length);
            assertApplicationDispatch(source, targetRid, invalidUtf8);
            assertEquals(2, applicationDispatches.get());
            assertEquals(1, controlDispatches.get());

            byte[] envelopePacket;
            try (Message envelope =
                    ZLinkChannelEnvelope.encodeHeader(
                            ZLinkChannelEnvelope.create(
                                    ZLinkChannelEnvelope.KIND_REQUEST,
                                    "control-dispatch",
                                    "Request",
                                    "application/json",
                                    null,
                                    Map.of(),
                                    null))) {
                envelopePacket = envelope.toByteArray();
            }
            assertApplicationDispatch(source, targetRid, envelopePacket);
            assertEquals(3, applicationDispatches.get());
            assertEquals(1, controlDispatches.get());
        }
    }

    private static void assertApplicationDispatch(
            ZLinkJavaRawMeshNode source, RoutingId targetRid, byte[] firstPart) throws Exception {
        try (Message packet = Message.from(firstPart);
                Message body = Message.from(APPLICATION_BODY);
                var reply =
                        source.spotNode()
                                .requestToNode(
                                        targetRid,
                                        null,
                                        List.of(packet, body),
                                        Duration.ofSeconds(5),
                                        null,
                                        null,
                                        null)
                                .toCompletableFuture()
                                .get(10, TimeUnit.SECONDS)) {
            assertEquals(ZLinkBackendRequestResult.OK, reply.result());
            assertEquals("reply", reply.parts().getFirst().toUtf8String());
            assertArrayEquals(APPLICATION_BODY, reply.parts().get(1).toByteArray());
        }
    }

    private static ZLinkJavaRawMeshNode meshNode(Context context) {
        ZLinkJavaRawMeshNode node = new ZLinkJavaRawMeshNode(context, "mesh");
        node.setApplicationJobQueue(
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.empty(),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null)));
        return node;
    }
}
