package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import java.lang.reflect.Proxy;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkSpotRuntimeCloseTest {
    private static final int COMPLETION_OWNER_TIMEOUT_SECONDS = 1;

    @Test
    void runtimeReportsSocketCloseFailureThroughItsExistingReporter() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(
                new systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore());
        options.addRouteMesh("close-report")
                .listen("inproc://close-report-" + System.nanoTime())
                .objects()
                .server();
        try (var framework = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var host = (ZLinkSpotRuntime) framework.spotManager();
            var nodes = host.nodes;
            var original = nodes.getFirst();
            var failure = new ZlinkCloseException(CloseResult.BUSY, 16);
            var reporter = host.dispatchErrors;
            long reportedBefore = reporter.reportedCount();
            nodes.set(
                    0,
                    (ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    ZLinkInternalSpotNode.class.getClassLoader(),
                                    new Class<?>[] {ZLinkInternalSpotNode.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("close")) {
                                            throw failure;
                                        }
                                        return method.invoke(original, args);
                                    }));
            try {
                var thrown =
                        assertThrows(
                                CompletionException.class,
                                () -> host.closeAsync().toCompletableFuture().join());
                assertSame(failure, thrown.getCause());
                assertEquals(reportedBefore + 1, reporter.reportedCount());
            } finally {
                nodes.set(0, original);
            }
        }
    }

    @Test
    void closeWaitsForActivationTerminalsWithoutBlockingTheirCompletionOwner() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(
                new systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore());
        options.addRouteMesh("close-terminal")
                .listen("inproc://close-terminal-" + System.nanoTime())
                .objects()
                .server();
        try (var framework = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var host = (ZLinkSpotRuntime) framework.spotManager();
            var nodes = host.nodes;
            var original = nodes.getFirst();
            var terminal = new CompletableFuture<Void>();
            var completionReturned = new CompletableFuture<Void>();
            var nodeClosed = new AtomicBoolean();
            nodes.set(
                    0,
                    (ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    ZLinkInternalSpotNode.class.getClassLoader(),
                                    new Class<?>[] {ZLinkInternalSpotNode.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("instanceActivationTerminals"))
                                            return terminal;
                                        if (method.getName().equals("close")) {
                                            // Host relocation §14: accepted transport callbacks
                                            // finish before the transport resource is destroyed.
                                            completionReturned.get(
                                                    COMPLETION_OWNER_TIMEOUT_SECONDS,
                                                    TimeUnit.SECONDS);
                                            nodeClosed.set(true);
                                        }
                                        return method.invoke(original, args);
                                    }));
            try (var completionOwner = java.util.concurrent.Executors.newSingleThreadExecutor()) {
                host.spotLifecycle().closeAllAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
                var closing = host.closeAsync().toCompletableFuture();
                assertFalse(closing.isDone(), "Spot close finished before activation terminals");
                assertFalse(nodeClosed.get(), "node closed before activation terminals");
                completionOwner.submit(
                        () -> {
                            terminal.complete(null);
                            completionReturned.complete(null);
                        });
                closing.get(5, TimeUnit.SECONDS);
                assertTrue(nodeClosed.get());
            } finally {
                nodes.set(0, original);
            }
        }
    }

    @Test
    void socketCloseFailureIsReturnedAndLaterFailuresAreSuppressed() throws Exception {
        var busy = new ZlinkCloseException(CloseResult.BUSY, 16);
        Runnable fail =
                () -> {
                    throw busy;
                };
        assertSame(busy, ZLinkSpotRuntime.closeRuntimeComponent(fail, null));
        var first = new IllegalStateException("earlier cleanup failure");
        assertSame(first, ZLinkSpotRuntime.closeRuntimeComponent(fail, first));
        assertEquals(1, first.getSuppressed().length);
        assertSame(busy, first.getSuppressed()[0]);
    }
}
