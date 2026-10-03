package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkActorRetrySchedulerTest {
    @Test
    void boundSessionSubmitsOnceWithItsEntireBudget() {
        AtomicInteger binds = new AtomicInteger();
        AtomicInteger submits = new AtomicInteger();
        Duration timeout = Duration.ofSeconds(3);
        var terminal = new CompletableFuture<Void>();
        var stream =
                (systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket)
                        java.lang.reflect.Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {
                                    systems.zlink.framework.runtime.internal.backend
                                            .ZLinkBackendStreamSocket.class
                                },
                                (proxy, method, args) -> {
                                    assertEquals("bindActor", method.getName());
                                    binds.incrementAndGet();
                                    return (systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkBackendActorBindOperation)
                                            remaining -> {
                                                submits.incrementAndGet();
                                                assertEquals(timeout, remaining);
                                                return terminal;
                                            };
                                });
        var bound =
                ZLinkBoundSessionRuntime.bindActorWithRetry(
                        stream,
                        RoutingId.from("session"),
                        new ZLinkBackendActorRef(RoutingId.from("target"), "actor", 1),
                        timeout);
        var failure =
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, "durable sender exhausted");
        terminal.completeExceptionally(failure);
        assertSame(
                failure,
                Assertions.assertThrows(
                                CompletionException.class, () -> bound.toCompletableFuture().join())
                        .getCause());
        assertEquals(1, binds.get());
        assertEquals(1, submits.get());
    }
}
