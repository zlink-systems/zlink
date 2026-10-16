package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireRuntime;

import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkRelocationPreflightFailureTest {
    @Test
    void providerTypedFailureRemainsAStoreFailureWithItsOriginalCause() throws Exception {
        verify(
                new ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                        "preflight type is incompatible"),
                ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE);
    }

    @Test
    void storePreflightFailureKeepsItsOriginalCause() throws Exception {
        verify(
                new IOException("preflight location read failed"),
                ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE);
    }

    @Test
    void providerTimeoutRemainsAStoreFailureWithItsOriginalCause() throws Exception {
        verify(
                new TimeoutException("preflight deadline expired"),
                ZLinkFrameworkRelocationReason.STORE_UNAVAILABLE);
    }

    private static void verify(Throwable cause, ZLinkFrameworkRelocationReason expected)
            throws Exception {
        var inner = new ZLinkInMemoryLocationStore();
        var inject = new ThreadLocal<Boolean>();
        var calls = new AtomicInteger();
        var barrier = new CompletableFuture<Object>();
        var store =
                (ZLinkLocationStore)
                        Proxy.newProxyInstance(
                                ZLinkLocationStore.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationStore.class},
                                (proxy, method, arguments) -> {
                                    if (Boolean.TRUE.equals(inject.get())
                                            && method.getName().equals("scan")) {
                                        calls.incrementAndGet();
                                        return barrier;
                                    }
                                    try {
                                        return method.invoke(inner, arguments);
                                    } catch (InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                });
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh("preflight")
                .listen("inproc://preflight-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("preflight-source"))
                .objects()
                .server();
        var failures = new CopyOnWriteArrayList<LogRecord>();
        var logger = Logger.getLogger(ZLinkFrameworkRuntime.class.getName());
        var listener =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        if (record.getThrown() != null) failures.add(record);
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(listener);
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            assertTrue(runtime.isReady());
            java.util.concurrent.CompletionStage<ZLinkFrameworkRelocationResult> operation;
            inject.set(true);
            try {
                operation =
                        runtime.relocate(
                                new ZLinkFrameworkRelocationOptions(
                                        ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                        null,
                                        Duration.ofSeconds(3)));
            } finally {
                inject.remove();
            }
            try {
                assertEquals(1, calls.get());
                assertFalse(operation.toCompletableFuture().isDone());
            } finally {
                barrier.completeExceptionally(new CompletionException(cause));
            }
            var result = operation.toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkRelocationOutcome.BLOCKED, result.outcome());
            assertEquals(expected, result.reason());
            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.status().state());
            assertEquals(
                    1,
                    failures.stream()
                            .filter(record -> record.getThrown().getCause() == cause)
                            .count(),
                    "the aggregate reason must not discard the original failure");
        } finally {
            barrier.completeExceptionally(cause);
            logger.removeHandler(listener);
        }
    }
}
