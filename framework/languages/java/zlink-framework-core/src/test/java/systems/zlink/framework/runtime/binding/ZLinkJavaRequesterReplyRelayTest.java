package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.PeerAuthorityFence;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.CoordinatorFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.Operation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.RelocationId;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.ReplyRelay;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.RequestSourceFence;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaRequesterReplyRelayTest {
    private static final Duration WAIT = Duration.ofSeconds(10);
    private static final long ROUTE = 33;
    private static final long LEASE = 7;
    private static final String OWNER = "owner";
    private static final ZLinkServiceRelocationWireCodec CODEC =
            new ZLinkServiceRelocationWireCodec();

    @Test
    void remoteRequesterReceivesFirstTerminalAndAcknowledgesDuplicates() throws Exception {
        try (var mesh = new Mesh()) {
            var request = mesh.register();
            var relay = mesh.relay(request.id(), ROUTE, mesh.coordinator());
            assertEquals(1, CODEC.decodeReplyRelayAck(await(mesh.send(relay, WAIT))).status());
            assertTrue(request.isDone(), "requester must complete before the target receives ACK");
            assertEquals(42, await(request));
            assertEquals(2, CODEC.decodeReplyRelayAck(await(mesh.send(relay, WAIT))).status());
        }
    }

    @Test
    void normallyCompletedRequestAcknowledgesAnAlreadyTerminalRelay() throws Exception {
        try (var mesh = new Mesh()) {
            var request = mesh.register();
            mesh.requester.operationRegistry().complete(request.id(), 17);
            assertEquals(17, await(request));
            var relay = mesh.relay(request.id(), ROUTE, mesh.coordinator());
            assertEquals(2, CODEC.decodeReplyRelayAck(await(mesh.send(relay, WAIT))).status());
            assertEquals(17, await(request));
        }
    }

    @Test
    void unknownOperationAndWrongReplyRouteGetNoAck() throws Exception {
        try (var mesh = new Mesh()) {
            var request = mesh.register();
            assertNoAck(
                    mesh.send(
                            mesh.relay(UUID.randomUUID(), ROUTE, mesh.coordinator()),
                            Duration.ofMillis(200)));
            assertNoAck(
                    mesh.send(
                            mesh.relay(request.id(), ROUTE + 1, mesh.coordinator()),
                            Duration.ofMillis(200)));
            assertFalse(request.isDone());
        }
    }

    @ParameterizedTest
    @ValueSource(strings = {"rid", "generation", "owner", "lease"})
    void coordinatorMustMatchAdmittedPeerFence(String mismatch) throws Exception {
        try (var mesh = new Mesh()) {
            var request = mesh.register();
            var expected = mesh.coordinator();
            var invalid =
                    new CoordinatorFence(
                            mismatch.equals("owner") ? "another-owner" : expected.ownerId(),
                            mismatch.equals("lease")
                                    ? expected.leaseGeneration() + 1
                                    : expected.leaseGeneration(),
                            mismatch.equals("rid")
                                    ? mesh.relocationSource.routingId()
                                    : expected.nodeRid(),
                            mismatch.equals("generation")
                                    ? expected.nodeGeneration() ^ Long.MIN_VALUE
                                    : expected.nodeGeneration(),
                            expected.expectedAuthorityStoreVersion());
            assertNoAck(
                    mesh.send(mesh.relay(request.id(), ROUTE, invalid), Duration.ofMillis(200)));
            assertFalse(request.isDone());
            assertEquals(
                    1,
                    CODEC.decodeReplyRelayAck(
                                    await(
                                            mesh.send(
                                                    mesh.relay(request.id(), ROUTE, expected),
                                                    WAIT)))
                            .status());
            assertEquals(42, await(request));
        }
    }

    @Test
    void ackMustMatchExpectedRequesterLease() throws Exception {
        try (var mesh = new Mesh()) {
            var request = mesh.register();
            var relay = mesh.relay(request.id(), ROUTE, mesh.coordinator());
            var frames = frames(relay);
            var expected = mesh.fence();
            var stale =
                    new RequestSourceFence(
                            expected.ownerId(),
                            expected.leaseGeneration() + 1,
                            expected.nodeRid(),
                            expected.nodeGeneration());
            assertNoAck(
                    mesh.target.requestRelocationReplyRelay(
                            mesh.requester.routingId(),
                            stale,
                            frames.getFirst(),
                            frames.subList(1, frames.size()),
                            Duration.ofMillis(200)));
            assertEquals(42, await(request));
            assertEquals(2, CODEC.decodeReplyRelayAck(await(mesh.send(relay, WAIT))).status());
        }
    }

    @Test
    void targetRejectsADestinationDifferentFromItsExpectedAckSender() throws Exception {
        try (var mesh = new Mesh()) {
            var request = mesh.register();
            var frames = frames(mesh.relay(request.id(), ROUTE, mesh.coordinator()));
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    await(
                                            mesh.target.requestRelocationReplyRelay(
                                                    mesh.relocationSource.routingId(),
                                                    mesh.fence(),
                                                    frames.getFirst(),
                                                    frames.subList(1, frames.size()),
                                                    WAIT)));
            assertInstanceOf(IllegalArgumentException.class, failure.getCause());
            assertFalse(request.isDone());
        }
    }

    private static void assertNoAck(CompletionStage<byte[]> ack) {
        var error = assertThrows(ExecutionException.class, () -> await(ack));
        assertEquals(
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                assertInstanceOf(ZLinkFrameworkException.class, error.getCause()).kind());
    }

    private static <T> T await(CompletionStage<T> value) throws Exception {
        return value.toCompletableFuture().get(WAIT.toSeconds(), TimeUnit.SECONDS);
    }

    private static List<byte[]> frames(ReplyRelay relay) {
        return CODEC.encodeReplyRelayFrames(
                relay,
                new ApplicationPayloadEnvelopeV1(
                        "reply", "application/octet-stream", new byte[] {42}));
    }

    private static final class Mesh implements AutoCloseable {
        private final systems.zlink.contracts.core.Context context = Zlink.createContext();
        private final ZLinkJavaRawMeshNode requester = node("requester");
        private final ZLinkJavaRawMeshNode relocationSource = node("relocation-source");
        private final ZLinkJavaRawMeshNode target = node("target");

        private Mesh() throws Exception {
            try {
                connect(target, requester);
                connect(relocationSource, requester);
                connect(target, relocationSource);
            } catch (Exception | Error failure) {
                close();
                throw failure;
            }
        }

        private ZLinkJavaRawMeshNode node(String name) {
            var node = new ZLinkJavaRawMeshNode(context, "mesh");
            node.setRoutingId(RoutingId.from(name));
            node.setBind("tcp://127.0.0.1:0");
            node.start();
            node.confirmLocalOwnerFence(
                    new PeerAuthorityFence(
                            node.routingId(), node.lifecycleGeneration(), OWNER, LEASE));
            return node;
        }

        private void connect(ZLinkJavaRawMeshNode first, ZLinkJavaRawMeshNode second)
                throws Exception {
            // The receiver authenticates command 33 from its admitted coordinator.
            // The sender authenticates command 46 from the pending row's requester fence.
            observe(second, first);
            first.connectPeer(second.status().localEndpoint(), second.routingId());
            long deadline = System.nanoTime() + WAIT.toNanos();
            while (!admitted(first, second) || !admitted(second, first)) {
                if (System.nanoTime() - deadline >= 0) {
                    throw new AssertionError("peer admission did not complete");
                }
                Thread.sleep(1);
            }
        }

        private static boolean admitted(ZLinkJavaRawMeshNode node, ZLinkJavaRawMeshNode peer) {
            return node.peers().stream()
                    .anyMatch(
                            value ->
                                    value.routingId().equals(peer.routingId())
                                            && value.state() == MeshPeerState.ADMITTED);
        }

        private static void observe(ZLinkJavaRawMeshNode node, ZLinkJavaRawMeshNode peer)
                throws Exception {
            String endpoint = peer.status().localEndpoint();
            await(
                    node.observePeerAdmissionExpectation(
                            peer.routingId(),
                            endpoint,
                            peer.lifecycleGeneration(),
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            ZLinkMeshNodeObjectRole.SERVER));
            await(
                    node.refreshSessionFences(
                            List.of(
                                    new ZLinkAutoConnectPeer(
                                            ZLinkAutoConnectType.ROUTE_MESH,
                                            "mesh",
                                            peer.routingId(),
                                            ZLinkLocationRole.ROUTER,
                                            endpoint,
                                            1,
                                            false,
                                            peer.lifecycleGeneration(),
                                            Map.of(),
                                            List.of(),
                                            OWNER,
                                            LEASE,
                                            Instant.now())),
                            null));
        }

        private ZLinkServiceOperationRegistry.Operation<Integer> register() throws Exception {
            var registry = requester.operationRegistry();
            var operation =
                    ZLinkServiceOperationRegistry.<Integer>prepareOperation(UUID.randomUUID());
            await(
                    registry.submitTerminal(
                            operation.id(),
                            WAIT,
                            terminal -> {
                                requester.registerRelocationReply(
                                        registry,
                                        operation,
                                        ROUTE,
                                        reply -> Byte.toUnsignedInt(reply.payload().payload()[0]));
                                return new CompletableFuture<>();
                            },
                            ignored -> {},
                            operation));
            return operation;
        }

        private CoordinatorFence coordinator() {
            return new CoordinatorFence(
                    OWNER, LEASE, target.routingId(), target.lifecycleGeneration(), "initial");
        }

        private RequestSourceFence fence() {
            return new RequestSourceFence(
                    OWNER, LEASE, requester.routingId(), requester.lifecycleGeneration());
        }

        private ReplyRelay relay(UUID operation, long replyRoute, CoordinatorFence coordinator) {
            return new ReplyRelay(
                    new Operation(
                            operation.getMostSignificantBits(),
                            operation.getLeastSignificantBits()),
                    replyRoute,
                    new RelocationId(4, 5),
                    6,
                    coordinator,
                    1,
                    1,
                    0,
                    0);
        }

        private CompletionStage<byte[]> send(ReplyRelay relay, Duration timeout) {
            var frames = frames(relay);
            return target.requestRelocationReplyRelay(
                    requester.routingId(),
                    fence(),
                    frames.getFirst(),
                    frames.subList(1, frames.size()),
                    timeout);
        }

        @Override
        public void close() {
            try (var shared = context;
                    var requesting = requester;
                    var source = relocationSource;
                    var receiving = target) {
                // Resources close in reverse creation order.
            }
        }
    }
}
