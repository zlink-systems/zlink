package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkInfrastructureShutdownDeadlineTest {
    private static final long WAIT_SECONDS = 30;

    @Test
    void lateInfrastructureSettlementReportsForceStopped() throws Exception {
        assertDeadlineOutcome(false, false);
    }

    @Test
    void channelExecutorDeadlineFailureReportsDeadlineExceeded() throws Exception {
        assertDeadlineOutcome(true, false);
    }

    @Test
    void lateSpotPublisherSettlementReportsForceStopped() throws Exception {
        assertDeadlineOutcome(false, true);
    }

    @Test
    void closePreservesShutdownTerminalAfterTeardownFailure() throws Exception {
        var executor = java.util.concurrent.Executors.newVirtualThreadPerTaskExecutor();
        var runtime =
                ZLinkFrameworkRuntimeTestAccess.startWithChannelInfrastructureExecutor(
                        new DefaultZLinkFrameworkOptions(),
                        new ZLinkJavaBackendAdapterFactory(),
                        failChannelCleanup(executor));
        runtime.startupCompletion().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
        var terminal =
                runtime.shutdown(Duration.ofSeconds(WAIT_SECONDS))
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
        assertEquals(ZLinkFrameworkTerminationOutcome.FORCE_STOPPED, terminal.outcome());
        assertEquals(ZLinkFrameworkTerminationReason.TEARDOWN_FAILED, terminal.reason());
        assertDoesNotThrow(runtime::close);
        assertDoesNotThrow(runtime::close);
        assertEquals(terminal, runtime.status().terminationResult().orElseThrow());
        assertEquals(
                terminal,
                runtime.shutdown(Duration.ofSeconds(WAIT_SECONDS))
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS));
        assertTrue(executor.isTerminated());
    }

    @Test
    void directCloseStillPropagatesCleanupFailure() throws Exception {
        var executor = java.util.concurrent.Executors.newVirtualThreadPerTaskExecutor();
        var runtime =
                ZLinkFrameworkRuntimeTestAccess.startWithChannelInfrastructureExecutor(
                        new DefaultZLinkFrameworkOptions(),
                        new ZLinkJavaBackendAdapterFactory(),
                        failChannelCleanup(executor));
        runtime.startupCompletion().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
        var failure = assertThrows(ZLinkFrameworkShutdown.Failure.class, runtime::close);
        assertEquals("injected executor cleanup failure", failure.getCause().getMessage());
        assertTrue(executor.isTerminated());
    }

    private ExecutorService failChannelCleanup(ExecutorService executor) {
        var failure = new IllegalStateException("injected executor cleanup failure");
        return (ExecutorService)
                Proxy.newProxyInstance(
                        ExecutorService.class.getClassLoader(),
                        new Class<?>[] {ExecutorService.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("isTerminated")) return false;
                            try {
                                var result = method.invoke(executor, arguments);
                                if (method.getName().equals("awaitTermination")) throw failure;
                                return result;
                            } catch (InvocationTargetException error) {
                                throw error.getCause();
                            }
                        });
    }

    private void assertDeadlineOutcome(boolean executorDeadline, boolean spotPublisher)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        if (spotPublisher) {
            options.addRouteMesh("game")
                    .listen("inproc://c1876-host-publisher")
                    .channelName("events")
                    .client();
        }
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var waiting = new CompletableFuture<Void>();
        ExecutorService executor;
        ZLinkFrameworkRuntime runtime;
        if (spotPublisher) {
            runtime =
                    ZLinkFrameworkRuntimeTestAccess.startHostWithConstruction(
                            options,
                            new ZLinkJavaBackendAdapterFactory(),
                            ZLinkHandlerActivator.reflection(),
                            null,
                            new ZLinkFrameworkRuntimeTestAccess.Construction());
            executor =
                    systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTestAccess
                            .multicastExecutor(
                                    (systems.zlink.framework.runtime.spots.ZLinkSpotRuntime)
                                            runtime.spotManager());
        } else {
            executor =
                    observeExecutor(
                            java.util.concurrent.Executors.newVirtualThreadPerTaskExecutor(),
                            waiting,
                            executorDeadline);
            runtime =
                    ZLinkFrameworkRuntimeTestAccess.startWithChannelInfrastructureExecutor(
                            options, new ZLinkJavaBackendAdapterFactory(), executor);
        }
        runtime.startupCompletion().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
        executor.execute(
                () -> {
                    entered.countDown();
                    try {
                        release.await();
                    } catch (InterruptedException interrupted) {
                        Thread.currentThread().interrupt();
                    }
                });
        CompletableFuture<ZLinkFrameworkTerminationResult> shutdown = null;
        try {
            assertTrue(entered.await(WAIT_SECONDS, TimeUnit.SECONDS));
            shutdown = runtime.shutdown(Duration.ofSeconds(WAIT_SECONDS)).toCompletableFuture();
            if (spotPublisher) {
                awaitShutdown(executor);
                waiting.complete(null);
            }
            CompletableFuture.anyOf(waiting, shutdown).get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertFalse(shutdown.isDone());
            if (!executorDeadline) {
                runtime.terminationDeadline.set(Instant.EPOCH);
            }
        } finally {
            release.countDown();
            assertTrue(executor.awaitTermination(WAIT_SECONDS, TimeUnit.SECONDS));
            if (shutdown != null) shutdown.get(WAIT_SECONDS, TimeUnit.SECONDS);
            var terminal = shutdown.get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertDoesNotThrow(runtime::close);
            assertDoesNotThrow(runtime::close);
            assertEquals(terminal, runtime.status().terminationResult().orElseThrow());
        }
        var result = shutdown.get(WAIT_SECONDS, TimeUnit.SECONDS);
        assertEquals(ZLinkFrameworkTerminationOutcome.FORCE_STOPPED, result.outcome());
        assertEquals(ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED, result.reason());
        assertEquals(ZLinkFrameworkRuntimeState.STOPPED, runtime.status().state());
        assertEquals(result, runtime.status().terminationResult().orElseThrow());
    }

    private ExecutorService observeExecutor(
            ExecutorService executor, CompletableFuture<Void> waiting, boolean deadlineFailure) {
        var firstWait = new AtomicBoolean(true);
        return (ExecutorService)
                Proxy.newProxyInstance(
                        ExecutorService.class.getClassLoader(),
                        new Class<?>[] {ExecutorService.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("awaitTermination")) {
                                if (deadlineFailure && firstWait.getAndSet(false)) return false;
                                waiting.complete(null);
                            }
                            try {
                                return method.invoke(executor, arguments);
                            } catch (InvocationTargetException error) {
                                throw error.getCause();
                            }
                        });
    }

    private static void awaitShutdown(ExecutorService executor) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(WAIT_SECONDS);
        while (!executor.isShutdown()) {
            assertTrue(System.nanoTime() < deadline, "runtime did not shut down its executor");
            Thread.onSpinWait();
        }
    }
}
