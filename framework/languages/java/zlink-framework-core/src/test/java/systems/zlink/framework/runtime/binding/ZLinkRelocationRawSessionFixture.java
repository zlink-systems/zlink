package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/** Real raw binding and native outbound, with the already-ready peer installed before replay. */
public final class ZLinkRelocationRawSessionFixture implements AutoCloseable {
    private final Context context = Zlink.createContext();
    private final ZLinkJavaRawMeshNode node = new ZLinkJavaRawMeshNode(context, "mesh");
    private final ZLinkJavaRawServicePort peerPort = new ZLinkJavaRawServicePort(context);
    private RouterSocket peer;

    public ZLinkRelocationRawSessionFixture() {
        try {
            var localRid = RoutingId.from("target-node");
            var peerRid = RoutingId.from("session-owner");
            node.setRoutingId(localRid);
            var port = (ZLinkJavaRawServicePort) field("port");
            var router = port.openRouter(localRid);
            set("router", router);
            String endpoint = "inproc://aggregate-replay-" + UUID.randomUUID();
            router.bind(endpoint);
            peer = peerPort.openRouter(peerRid);
            peer.connect(endpoint);
            peerPort.send(peer, localRid, List.of(new byte[] {1})).toCompletableFuture().join();
            assertTrue(port.waitForReadable(router, Duration.ofSeconds(1)));
            try (var inbound = port.receiveNow(router)) {
                assertNotNull(inbound);
                assertEquals(peerRid, inbound.source());
            }
            var topology = new ZLinkServiceTopologyRegistry(descriptor(localRid, 17));
            topology.admit(descriptor(peerRid, 31), "ready", ServiceWireConstants.COMMAND_HELLO);
            set("topology", topology);
            @SuppressWarnings("unchecked")
            var ready = (Map<RoutingId, String>) field("admissionControlReadyConnections");
            ready.put(peerRid, "ready");
        } catch (ReflectiveOperationException | RuntimeException | Error failure) {
            close();
            throw new AssertionError(failure);
        }
    }

    public ZLinkInternalMeshNode node() {
        return node;
    }

    public void assertFirstPush() {
        assertTrue(peerPort.waitForReadable(peer, Duration.ofSeconds(1)));
        try (var inbound = peerPort.receiveNow(peer)) {
            assertNotNull(inbound);
            assertEquals(2, inbound.frames().size());
            var sent =
                    new ZLinkServiceM6BWireCodec()
                            .decodeBoundSessionSendHeader(inbound.frames().getFirst());
            assertEquals("actor-a", sent.actor().actor().actorId());
            assertEquals(RoutingId.from("target-node"), sent.actor().actor().nodeRid());
            assertEquals(4, sent.actor().actor().generation());
            assertEquals(17, sent.actor().targetNodeGeneration());
            assertEquals(8, sent.actor().authorityOwnerGeneration());
            assertEquals(23, sent.actor().ownerLeaseGeneration());
            assertEquals(33, sent.expectedBindingGeneration());
            var parts =
                    ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrame(
                            ByteBuffer.wrap(inbound.frames().getLast()));
            try {
                assertEquals(1, parts.size());
                var frame =
                        ZLinkStreamFrameCodec.tryDecode(parts.getFirst().toByteArray())
                                .orElseThrow();
                assertEquals(
                        "BoundPush",
                        ZLinkStreamHeaderCodec.decodeOrPlain(frame.header()).packetName());
                assertEquals(
                        "{\"value\":\"first-replay\"}",
                        new String(frame.body(), StandardCharsets.UTF_8));
            } finally {
                parts.forEach(Message::close);
            }
        }
    }

    @Override
    public void close() {
        if (peer != null) peer.close();
        peerPort.close();
        node.close();
        context.close();
    }

    private Object field(String name) throws ReflectiveOperationException {
        var field = ZLinkJavaRawMeshNode.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(node);
    }

    private void set(String name, Object value) throws ReflectiveOperationException {
        var field = ZLinkJavaRawMeshNode.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(node, value);
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid, long generation) {
        return new ZLinkServiceNodeDescriptor(
                "mesh",
                rid,
                generation,
                1,
                "inproc://" + rid,
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                1,
                1,
                1,
                0,
                0);
    }
}
