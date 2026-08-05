package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

final class ZLinkSessionRelocationPeerClientTest {
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");
    private static final RoutingId SESSION_OWNER = RoutingId.from("session-owner");
    private static final RoutingId SESSION = RoutingId.from("session");

    @Test
    void opensAdmissionOnlyAfterExactAckAndSteadyNormalization() {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        List<String> events = new ArrayList<>();
        AtomicInteger submissions = new AtomicInteger();
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            submissions.incrementAndGet();
            assertEquals(SESSION_OWNER, target);
            assertEquals(command, codec.decodeSessionRelocationRoute(encoded));
            events.add("ack");
            return codec.encodeSessionRelocationRouted(ack(command));
        });
        var gate = new Gate(events);

        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRouteThenOpenAdmission(
                command,
                Duration.ofSeconds(1),
                (sent, received) -> {
                    events.add("steady");
                    return CompletableFuture.completedFuture(null);
                },
                gate)
            .toCompletableFuture().join();

        assertEquals(List.of("closed", "ack", "steady", "open"), events);
        assertEquals(1, submissions.get());
        assertTrue(gate.open);
    }

    @Test
    void mismatchedAckDoesNotNormalizeOpenOrRetry() {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger submissions = new AtomicInteger();
        AtomicInteger normalizations = new AtomicInteger();
        Gate gate = new Gate(new ArrayList<>());
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            submissions.incrementAndGet();
            var valid = ack(command);
            return codec.encodeSessionRelocationRouted(
                new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
                    valid.relocation(), valid.coordinator(), valid.actor(),
                    valid.session(), valid.action(),
                    valid.currentAuthorityOwnerGeneration(), 16));
        });

        assertThrows(CompletionException.class, () ->
            new ZLinkSessionRelocationPeerClient(node, codec)
                .switchRouteThenOpenAdmission(
                    command,
                    Duration.ofSeconds(1),
                    (sent, received) -> {
                        normalizations.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    },
                    gate)
                .toCompletableFuture().join());

        assertEquals(1, submissions.get());
        assertEquals(0, normalizations.get());
        assertFalse(gate.open);
    }

    @Test
    void retriesOnlyTransportNotConnectedBeforeTheRouteAck() {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger submissions = new AtomicInteger();
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            assertEquals(SESSION_OWNER, target);
            assertEquals(command, codec.decodeSessionRelocationRoute(encoded));
            if (submissions.incrementAndGet() == 1) {
                throw new CompletionException(
                    new ZlinkRequestException(RequestResult.NOT_CONNECTED));
            }
            return codec.encodeSessionRelocationRouted(ack(command));
        });

        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRoute(command, Duration.ofSeconds(1))
            .toCompletableFuture()
            .join();

        assertEquals(2, submissions.get());
    }

    private static ZLinkInternalMeshNode node(Sender sender) {
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkSessionRelocationPeerClientTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("requestSessionRelocationRoute")) {
                    return CompletableFuture.completedFuture(sender.send(
                        (RoutingId) arguments[0], (byte[]) arguments[1]));
                }
                if (method.getName().equals("toString")) return "peer";
                throw new UnsupportedOperationException(method.getName());
            });
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute command() {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(1, 2),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 3, SOURCE, 4, "store-v5"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor", 6),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                SESSION_OWNER, 7, "session-owner", 8, SESSION, 9),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            10, 11, TARGET, 12, 17);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRouted ack(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
            command.relocation(), command.coordinator(), command.actor(),
            command.session(), command.action(),
            command.currentAuthorityOwnerGeneration(),
            command.lastAcceptedSessionSequence());
    }

    private static final class Gate
        implements ZLinkSessionRelocationPeerClient.AdmissionGate {
        private final List<String> events;
        private boolean open;

        private Gate(List<String> events) { this.events = events; }

        @Override public void close() { open = false; events.add("closed"); }
        @Override public void open() { open = true; events.add("open"); }
    }

    @FunctionalInterface
    private interface Sender {
        byte[] send(RoutingId target, byte[] command);
    }
}
