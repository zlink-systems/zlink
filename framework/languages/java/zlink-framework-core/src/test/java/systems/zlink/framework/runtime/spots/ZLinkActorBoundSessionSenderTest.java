package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkActorBoundSessionSenderTest {
    @Test
    void missingCurrentBindingFailsWithInvalidOperation() throws Exception {
        AtomicInteger routeChecks = new AtomicInteger();
        AtomicInteger submissions = new AtomicInteger();
        ZLinkInternalSpotNode node =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "hasRemoteActorBoundSessionRoute" -> false;
                                            case "hasLocalActorBoundSessionRoute" -> {
                                                routeChecks.incrementAndGet();
                                                yield false;
                                            }
                                            case "sendLocalActorBoundSessionAsync" -> {
                                                submissions.incrementAndGet();
                                                yield CompletableFuture.completedFuture(null);
                                            }
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        var result =
                new ZLinkActorBoundSessionSender(Duration.ofSeconds(1), () -> false)
                        .send(
                                node,
                                new ZLinkBackendActorRef(RoutingId.from("node"), "actor", 1),
                                "actor",
                                new byte[] {1},
                                "bound reply failed");
        var terminal =
                org.junit.jupiter.api.Assertions.assertThrows(
                        java.util.concurrent.ExecutionException.class,
                        () -> result.toCompletableFuture().get(1, TimeUnit.SECONDS));
        var error =
                org.junit.jupiter.api.Assertions.assertInstanceOf(
                        systems.zlink.framework.errors.ZLinkFrameworkException.class,
                        terminal.getCause());
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                error.kind());
        assertEquals(1, routeChecks.get());
        assertEquals(0, submissions.get());
    }

    @Test
    void localNotFoundTerminalIsNotResubmitted() throws Exception {
        AtomicInteger submissions = new AtomicInteger();
        var failure =
                new systems.zlink.contracts.errors.ZlinkSubmitException(
                        systems.zlink.contracts.sockets.SubmitResult.NOT_FOUND);
        ZLinkInternalSpotNode node =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "hasRemoteActorBoundSessionRoute" -> false;
                                            case "hasLocalActorBoundSessionRoute" -> true;
                                            case "sendLocalActorBoundSessionAsync" -> {
                                                submissions.incrementAndGet();
                                                yield CompletableFuture.failedFuture(failure);
                                            }
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        var result =
                new ZLinkActorBoundSessionSender(Duration.ofMillis(80), () -> false)
                        .send(
                                node,
                                new ZLinkBackendActorRef(RoutingId.from("node"), "actor", 1),
                                "actor",
                                new byte[] {1},
                                "bound reply failed");
        var terminal =
                org.junit.jupiter.api.Assertions.assertThrows(
                        java.util.concurrent.ExecutionException.class,
                        () -> result.toCompletableFuture().get(1, TimeUnit.SECONDS));
        org.junit.jupiter.api.Assertions.assertSame(failure, terminal.getCause());
        assertEquals(1, submissions.get());
    }

    @Test
    void localStreamRouteDelegatesOnceToTheBindingAdmissionTerminal() throws Exception {
        AtomicInteger synchronousSubmissions = new AtomicInteger();
        AtomicInteger asynchronousSubmissions = new AtomicInteger();
        AtomicBoolean closing = new AtomicBoolean();
        CompletableFuture<Void> admissionTerminal = new CompletableFuture<>();
        ZLinkInternalSpotNode node =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
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
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        ZLinkActorBoundSessionSender sender =
                new ZLinkActorBoundSessionSender(Duration.ofSeconds(1), closing::get);

        CompletionStage<Void> submitted =
                sender.send(
                        node,
                        new ZLinkBackendActorRef(
                                RoutingId.from("local-actor-node"), "local-actor", 1),
                        "local-actor",
                        new byte[] {1, 2, 3},
                        "bound reply failed");
        try {
            long evidenceDeadline = System.nanoTime() + Duration.ofMillis(400).toNanos();
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
        ZLinkInternalSpotNode node =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName()
                                            .equals("hasRemoteActorBoundSessionRoute")) {
                                        return true;
                                    }
                                    if (method.getName().equals("sendRemoteActorBoundSession")) {
                                        submissions.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    }
                                    throw new UnsupportedOperationException(method.getName());
                                });
        ZLinkActorBoundSessionSender sender =
                new ZLinkActorBoundSessionSender(Duration.ofSeconds(1), () -> false);

        sender.send(
                        node,
                        new ZLinkBackendActorRef(RoutingId.from("actor-node"), "actor-1", 1),
                        "actor-1",
                        new byte[] {1, 2, 3},
                        "bound reply failed")
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

        assertEquals(1, submissions.get());
    }
}
