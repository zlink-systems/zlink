package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;

final class ZLinkJavaRawMeshNodeEndedRouteClaimTest {
    private static final Duration WAIT = Duration.ofSeconds(5);

    @Test
    void deliversBufferedChannelSendAfterTheAdmittedRouteEnds() throws Exception {
        try (var fixture = new EndedRouteFixture()) {
            long ended = fixture.queueAndEndRoute();
            fixture.dispatch(ended);
            assertEquals(List.of("buffered"), fixture.channelPayloads);
        }
    }

    @Test
    void replacementNeedsAdmitAndThenRetiresTheEndedAdmission() throws Exception {
        try (var fixture = new EndedRouteFixture()) {
            long ended = fixture.queueAndEndRoute();
            // Take the records Core still returns before a new selection can discard them.
            try (var beforeAdmit = fixture.receive(ended);
                    var afterAdmit = fixture.receive(ended);
                    var replacement = fixture.connect()) {
                fixture.sendChannel(replacement, "before handshake");
                long selected = fixture.dispatch();
                assertNotEquals(ended, selected);
                assertTrue(fixture.channelPayloads.isEmpty());

                fixture.handshake(replacement, ServiceWireConstants.COMMAND_HELLO);
                assertFalse(fixture.topology.isReady(fixture.sourceRid));
                // The receiver admitted the replacement HELLO, so it processes the record
                // before its own HELLO is answered (mesh-node receive admission).
                fixture.sendChannel(replacement, "before ADMIT");
                fixture.dispatch(selected);
                assertEquals(List.of("before ADMIT"), fixture.channelPayloads);

                fixture.node.dispatch(beforeAdmit);
                assertEquals(List.of("before ADMIT", "buffered"), fixture.channelPayloads);

                fixture.handshake(replacement, ServiceWireConstants.COMMAND_ADMIT);
                assertTrue(fixture.topology.isReady(fixture.sourceRid));
                fixture.node.dispatch(afterAdmit);
                fixture.sendChannel(replacement, "after ADMIT");
                fixture.dispatch(selected);
                assertEquals(
                        List.of("before ADMIT", "buffered", "after ADMIT"),
                        fixture.channelPayloads);
            }
        }
    }

    @Test
    void descriptorWithdrawalRetiresTheEndedAdmission() throws Exception {
        try (var fixture = new EndedRouteFixture()) {
            fixture.observeDescriptor(1);
            long ended = fixture.queueAndEndRoute();
            fixture.dispatch(ended);
            assertEquals(List.of("buffered"), fixture.channelPayloads);

            fixture.node
                    .forgetPeerAdmissionExpectation(fixture.sourceRid)
                    .toCompletableFuture()
                    .join();
            fixture.dispatch(ended);
            assertEquals(List.of("buffered"), fixture.channelPayloads);
        }
    }

    @Test
    void descriptorLifecycleChangeRetiresTheEndedAdmission() throws Exception {
        try (var fixture = new EndedRouteFixture()) {
            fixture.observeDescriptor(1);
            long ended = fixture.queueAndEndRoute();
            fixture.dispatch(ended);
            assertEquals(List.of("buffered"), fixture.channelPayloads);

            fixture.observeDescriptor(2);
            fixture.dispatch(ended);
            assertEquals(List.of("buffered"), fixture.channelPayloads);
        }
    }

    @Test
    void endedRouteCannotReadmitItselfWithABufferedAdmit() throws Exception {
        try (var fixture = new EndedRouteFixture()) {
            long ended = fixture.queueAndEndRoute();
            fixture.dispatch(ended);
            fixture.dispatch(ended);
            fixture.dispatch(ended);
            assertEquals(List.of("buffered", "buffered"), fixture.channelPayloads);
            assertTrue(fixture.topology.peer(fixture.sourceRid).isEmpty());
        }
    }

    private static final class EndedRouteFixture implements AutoCloseable {
        private final String id = UUID.randomUUID().toString();
        private final String mesh = "ended-route-" + id;
        private final String endpoint = "inproc://" + mesh;
        private final RoutingId targetRid = RoutingId.from("target-" + id);
        private final RoutingId sourceRid = RoutingId.from("source-" + id);
        private final ZLinkServiceM6AWireCodec wire = new ZLinkServiceM6AWireCodec();
        private final List<String> channelPayloads = new ArrayList<>();
        private final Context context = Zlink.createContext();
        private final ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort(context);
        private final RouterSocket target = port.openRouter(targetRid, true);
        private final ZLinkServiceNodeDescriptor local = descriptor(targetRid);
        private final ZLinkServiceTopologyRegistry topology =
                new ZLinkServiceTopologyRegistry(local, port.stateLane());
        private final ZLinkJavaRawMeshNode node =
                ZLinkJavaRawMeshNodeTesting.create(
                        mesh, port, target, local, topology, Map.of(), Runnable::run);

        EndedRouteFixture() {
            target.bind(endpoint);
            node.startDispatch(
                    record -> {
                        try (record) {
                            assertEquals(RecordKind.CHANNEL_SEND, record.receive().kind());
                            assertEquals("orders", record.receive().channelName());
                            channelPayloads.add(record.parts().getFirst().toUtf8String());
                        }
                    });
        }

        DealerSocket connect() {
            var source = context.createDealerSocket();
            try {
                source.setRoutingId(sourceRid);
                source.connect(endpoint);
                return source;
            } catch (RuntimeException | Error failure) {
                source.close();
                throw failure;
            }
        }

        long queueAndEndRoute() {
            long generation;
            try (var source = connect()) {
                generation = handshake(source, ServiceWireConstants.COMMAND_ADMIT);
                assertTrue(topology.isReady(sourceRid));
                assertEquals(generation, port.routesSnapshot(target).getFirst().routeGeneration());
                sendChannel(source, "buffered");
                sendChannel(source, "buffered");
                sendAdmission(source, ServiceWireConstants.COMMAND_ADMIT);
                awaitReadable();
            }
            long deadline = System.nanoTime() + WAIT.toNanos();
            while (!port.routesSnapshot(target).isEmpty()) {
                assertTrue(System.nanoTime() < deadline, "Core did not observe route loss");
                port.waitForReady(target, Duration.ZERO);
            }
            node.observeTransportState(ZLinkJavaSocketReceivePoller.ROUTE_CHANGED);
            assertTrue(topology.peer(sourceRid).isEmpty());
            return generation;
        }

        long handshake(DealerSocket source, int command) {
            sendAdmission(source, command);
            return dispatch();
        }

        private void sendAdmission(DealerSocket source, int command) {
            send(source, List.of(wire.encodeAdmission(command, descriptor(sourceRid))));
        }

        void sendChannel(DealerSocket source, String payload) {
            try (var message = Message.from(payload)) {
                send(
                        source,
                        List.of(
                                wire.encodeChannelSendHeader("orders", 0),
                                ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(
                                        List.of(message))));
            }
        }

        void observeDescriptor(long lifecycle) {
            node.observePeerAdmissionExpectation(
                            sourceRid,
                            descriptor(sourceRid).advertisedEndpoint(),
                            lifecycle,
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            ZLinkMeshNodeObjectRole.NONE)
                    .toCompletableFuture()
                    .join();
        }

        private void awaitReadable() {
            long deadline = System.nanoTime() + WAIT.toNanos();
            int ready;
            do {
                long remaining = deadline - System.nanoTime();
                assertTrue(remaining > 0, "Expected Core to return a queued record");
                ready = port.waitForReady(target, Duration.ofNanos(remaining));
                if ((ready
                                & (ZLinkJavaSocketReceivePoller.ROUTE_CHANGED
                                        | ZLinkJavaSocketReceivePoller.CONNECTION_CHANGED))
                        != 0) {
                    node.observeTransportState(ready);
                }
            } while ((ready & ZLinkJavaSocketReceivePoller.READABLE) == 0);
        }

        private ZLinkJavaRawServicePort.Inbound receive() {
            awaitReadable();
            var inbound = port.receiveNow(target);
            assertNotNull(inbound);
            return inbound;
        }

        ZLinkJavaRawServicePort.Inbound receive(long generation) {
            var inbound = receive();
            long actual = inbound.routeGeneration();
            if (actual != generation) {
                inbound.close();
                assertEquals(generation, actual);
            }
            return inbound;
        }

        long dispatch() {
            var inbound = receive();
            long generation = inbound.routeGeneration();
            node.dispatch(inbound);
            return generation;
        }

        void dispatch(long generation) {
            node.dispatch(receive(generation));
        }

        private static void send(DealerSocket socket, List<byte[]> record) {
            List<Message> frames = record.stream().map(Message::from).toList();
            try {
                var operation = socket.send().message(frames.getFirst());
                for (int index = 1; index < frames.size(); index++) {
                    operation.message(frames.get(index));
                }
                operation.submit_sync();
            } finally {
                frames.forEach(Message::close);
            }
        }

        private ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
            return new ZLinkServiceNodeDescriptor(
                    mesh,
                    rid,
                    1,
                    1,
                    "inproc://" + rid,
                    List.of(),
                    ZLinkServiceNodeDescriptor.State.SERVING,
                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                    1,
                    List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                    ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                    1,
                    1,
                    0,
                    0,
                    0);
        }

        @Override
        public void close() {
            try (context;
                    port;
                    node) {
                // Close the node and its socket before the context.
            }
        }
    }
}
