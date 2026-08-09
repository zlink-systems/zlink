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
                    ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
                        .APPLIED,
                    valid.currentAuthorityOwnerGeneration(), 16));
        });

        assertThrows(CompletionException.class, () ->
            new ZLinkSessionRelocationPeerClient(node, codec)
                .switchRoute(command, Duration.ofSeconds(1))
                .toCompletableFuture().join());

        assertEquals(1, submissions.get());
    }

    //  Spec 20 §5: the target stops retransmitting on any of the four
    //  results, so a `Stale` refusal is terminal exactly like `Applied`.
    @Test
    void aStaleResultEndsTheRouteSwitchWithoutRetransmitting() {
        var codec = new ZLinkServiceM6BWireCodec();
        var command = command();
        AtomicInteger submissions = new AtomicInteger();
        ZLinkInternalMeshNode node = node((target, encoded) -> {
            submissions.incrementAndGet();
            var echo = ack(command);
            return codec.encodeSessionRelocationRouted(
                new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
                    echo.relocation(), echo.coordinator(), echo.actor(),
                    echo.session(), echo.action(),
                    ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
                        .STALE,
                    echo.currentAuthorityOwnerGeneration(),
                    echo.lastAcceptedSessionSequence()));
        });
        var terminal = new CompletableFuture<
            ZLinkServiceM6BWireCodec.SessionRelocationRouted>();

        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRouteUntilTerminal(
                command,
                Duration.ofSeconds(1),
                () -> CompletableFuture.completedFuture(false),
                (routed, failure) -> terminal.complete(routed))
            .toCompletableFuture().join();

        assertEquals(
            ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.STALE,
            terminal.join().result());
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
    void aRefusedRouteKeepsRetransmittingUntilTheCallerSaysSuperseded()
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

        //  Command 45 has no result field, so a refused command 44 reaches the
        //  source as an unanswered request. Spec 20 §5 keeps retransmitting
        //  until the caller's own superseded check ends it - the refusal text
        //  is not a terminal signal.
        new ZLinkSessionRelocationPeerClient(node, codec)
            .switchRouteUntilTerminal(
                command,
                Duration.ofSeconds(1),
                () -> {
                    supersededChecks.incrementAndGet();
                    return CompletableFuture.completedFuture(true);
                })
            .toCompletableFuture().join();

        Thread.sleep(120);
        assertEquals(1, submissions.get());
        assertEquals(1, supersededChecks.get(),
            "the caller's superseded check is what ends the retransmission");
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
                () -> CompletableFuture.completedFuture(true),
                (routed, failure) ->
                    terminals.add(routed == null ? "failed" : "switched"))
            .toCompletableFuture().join();

        Thread.sleep(120);
        assertEquals(List.of("failed"), terminals);
    }

    //  Command 42 loss policy is `retransmit-until-sealed-or-deadline`: an
    //  unanswered seal is retransmitted on the spec 20 §5 step 8 schedule and
    //  the deadline - not the caller - stops the loop.
    @Test
    void command42RetransmitsUntilTheSealedAckOrTheDeadline() {
        var codec = new ZLinkServiceM6BWireCodec();
        var seal = seal();
        AtomicInteger submissions = new AtomicInteger();
        ZLinkInternalMeshNode retrying = sealNode((target, encoded) -> {
            assertEquals(SESSION_OWNER, target);
            assertEquals(seal, codec.decodeSessionRelocationSeal(encoded));
            if (submissions.incrementAndGet() < 2) {
                throw new ZlinkRequestException(RequestResult.TIMED_OUT);
            }
            return codec.encodeSessionRelocationSealed(
                new ZLinkServiceM6BWireCodec.SessionRelocationSealed(
                    seal.relocation(), seal.coordinator(), seal.actor(),
                    seal.session(), 23));
        });

        var sealed = new ZLinkSessionRelocationPeerClient(retrying, codec)
            .sealRouteUntilAck(seal, Duration.ofSeconds(10))
            .toCompletableFuture().join();

        assertEquals(23, sealed.lastAcceptedSessionSequence());
        assertEquals(2, submissions.get());

        //  An ACK that does not echo the seal is terminal - retransmitting the
        //  same bytes can never make a different fence match.
        ZLinkInternalMeshNode divergent = sealNode((target, encoded) ->
            codec.encodeSessionRelocationSealed(
                new ZLinkServiceM6BWireCodec.SessionRelocationSealed(
                    seal.relocation(), seal.coordinator(), seal.actor(),
                    new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        SESSION_OWNER, 7, "session-owner", 8, SESSION, 10),
                    23)));
        assertThrows(CompletionException.class, () ->
            new ZLinkSessionRelocationPeerClient(divergent, codec)
                .sealRouteUntilAck(seal, Duration.ofSeconds(2))
                .toCompletableFuture().join());
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationSeal seal() {
        return new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(1, 2),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 3, SOURCE, 4, "store-v5"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(SOURCE, "actor", 6),
                4, 10, 11),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                SESSION_OWNER, 7, "session-owner", 8, SESSION, 9));
    }

    private static ZLinkInternalMeshNode sealNode(Sender sender) {
        return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
            ZLinkSessionRelocationPeerClientTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalMeshNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("requestSessionRelocationSeal")) {
                    try {
                        return CompletableFuture.completedFuture(sender.send(
                            (RoutingId) arguments[0], (byte[]) arguments[1]));
                    } catch (RuntimeException failure) {
                        return CompletableFuture.failedFuture(failure);
                    }
                }
                if (method.getName().equals("toString")) return "peer";
                throw new UnsupportedOperationException(method.getName());
            });
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
            ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED,
            command.currentAuthorityOwnerGeneration(),
            command.lastAcceptedSessionSequence());
    }

    @FunctionalInterface
    private interface Sender {
        byte[] send(RoutingId target, byte[] command);
    }
}
