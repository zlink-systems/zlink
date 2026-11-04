package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;

final class BoundSessionRelayOrderTest {
    @ParameterizedTest
    @ValueSource(strings = {"fifo", "executor", "authority"})
    void relaysKeepArrivalOrderBeforeSequenceAcceptance(String schedule) throws Exception {
        try (var fixture = new Fixture(schedule.equals("authority"))) {
            fixture.send(1, 1);
            fixture.send(2, 1);
            if (schedule.equals("authority")) {
                assertEquals(2, fixture.resolutions.size());
                assertTrue(fixture.tasks.isEmpty());
                fixture.resolutions.get(1).complete(fixture.authority());
                fixture.resolutions.get(0).complete(fixture.authority());
            }
            fixture.drain(schedule.equals("executor"));
            assertEquals(List.of("1", "2"), fixture.delivered);
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
            assertTrue(fixture.tails.stream().allMatch(CompletableFuture::isDone));
        }
    }

    @Test
    void duplicateAndLowerSequencesRemainStale() throws Exception {
        try (var fixture = new Fixture(false)) {
            fixture.send(1, 1);
            fixture.send(2, 1);
            fixture.send(2, 1);
            fixture.send(1, 1);
            fixture.drain(false);
            assertEquals(List.of("1", "2"), fixture.delivered);
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
        }
    }

    @Test
    void authorityFailureReleasesTheNextRelay() throws Exception {
        try (var fixture = new Fixture(true)) {
            fixture.send(1, 1);
            fixture.send(2, 1);
            fixture.resolutions.get(1).complete(fixture.authority());
            fixture.resolutions
                    .get(0)
                    .completeExceptionally(new IllegalStateException("authority failed"));
            fixture.drain(false);
            assertEquals(List.of("2"), fixture.delivered);
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
            assertTrue(fixture.tails.stream().allMatch(CompletableFuture::isDone));
        }
    }

    @ParameterizedTest
    @ValueSource(
            strings = {
                "actor",
                "binding",
                "shutdown",
                "spot",
                "queuedActor",
                "queuedBinding",
                "queuedShutdown",
                "queuedSpot"
            })
    void retirementFinishesEveryPendingTailWithoutAuthorityCompletion(String retirement)
            throws Exception {
        try (var fixture = new Fixture(!retirement.startsWith("queued"))) {
            fixture.send(1, 1);
            fixture.send(2, 1);
            switch (retirement.replace("queued", "").toLowerCase(java.util.Locale.ROOT)) {
                case "actor" ->
                        fixture.spots
                                .destroyActor(fixture.actor, Duration.ofSeconds(1))
                                .toCompletableFuture()
                                .join();
                case "binding" -> fixture.bind(2);
                case "shutdown" -> fixture.node.close();
                case "spot" -> fixture.spots.close();
                default -> throw new AssertionError(retirement);
            }
            assertTrue(fixture.resolutions.stream().noneMatch(CompletableFuture::isDone));
            assertTrue(
                    fixture.tails.stream().allMatch(CompletableFuture::isCompletedExceptionally));
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
            fixture.drain(true);
            assertTrue(fixture.delivered.isEmpty());
            fixture.resolutions.forEach(resolution -> resolution.complete(fixture.authority()));
            fixture.drain(false);
            assertTrue(fixture.delivered.isEmpty());
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
        }
    }

    @Test
    void replacedBindingRejectsOldRelaysAndAcceptsNewBindingSequence() throws Exception {
        try (var fixture = new Fixture(false)) {
            fixture.send(1, 1);
            fixture.bind(2);
            fixture.send(2, 1);
            fixture.send(1, 2);
            fixture.drain(true);
            assertEquals(List.of("1"), fixture.delivered);
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
        }
    }

    @Test
    void actorCloseAlsoFinishesRelaysReceivedAfterBindingRetirement() throws Exception {
        try (var fixture = new Fixture(true)) {
            assertTrue(
                    fixture.spots
                            .applyRemoteStreamBinding(
                                    fixture.source,
                                    1,
                                    "owner",
                                    1,
                                    new ZLinkServiceM6BWireCodec.BoundSessionBind(
                                            1, fixture.route, fixture.session, false, 1),
                                    ignored -> {})
                            .toCompletableFuture()
                            .join());
            fixture.send(1, 1);
            assertFalse(fixture.tails.isEmpty());
            fixture.spots
                    .destroyActor(fixture.actor, Duration.ofSeconds(1))
                    .toCompletableFuture()
                    .join();
            assertTrue(
                    fixture.tails.stream().allMatch(CompletableFuture::isCompletedExceptionally));
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
            fixture.drain(false);
            assertTrue(fixture.delivered.isEmpty());
        }
    }

    @Test
    void executorRejectionReleasesTheTailAndAllowsTheNextRelay() throws Exception {
        try (var fixture = new Fixture(false)) {
            fixture.rejectDispatch = true;
            fixture.send(1, 1);
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
            assertFalse(fixture.tails.isEmpty());
            assertTrue(fixture.tails.stream().allMatch(CompletableFuture::isDone));
            fixture.rejectDispatch = false;
            fixture.send(2, 1);
            fixture.drain(false);
            assertEquals(List.of("2"), fixture.delivered);
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
        }
    }

    @Test
    void deepTailRetirementSettlesEveryPendingRelay() throws Exception {
        try (var fixture = new Fixture(true)) {
            for (int sequence = 1; sequence <= 4096; sequence++) fixture.send(sequence, 1);
            fixture.spots
                    .destroyActor(fixture.actor, Duration.ofSeconds(1))
                    .toCompletableFuture()
                    .join();
            assertEquals(4096, fixture.tails.size());
            assertTrue(
                    fixture.tails.stream().allMatch(CompletableFuture::isCompletedExceptionally));
            assertTrue(fixture.node.orderedActorRelayTails.isEmpty());
            fixture.drain(true);
            assertTrue(fixture.delivered.isEmpty());
        }
    }

    private static final class Fixture implements AutoCloseable {
        final RoutingId source = RoutingId.from("relay-source");
        final RoutingId target = RoutingId.from("relay-target");
        final RoutingId session = RoutingId.from("relay-session");
        final ArrayDeque<Runnable> tasks = new ArrayDeque<>();
        final List<CompletableFuture<Optional<ZLinkInternalMeshNode.PeerAuthorityFence>>>
                resolutions = new ArrayList<>();
        final List<CompletableFuture<Void>> tails = new ArrayList<>();
        final List<String> delivered = new ArrayList<>();
        final systems.zlink.contracts.core.Context context = Zlink.createContext();
        final ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort(context);
        final systems.zlink.contracts.sockets.RouterSocket router = port.openRouter(target);
        boolean rejectDispatch;
        final ZLinkJavaRawMeshNode node;
        final ZLinkJavaRawSpotNode spots;
        final ZLinkBackendActorRef actor;
        final ZLinkServiceM6BWireCodec.ActorRouteFence route;

        Fixture(boolean deferAuthority) throws Exception {
            var local = descriptor(target);
            var topology = new ZLinkServiceTopologyRegistry(local);
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    topology.admit(descriptor(source), "0", ServiceWireConstants.COMMAND_HELLO));
            node =
                    ZLinkJavaRawMeshNodeTesting.create(
                            "relay", port, router, local, topology, Map.of(), this::execute);
            node.setRoutingId(target);
            node.setPeerAuthorityResolver(
                    (mesh, rid, generation) -> {
                        if (deferAuthority && rid.equals(source)) {
                            var pending =
                                    new CompletableFuture<
                                            Optional<ZLinkInternalMeshNode.PeerAuthorityFence>>();
                            resolutions.add(pending);
                            return pending;
                        }
                        return CompletableFuture.completedFuture(
                                Optional.of(
                                        new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                rid, generation, "owner", 1)));
                    });
            spots = (ZLinkJavaRawSpotNode) node.spotNode();
            try (var create = Message.from("create")) {
                actor = spots.createActor("actor", create);
            }
            spots.rememberActorAuthority(actor, 1, 1);
            route = new ZLinkServiceM6BWireCodec.ActorRouteFence(actor, 1, 1, 1);
            bind(1);
            spots.entrySpot()
                    .onDispatchEvent(
                            info -> {
                                if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                                    delivered.add(
                                            info.actorMessages().get(1).message().toUtf8String());
                                    info.actorMessages().forEach(ZLinkBackendActorReceived::close);
                                }
                            });
        }

        void bind(long generation) {
            assertTrue(
                    spots.applyRemoteStreamBinding(
                                    source,
                                    1,
                                    "owner",
                                    1,
                                    new ZLinkServiceM6BWireCodec.BoundSessionBind(
                                            generation, route, session, true, generation),
                                    ignored -> {})
                            .toCompletableFuture()
                            .join());
        }

        void send(long sequence, long generation) {
            var actorWire = new ZLinkServiceM6BWireCodec();
            var wire = new ZLinkServiceM6AWireCodec();
            byte[] head =
                    actorWire.encodeActorHeader(
                            false,
                            ServiceWireConstants.FLAG_BOUND_SESSION
                                    | ServiceWireConstants.FLAG_SOURCE_SPOT_ID,
                            null,
                            1,
                            sequence,
                            0,
                            null,
                            route,
                            new ZLinkServiceM6BWireCodec.BoundSessionTail(
                                    session, generation, sequence));
            byte[] payload;
            try (var packet = Message.from("MoveRequest");
                    var body = Message.from(Long.toString(sequence))) {
                payload = wire.encodeFrameworkMultipartFrame(List.of(packet, body));
            }
            var received =
                    new Received(source, new Message[] {Message.from(head), Message.from(payload)});
            node.dispatch(new ZLinkJavaRawServicePort.Inbound(source, null, received));
            tails.addAll(node.orderedActorRelayTails.values());
        }

        Optional<ZLinkInternalMeshNode.PeerAuthorityFence> authority() {
            return Optional.of(new ZLinkInternalMeshNode.PeerAuthorityFence(source, 1, "owner", 1));
        }

        void execute(Runnable task) {
            if (rejectDispatch) {
                tails.addAll(node.orderedActorRelayTails.values());
                throw new java.util.concurrent.RejectedExecutionException("controlled rejection");
            }
            tasks.addLast(task);
        }

        void drain(boolean reverse) {
            while (!tasks.isEmpty()) {
                if (reverse) tasks.removeLast().run();
                else tasks.removeFirst().run();
            }
        }

        @Override
        public void close() {
            node.close();
            drain(false);
            context.close();
        }
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
        return new ZLinkServiceNodeDescriptor(
                "relay",
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
}
