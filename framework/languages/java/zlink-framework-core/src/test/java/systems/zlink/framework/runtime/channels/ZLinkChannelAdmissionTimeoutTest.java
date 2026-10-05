package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkChannelAdmissionTimeoutTest {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(5);
    private static final Duration DEFAULT_TIMEOUT = Duration.ofSeconds(10);

    @Test
    void fanoutBuilderRejectsInvalidSendTimeouts() {
        for (Duration timeout :
                new Duration[] {
                    Duration.ZERO,
                    Duration.ofNanos(-1),
                    Duration.ofMillis(Integer.MAX_VALUE).plusNanos(1)
                }) {
            DefaultZLinkFrameworkOptions fanoutOptions = new DefaultZLinkFrameworkOptions();
            assertThrows(
                    ZLinkConfigurationException.class,
                    () -> fanoutOptions.addFanoutChannel("fanout").setSendTimeout(timeout));
        }

        Duration maximum = Duration.ofMillis(Integer.MAX_VALUE);
        new DefaultZLinkFrameworkOptions()
                .addFanoutChannel("maximum-fanout")
                .setSendTimeout(maximum);
    }

    @RepeatedTest(5)
    void requestsAdmitWhenServerBecomesReadyWithinRequestTimeout() throws Exception {
        for (String operation : new String[] {"request"}) {
            AtomicReference<Duration> submittedTimeout = new AtomicReference<>();
            ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
            setupClient(sockets);
            try {
                CompletionStage<String> accepted = submit(sockets, submittedTimeout, operation);
                org.junit.jupiter.api.Assertions.assertFalse(
                        accepted.toCompletableFuture().isDone());
                assertTrue(sockets.admitClientServerConnection("server", descriptor()));
                assertEquals(operation, accepted.toCompletableFuture().get(2, TimeUnit.SECONDS));
                assertTrue(submittedTimeout.get().compareTo(REQUEST_TIMEOUT) <= 0);
                sockets.signalTopologyChanged();
                assertEquals(operation, accepted.toCompletableFuture().join());
            } finally {
                sockets.closeAll();
            }
        }
    }

    @Test
    void cancelledReadinessDoesNotSubmitAndAdmittedCancellationReachesBinding() {
        ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        setupClient(sockets);
        try {
            var submittedTimeout = new AtomicReference<Duration>();
            var pending = submit(sockets, submittedTimeout).toCompletableFuture();
            assertTrue(pending.cancel(false));
            assertTrue(sockets.admitClientServerConnection("server", descriptor()));
            assertNull(submittedTimeout.get());
            var binding = new CompletableFuture<String>();
            var accepted =
                    sockets.submitToChannel(
                                    "work",
                                    REQUEST_TIMEOUT,
                                    DEFAULT_TIMEOUT,
                                    (client, timeout) -> binding,
                                    (node, timeout) -> {
                                        throw new AssertionError("ClientServer must use DEALER");
                                    },
                                    failure -> {})
                            .toCompletableFuture();
            assertTrue(accepted.cancel(false));
            assertTrue(binding.isCancelled());
        } finally {
            sockets.closeAll();
        }
    }

    @Test
    void descriptorRefreshCannotRestoreInvalidatedAdmissionOrRetireAheadOfReplacement()
            throws Exception {
        var sockets = new ZLinkChannelSocketRegistry();
        var physical = dealer();
        var replacement = dealer();
        var registration = new ChannelRegistration("work", ChannelKind.CLIENT_SERVER);
        registration.enableClient();
        sockets.registerChannel(registration);
        sockets.addClientServerConnection("server", descriptor(), physical);
        try {
            assertTrue(sockets.admitClientServerConnection("server", descriptor()));
            sockets.clientServerTransportReady("server", physical);
            var fence =
                    sockets.updateClientServerConnection("server", descriptor(), physical)
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS);
            org.junit.jupiter.api.Assertions.assertNotNull(
                    fence, "Registry invalidation must survive a Location descriptor refresh");
            var submitted = new AtomicReference<Duration>();
            var pending = submit(sockets, submitted).toCompletableFuture();
            org.junit.jupiter.api.Assertions.assertFalse(pending.isDone());
            assertNull(submitted.get());
            sockets.addClientServerConnection("replacement", descriptor(), replacement);
            org.junit.jupiter.api.Assertions.assertFalse(
                    sockets.retireClientServerConnection(
                                    "server", physical, java.util.Set.of("replacement"))
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS));
            assertEquals(2, sockets.clientServerPhysicalConnectionCount());
            pending.cancel(false);
        } finally {
            sockets.closeAll();
        }
    }

    @Test
    void cancelledReadinessDuringBindingStartDoesNotReleaseTransferredPayload() throws Exception {
        var sockets = new ZLinkChannelSocketRegistry();
        setupClient(sockets);
        var entered = new java.util.concurrent.CountDownLatch(1);
        var resume = new java.util.concurrent.CountDownLatch(1);
        var rejected = new java.util.concurrent.atomic.AtomicInteger();
        var released = new java.util.concurrent.atomic.AtomicInteger();
        var binding = new CompletableFuture<String>();
        try (var payload = systems.zlink.contracts.messaging.Message.from("binding-owned");
                var worker = java.util.concurrent.Executors.newSingleThreadExecutor()) {
            var pending =
                    sockets.submitToChannel(
                                    "work",
                                    REQUEST_TIMEOUT,
                                    DEFAULT_TIMEOUT,
                                    (client, timeout) -> {
                                        entered.countDown();
                                        try {
                                            assertTrue(resume.await(1, TimeUnit.SECONDS));
                                        } catch (InterruptedException failure) {
                                            Thread.currentThread().interrupt();
                                            throw new AssertionError(failure);
                                        }
                                        assertEquals("binding-owned", payload.toUtf8String());
                                        binding.whenComplete(
                                                (value, failure) -> {
                                                    released.incrementAndGet();
                                                    payload.close();
                                                });
                                        return binding;
                                    },
                                    (node, timeout) -> {
                                        throw new AssertionError();
                                    },
                                    failure -> {
                                        rejected.incrementAndGet();
                                        payload.close();
                                    })
                            .toCompletableFuture();
            var ready =
                    worker.submit(
                            () -> sockets.admitClientServerConnection("server", descriptor()));
            assertTrue(entered.await(1, TimeUnit.SECONDS));
            assertTrue(pending.cancel(false));
            try {
                assertEquals(
                        0,
                        rejected.get(),
                        "readiness ownership was already transferred to binding start");
                assertEquals("binding-owned", payload.toUtf8String());
            } finally {
                resume.countDown();
            }
            ready.get(1, TimeUnit.SECONDS);
            assertTrue(binding.isCancelled());
            assertTrue(payload.empty());
            assertEquals(1, released.get());
        } finally {
            resume.countDown();
            sockets.closeAll();
        }
    }

    @Test
    void cancelledOrExpiredReadinessReleasesUntransferredPayloadOnce() {
        for (boolean cancel : new boolean[] {true, false}) {
            var sockets = new ZLinkChannelSocketRegistry();
            setupClient(sockets);
            var rejected = new java.util.concurrent.atomic.AtomicInteger();
            try (var payload = systems.zlink.contracts.messaging.Message.from("untransferred")) {
                // 02-channel-transport/02-channel-messaging.ko.md:172: readiness precedes binding
                // admission.
                var pending =
                        sockets.submitToChannel(
                                        "work",
                                        REQUEST_TIMEOUT,
                                        DEFAULT_TIMEOUT,
                                        (client, timeout) -> {
                                            throw new AssertionError(
                                                    "rejected admission must not submit");
                                        },
                                        (node, timeout) -> {
                                            throw new AssertionError();
                                        },
                                        failure -> {
                                            rejected.incrementAndGet();
                                            payload.close();
                                        })
                                .toCompletableFuture();
                org.junit.jupiter.api.Assertions.assertFalse(pending.isDone());
                if (cancel) assertTrue(pending.cancel(false));
                else assertThrows(java.util.concurrent.CompletionException.class, pending::join);
                assertTrue(payload.empty());
                assertEquals(1, rejected.get());
                assertTrue(sockets.admitClientServerConnection("server", descriptor()));
                assertEquals(1, rejected.get());
            } finally {
                sockets.closeAll();
            }
        }
    }

    @Test
    void readinessRejectionRestoresCapturedFlowAndSuppressesUncapturedAmbientFlow() {
        for (boolean captured : new boolean[] {true, false}) {
            var sockets = new ZLinkChannelSocketRegistry();
            setupClient(sockets);
            var original =
                    captured
                            ? systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
                                    .create(
                                            systems.zlink.framework.monitoring.ZLinkFlowOrigin
                                                    .APPLICATION)
                            : null;
            var ambient =
                    systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.create(
                            systems.zlink.framework.monitoring.ZLinkFlowOrigin.APPLICATION);
            var observed =
                    new AtomicReference<
                            systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
                                    .State>();
            var observedCalls = new java.util.concurrent.atomic.AtomicInteger();
            CompletableFuture<String> pending;
            try {
                try (var operation =
                        systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.enter(
                                original)) {
                    pending =
                            sockets.<String>submitToChannel(
                                            "work",
                                            REQUEST_TIMEOUT,
                                            DEFAULT_TIMEOUT,
                                            (client, timeout) -> {
                                                throw new AssertionError();
                                            },
                                            (node, timeout) -> {
                                                throw new AssertionError();
                                            },
                                            failure -> {
                                                observedCalls.incrementAndGet();
                                                observed.set(
                                                        systems.zlink.framework.runtime.internal
                                                                .diagnostics.ZLinkFlowContext
                                                                .current());
                                            })
                                    .toCompletableFuture();
                }
                // 06-observability/04-flow-correlation.ko.md:101: cancellation terminal uses the
                // captured operation flow.
                try (var caller =
                        systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.enter(
                                ambient)) {
                    assertTrue(pending.cancel(false));
                    assertEquals(1, observedCalls.get());
                    org.junit.jupiter.api.Assertions.assertSame(original, observed.get());
                    org.junit.jupiter.api.Assertions.assertSame(
                            ambient,
                            systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
                                    .current());
                }
            } finally {
                sockets.closeAll();
            }
        }
    }

    private static CompletionStage<String> submit(
            ZLinkChannelSocketRegistry sockets, AtomicReference<Duration> timeoutResult) {
        return submit(sockets, timeoutResult, "accepted");
    }

    private static CompletionStage<String> submit(
            ZLinkChannelSocketRegistry sockets,
            AtomicReference<Duration> timeoutResult,
            String operation) {
        return sockets.submitToChannel(
                "work",
                REQUEST_TIMEOUT,
                DEFAULT_TIMEOUT,
                (client, timeout) -> {
                    assertTrue(
                            timeoutResult.compareAndSet(null, timeout),
                            "one logical call must start only one binding operation");
                    return CompletableFuture.completedFuture(operation);
                },
                (node, timeout) -> {
                    throw new AssertionError("ClientServer must use its DEALER");
                },
                failure -> {});
    }

    private static void setupClient(ZLinkChannelSocketRegistry sockets) {
        ChannelRegistration registration =
                new ChannelRegistration("work", ChannelKind.CLIENT_SERVER);
        registration.enableClient();
        sockets.registerChannel(registration);
        sockets.addClientServerConnection("server", descriptor(), dealer());
    }

    private static ZLinkBackendDealerSocket dealer() {
        return (ZLinkBackendDealerSocket)
                java.lang.reflect.Proxy.newProxyInstance(
                        ZLinkBackendDealerSocket.class.getClassLoader(),
                        new Class<?>[] {ZLinkBackendDealerSocket.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("toString")) return "test-dealer";
                            if (method.getName().equals("hashCode")) {
                                return System.identityHashCode(proxy);
                            }
                            if (method.getName().equals("equals")) return proxy == arguments[0];
                            if (method.getName().equals("close")) return null;
                            return null;
                        });
    }

    private static ZLinkClientServerServerDescriptor descriptor() {
        return new ZLinkClientServerServerDescriptor(
                "work",
                RoutingId.from("server"),
                1,
                1,
                "tcp://127.0.0.1:10001",
                100,
                ZLinkFrameworkRuntimeState.SERVING,
                "default",
                "local",
                1,
                Instant.EPOCH);
    }
}
