package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkConfigurationException;

import java.time.Duration;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionHandler;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

final class ZLinkFrameworkRegistrationTest {
    @Test
    void messageFollowDurationDefaultsToCommonContract() {
        var registration = new ZLinkFrameworkRegistration();

        assertEquals(
                Duration.ofSeconds(30), registration.locations().options().messageFollowDuration());
    }

    @RepeatedTest(100)
    void handlerExecutorRejectsBuiltInDiscardPoliciesOnly() {
        assertRejected(new ThreadPoolExecutor.DiscardPolicy());
        assertRejected(new ThreadPoolExecutor.DiscardOldestPolicy());
        assertRejected(new ThreadPoolExecutor.DiscardPolicy() {});
        assertRejected(new ThreadPoolExecutor.DiscardOldestPolicy() {});
        assertRejected(
                new ThreadPoolExecutor(
                        1,
                        1,
                        0,
                        TimeUnit.MILLISECONDS,
                        new ArrayBlockingQueue<>(1),
                        new ThreadPoolExecutor.DiscardPolicy()) {});

        assertAccepted(threadPool(null));
        assertAccepted(threadPool(new ThreadPoolExecutor.CallerRunsPolicy()));
        assertAccepted(Runnable::run);
    }

    private static void assertRejected(RejectedExecutionHandler policy) {
        assertRejected(threadPool(policy));
    }

    private static void assertRejected(ThreadPoolExecutor executor) {
        var registration = new ZLinkFrameworkRegistration();
        Executor configuredExecutor = registration.handlerExecutor();
        try {
            var failure =
                    assertThrows(
                            ZLinkConfigurationException.class,
                            () -> registration.useHandlerExecutor(executor));

            assertEquals(
                    "handler executor rejection policy must not discard tasks",
                    failure.getMessage());
            assertSame(configuredExecutor, registration.handlerExecutor());
            registration.useHandlerExecutor(Runnable::run);
        } finally {
            executor.shutdownNow();
        }
    }

    private static void assertAccepted(Executor executor) {
        var registration = new ZLinkFrameworkRegistration();
        try {
            registration.useHandlerExecutor(executor);
            assertSame(executor, registration.handlerExecutor());
        } finally {
            if (executor instanceof ThreadPoolExecutor pool) pool.shutdownNow();
        }
    }

    private static ThreadPoolExecutor threadPool(RejectedExecutionHandler handler) {
        if (handler == null) {
            return new ThreadPoolExecutor(
                    1, 1, 0, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(1));
        }
        return new ThreadPoolExecutor(
                1, 1, 0, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(1), handler);
    }
}
