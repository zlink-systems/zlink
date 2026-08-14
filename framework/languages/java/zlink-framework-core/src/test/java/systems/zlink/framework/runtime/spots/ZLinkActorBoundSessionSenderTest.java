package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertFalse;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

final class ZLinkActorBoundSessionSenderTest {
    @Test
    void missingLocalBindingKeepsLogicalRouteReevaluation()
        throws Exception {
        AtomicBoolean localRoute = new AtomicBoolean();
        AtomicInteger routeChecks = new AtomicInteger();
        AtomicInteger asynchronousSubmissions = new AtomicInteger();
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "hasRemoteActorBoundSessionRoute" -> false;
                case "hasLocalActorBoundSessionRoute" -> {
                    int check = routeChecks.incrementAndGet();
                    yield check > 1 && localRoute.get();
                }
                case "sendLocalActorBoundSessionAsync" -> {
                    asynchronousSubmissions.incrementAndGet();
                    yield CompletableFuture.completedFuture(null);
                }
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
        ZLinkActorBoundSessionSender sender = new ZLinkActorBoundSessionSender(
            Duration.ofSeconds(1),
            () -> false,
            ignored -> { });

        CompletionStage<Void> submitted = sender.send(
            node,
            new ZLinkBackendActorRef(
                RoutingId.from("logical-route-node"),
                "logical-route-actor",
                1),
            "logical-route-actor",
            new byte[] {1, 2, 3},
            "bound reply failed");
        long firstCheckDeadline =
            System.nanoTime() + Duration.ofSeconds(1).toNanos();
        while (routeChecks.get() == 0
            && System.nanoTime() < firstCheckDeadline) {
            Thread.onSpinWait();
        }
        localRoute.set(true);

        submitted.toCompletableFuture().get(1, TimeUnit.SECONDS);

        assertEquals(1, asynchronousSubmissions.get());
        assertEquals(2, routeChecks.get());
    }

    @Test
    void localStreamRouteDelegatesOnceToTheBindingAdmissionTerminal()
        throws Exception {
        AtomicInteger synchronousSubmissions = new AtomicInteger();
        AtomicInteger asynchronousSubmissions = new AtomicInteger();
        AtomicBoolean closing = new AtomicBoolean();
        CompletableFuture<Void> admissionTerminal = new CompletableFuture<>();
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "hasRemoteActorBoundSessionRoute" -> false;
                case "hasLocalActorBoundSessionRoute" -> true;
                case "sendLocalActorBoundSession" -> {
                    synchronousSubmissions.incrementAndGet();
                    yield false;
                }
                case "sendLocalActorBoundSessionAsync" -> {
                    asynchronousSubmissions.incrementAndGet();
                    yield admissionTerminal;
                }
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
        ZLinkActorBoundSessionSender sender = new ZLinkActorBoundSessionSender(
            Duration.ofSeconds(1),
            closing::get,
            ignored -> { });

        CompletionStage<Void> submitted = sender.send(
            node,
            new ZLinkBackendActorRef(
                RoutingId.from("local-actor-node"),
                "local-actor",
                1),
            "local-actor",
            new byte[] {1, 2, 3},
            "bound reply failed");
        try {
            long evidenceDeadline =
                System.nanoTime() + Duration.ofMillis(400).toNanos();
            while (asynchronousSubmissions.get() == 0
                && synchronousSubmissions.get() < 2
                && System.nanoTime() < evidenceDeadline) {
                Thread.onSpinWait();
            }
            assertAll(
                () -> assertEquals(1, asynchronousSubmissions.get()),
                () -> assertEquals(0, synchronousSubmissions.get()),
                () -> assertFalse(submitted.toCompletableFuture().isDone()));
        } finally {
            closing.set(true);
            admissionTerminal.complete(null);
            submitted.toCompletableFuture().get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void successfulTransportSubmissionCompletesWithoutRetry() throws Exception {
        AtomicInteger submissions = new AtomicInteger();
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals(
                        "hasRemoteActorBoundSessionRoute")) {
                    return true;
                }
                if (method.getName().equals(
                        "sendRemoteActorBoundSession")) {
                    submissions.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                }
                throw new UnsupportedOperationException(method.getName());
            });
        ZLinkActorBoundSessionSender sender = new ZLinkActorBoundSessionSender(
            Duration.ofSeconds(1),
            () -> false,
            ignored -> { });

        sender.send(
                node,
                new ZLinkBackendActorRef(
                    RoutingId.from("actor-node"),
                    "actor-1",
                    1),
                "actor-1",
                new byte[] {1, 2, 3},
                "bound reply failed")
            .toCompletableFuture()
            .get(1, TimeUnit.SECONDS);

        assertEquals(1, submissions.get());
    }
}
