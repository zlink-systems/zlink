package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

final class ZLinkSessionRelocationPeerClientTest {
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");
    private static final RoutingId SESSION_OWNER = RoutingId.from("session-owner");
    private static final RoutingId SESSION = RoutingId.from("session");

    @Test
    void mismatchedAckFailsTheRouteSwitchWithoutRetry() {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger submissions = new AtomicInteger();
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
                .switchRoute(command, Duration.ofSeconds(1))
                .toCompletableFuture().join());

        assertEquals(1, submissions.get());
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

    @Test
    void staleBindingRejectionIsATerminalNoOpWithoutRetry()
        throws InterruptedException {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger submissions = new AtomicInteger();
        AtomicInteger supersededChecks = new AtomicInteger();
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            submissions.incrementAndGet();
            throw new CompletionException(new ZLinkConfigurationException(
                "Session relocation route command has a stale binding fence"));
        });

        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRouteUntilTerminal(
                command,
                Duration.ofSeconds(1),
                () -> {
                    supersededChecks.incrementAndGet();
                    return CompletableFuture.completedFuture(false);
                })
            .toCompletableFuture().join();

        Thread.sleep(120);
        assertEquals(1, submissions.get(),
            "a superseded binding never retries");
        assertEquals(0, supersededChecks.get(),
            "the rejection itself is the terminal proof");
    }

    @Test
    void supersededStoreProofStopsTheBackgroundRetry()
        throws InterruptedException {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger submissions = new AtomicInteger();
        AtomicInteger supersededChecks = new AtomicInteger();
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            submissions.incrementAndGet();
            throw new CompletionException(
                new IllegalStateException("route switch failed"));
        });

        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRouteUntilTerminal(
                command,
                Duration.ofSeconds(1),
                () -> CompletableFuture.completedFuture(
                    supersededChecks.incrementAndGet() >= 2))
            .toCompletableFuture().join();

        //  Spec 20 §5 step 8: the first retransmission is one second after the
        //  first send, so the second submission cannot be observed earlier.
        Thread.sleep(1400);
        assertEquals(2, submissions.get(),
            "the retry stops once the store proves supersession");
        assertEquals(2, supersededChecks.get());
    }

    @Test
    void routeFailureSettlesFinalizeAndConvergesInTheBackground()
        throws InterruptedException {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger submissions = new AtomicInteger();
        CountDownLatch converged = new CountDownLatch(1);
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            if (submissions.incrementAndGet() == 1) {
                throw new CompletionException(
                    new IllegalStateException("route switch failed"));
            }
            converged.countDown();
            return codec.encodeSessionRelocationRouted(ack(command));
        });

        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRouteUntilTerminal(
                command,
                Duration.ofSeconds(1),
                () -> CompletableFuture.completedFuture(false))
            .toCompletableFuture().join();

        //  The spec retransmission schedule puts the second send one second
        //  after the first (spec 20 §5 step 8).
        assertTrue(converged.await(3, TimeUnit.SECONDS),
            "the failed route keeps retrying after finalize settles");
        assertEquals(2, submissions.get());
    }

    @Test
    void onlyTheCommand45AckReportsTheRouteAsSwitched()
        throws InterruptedException {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger submissions = new AtomicInteger();
        List<String> terminals = new CopyOnWriteArrayList<>();
        CountDownLatch reported = new CountDownLatch(1);
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            if (submissions.incrementAndGet() == 1) {
                throw new CompletionException(
                    new IllegalStateException("route switch failed"));
            }
            return codec.encodeSessionRelocationRouted(ack(command));
        });

        //  The returned stage settles after the first attempt whatever the
        //  outcome, so it can never mean "switched". Only the terminal
        //  callback may report that, and only when a command 45 ACK arrived.
        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRouteUntilTerminal(
                command,
                Duration.ofSeconds(1),
                () -> CompletableFuture.completedFuture(false),
                (routed, failure) -> {
                    terminals.add(routed == null ? "failed" : "switched");
                    reported.countDown();
                })
            .toCompletableFuture().join();

        assertTrue(terminals.isEmpty(),
            "a retrying route has not switched yet");
        assertTrue(reported.await(3, TimeUnit.SECONDS));
        Thread.sleep(120);
        assertEquals(List.of("switched"), terminals);
    }

    @Test
    void aSupersededRouteReportsATerminalFailureInsteadOfASwitch()
        throws InterruptedException {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        List<String> terminals = new CopyOnWriteArrayList<>();
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            throw new CompletionException(new ZLinkConfigurationException(
                "Session relocation route command has a stale binding fence"));
        });

        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRouteUntilTerminal(
                command,
                Duration.ofSeconds(1),
                () -> CompletableFuture.completedFuture(false),
                (routed, failure) ->
                    terminals.add(routed == null ? "failed" : "switched"))
            .toCompletableFuture().join();

        Thread.sleep(120);
        assertEquals(List.of("failed"), terminals);
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

    @FunctionalInterface
    private interface Sender {
        byte[] send(RoutingId target, byte[] command);
    }
}
