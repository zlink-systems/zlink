package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BiFunction;

final class ZLinkChannelSubmissionClockTest {
    @ParameterizedTest
    @ValueSource(strings = {"channel", "node", "spot"})
    void untimedSubmissionDoesNotReadTheRequestClock(String route) {
        var reads = new AtomicInteger();
        var sockets =
                new ZLinkChannelSocketRegistry(null, () -> reads.incrementAndGet(), () -> false);
        registerNode(sockets);
        try {
            var submissions = new AtomicInteger();
            for (int index = 0; index < 100; index++) {
                submit(
                                sockets,
                                route,
                                null,
                                null,
                                (node, timeout) -> {
                                    assertNull(timeout);
                                    submissions.incrementAndGet();
                                    return CompletableFuture.completedFuture(null);
                                })
                        .toCompletableFuture()
                        .join();
            }
            assertEquals(100, submissions.get());
            assertEquals(0, reads.get(), "untimed sends have no request budget to measure");
        } finally {
            sockets.closeAll();
        }
    }

    @ParameterizedTest
    @CsvSource({
        "channel,false",
        "node,false",
        "spot,false",
        "channel,true",
        "node,true",
        "spot,true"
    })
    void timedSubmissionPreservesTheElapsedRequestBudget(String route, boolean overrideOnly) {
        var reads = new AtomicInteger();
        var now = new AtomicLong(1_000);
        var sockets =
                new ZLinkChannelSocketRegistry(
                        null,
                        () -> {
                            reads.incrementAndGet();
                            return now.getAndAdd(25);
                        },
                        () -> false);
        registerNode(sockets);
        try {
            var submissions = new AtomicInteger();
            submit(
                            sockets,
                            route,
                            overrideOnly ? Duration.ofNanos(100) : null,
                            overrideOnly ? null : Duration.ofNanos(100),
                            (node, timeout) -> {
                                assertEquals(Duration.ofNanos(75), timeout);
                                submissions.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            })
                    .toCompletableFuture()
                    .join();
            assertEquals(1, submissions.get());
            assertEquals(2, reads.get());
        } finally {
            sockets.closeAll();
        }
    }

    @ParameterizedTest
    @CsvSource({
        "channel,false",
        "node,false",
        "spot,false",
        "channel,true",
        "node,true",
        "spot,true"
    })
    void elapsedRequestBudgetRejectsBeforeBackendSubmission(String route, boolean overrideOnly) {
        var now = new AtomicLong(1_000);
        var sockets = new ZLinkChannelSocketRegistry(null, () -> now.getAndAdd(100), () -> false);
        registerNode(sockets);
        try {
            var submissions = new AtomicInteger();
            CompletionException failure =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    submit(
                                                    sockets,
                                                    route,
                                                    overrideOnly ? Duration.ofNanos(100) : null,
                                                    overrideOnly ? null : Duration.ofNanos(100),
                                                    (node, timeout) -> {
                                                        submissions.incrementAndGet();
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    })
                                            .toCompletableFuture()
                                            .join());
            var cause = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
            assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, cause.kind());
            assertEquals(0, submissions.get());
        } finally {
            sockets.closeAll();
        }
    }

    private static CompletionStage<Void> submit(
            ZLinkChannelSocketRegistry sockets,
            String route,
            Duration timeoutOverride,
            Duration defaultTimeout,
            BiFunction<ZLinkInternalSpotNode, Duration, CompletionStage<Void>> submission) {
        return switch (route) {
            case "channel" ->
                    sockets.submitToChannel(
                            "orders",
                            timeoutOverride,
                            defaultTimeout,
                            (client, remaining) -> {
                                throw new AssertionError("unexpected client");
                            },
                            submission,
                            failure -> {});
            case "node" ->
                    sockets.submitToNode(
                            "orders",
                            timeoutOverride,
                            defaultTimeout,
                            (router, remaining) -> {
                                throw new AssertionError("unexpected router");
                            },
                            submission,
                            failure -> {});
            case "spot" ->
                    sockets.submitToSpot(
                            "orders",
                            RoutingId.from("target"),
                            null,
                            timeoutOverride,
                            defaultTimeout,
                            (bridge, remaining) -> {
                                throw new AssertionError("unexpected bridge");
                            },
                            submission);
            default -> throw new AssertionError(route);
        };
    }

    private static void registerNode(ZLinkChannelSocketRegistry sockets) {
        var node =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkChannelSubmissionClockTest.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("sourceTransportLane")) return null;
                                    throw new AssertionError("unexpected node call: " + method);
                                });
        sockets.registerSpotRouterNode("orders", node);
    }
}
