package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

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

    private void assertDeadlineOutcome(boolean executorDeadline, boolean spotPublisher)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        if (spotPublisher) {
            options.addRouteMesh("game")
                    .listen("inproc://c1876-host-publisher")
                    .channelName("events")
                    .client();
        }
        var runtime = ZLinkFrameworkRuntimeTestAccess.start(options);
        runtime.startupCompletion().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var waiting = new CompletableFuture<Void>();
        ExecutorService executor;
        if (spotPublisher) {
            var spotsField = ZLinkFrameworkRuntime.class.getDeclaredField("spots");
            spotsField.setAccessible(true);
            var spots = (ZLinkSpotRuntime) spotsField.get(runtime);
            var publishersField = ZLinkSpotRuntime.class.getDeclaredField("publishers");
            publishersField.setAccessible(true);
            var publishers = publishersField.get(spots);
            var poolField = publishers.getClass().getDeclaredField("multicastExecutor");
            poolField.setAccessible(true);
            ((ExecutorService) poolField.get(publishers)).shutdown();
            var pool =
                    new ThreadPoolExecutor(1, 1, 0, TimeUnit.SECONDS, new LinkedBlockingQueue<>()) {
                        @Override
                        public boolean awaitTermination(long timeout, TimeUnit unit)
                                throws InterruptedException {
                            waiting.complete(null);
                            return super.awaitTermination(timeout, unit);
                        }
                    };
            poolField.set(publishers, pool);
            executor = pool;
        } else {
            var channelsField = ZLinkFrameworkRuntime.class.getDeclaredField("channels");
            channelsField.setAccessible(true);
            var channels = (ZLinkChannelRuntime) channelsField.get(runtime);
            var executorField =
                    ZLinkChannelRuntime.class.getDeclaredField("infrastructureExecutor");
            executorField.setAccessible(true);
            executor = (ExecutorService) executorField.get(channels);
            var firstWait = new AtomicBoolean(true);
            executorField.set(
                    channels,
                    Proxy.newProxyInstance(
                            ExecutorService.class.getClassLoader(),
                            new Class<?>[] {ExecutorService.class},
                            (proxy, method, arguments) -> {
                                if (method.getName().equals("awaitTermination")) {
                                    if (executorDeadline && firstWait.getAndSet(false))
                                        return false;
                                    waiting.complete(null);
                                }
                                try {
                                    return method.invoke(executor, arguments);
                                } catch (InvocationTargetException error) {
                                    throw error.getCause();
                                }
                            }));
        }
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
            CompletableFuture.anyOf(waiting, shutdown).get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertFalse(shutdown.isDone());
            if (!executorDeadline) {
                var deadlineField =
                        ZLinkFrameworkRuntime.class.getDeclaredField("terminationDeadline");
                deadlineField.setAccessible(true);
                @SuppressWarnings("unchecked")
                var deadline = (AtomicReference<Instant>) deadlineField.get(runtime);
                deadline.set(Instant.EPOCH);
            }
        } finally {
            release.countDown();
            assertTrue(executor.awaitTermination(WAIT_SECONDS, TimeUnit.SECONDS));
            if (shutdown != null) shutdown.get(WAIT_SECONDS, TimeUnit.SECONDS);
            if (executorDeadline) {
                var failure = assertThrows(ZLinkFrameworkShutdown.Failure.class, runtime::close);
                assertTrue(failure.getCause() instanceof TimeoutException);
            } else {
                runtime.close();
            }
        }
        var result = shutdown.get(WAIT_SECONDS, TimeUnit.SECONDS);
        assertEquals(ZLinkFrameworkTerminationOutcome.FORCE_STOPPED, result.outcome());
        assertEquals(ZLinkFrameworkTerminationReason.DEADLINE_EXCEEDED, result.reason());
        assertEquals(ZLinkFrameworkRuntimeState.STOPPED, runtime.status().state());
        assertEquals(result, runtime.status().terminationResult().orElseThrow());
    }
}
