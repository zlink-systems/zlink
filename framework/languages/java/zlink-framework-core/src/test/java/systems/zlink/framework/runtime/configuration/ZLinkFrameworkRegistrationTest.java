package systems.zlink.framework.runtime.configuration;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkStreamNodeBuilder;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.streams.ZLinkSession;

import java.time.Duration;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionHandler;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

final class ZLinkFrameworkRegistrationTest {
    @Test
    void authenticationAdmissionValidatesSettingsBeforeHostStartup() {
        assertAdmissionInvalid(node -> node.maxConcurrentAuthentications(0));
        assertAdmissionInvalid(node -> node.maxConcurrentAuthentications(-1));
        assertAdmissionInvalid(node -> node.authenticationRetryDelay(Duration.ZERO));
        assertAdmissionInvalid(node -> node.authenticationRetryDelay(Duration.ofNanos(1)));
        assertAdmissionInvalid(
                node -> node.authenticationRetryDelay(Duration.ofMillis(1).plusNanos(1)));
        assertAdmissionInvalid(node -> node.authenticationRetryDelay(Duration.ofMillis(-1)));
        assertAdmissionInvalid(
                node -> node.authenticationRetryDelay(Duration.ofMillis(1L + Integer.MAX_VALUE)));

        var options = new DefaultZLinkFrameworkOptions();
        options.addStreamNode("clients")
                .bind(0)
                .registerSession(FirstSession.class)
                .authenticationPacketName("Login")
                .maxConcurrentAuthentications(Integer.MAX_VALUE)
                .authenticationRetryDelay(Duration.ofMillis(Integer.MAX_VALUE));
        assertDoesNotThrow(options::validate);
    }

    @Test
    void authenticationAdmissionRequiresMatchingOptionsAcrossStreamRegistrations() {
        assertAdmissionConflict(node -> node.authenticationPacketName("OtherLogin"));
        assertAdmissionConflict(node -> node.authenticationPacketName(null));
        assertAdmissionConflict(node -> node.maxConcurrentAuthentications(3));
        assertAdmissionConflict(node -> node.authenticationRetryDelay(Duration.ofMillis(499)));
        assertDoesNotThrow(admissionAcrossStreamRegistrations().options()::validate);
    }

    private static void assertAdmissionInvalid(Consumer<ZLinkStreamNodeBuilder> change) {
        var options = new DefaultZLinkFrameworkOptions();
        var node = options.addStreamNode("clients").bind(0).registerSession(FirstSession.class);
        assertThrows(
                ZLinkConfigurationException.class,
                () -> {
                    change.accept(node);
                    options.validate();
                });
    }

    private static void assertAdmissionConflict(Consumer<ZLinkStreamNodeBuilder> change) {
        var registrations = admissionAcrossStreamRegistrations();
        change.accept(registrations.second());
        assertThrows(ZLinkConfigurationException.class, registrations.options()::validate);
    }

    private static StreamRegistrations admissionAcrossStreamRegistrations() {
        var options = new DefaultZLinkFrameworkOptions();
        options.addStreamNode("first")
                .bind(0)
                .registerSession(FirstSession.class)
                .authenticationPacketName("Login")
                .maxConcurrentAuthentications(2);
        var second =
                options.addStreamNode("second")
                        .bind(0)
                        .registerSession(SecondSession.class)
                        .authenticationPacketName("Login")
                        .maxConcurrentAuthentications(2)
                        .authenticationRetryDelay(Duration.ofMillis(500));
        return new StreamRegistrations(options, second);
    }

    private record StreamRegistrations(
            DefaultZLinkFrameworkOptions options, ZLinkStreamNodeBuilder second) {}

    private abstract static class FirstSession implements ZLinkSession {}

    private abstract static class SecondSession implements ZLinkSession {}

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
