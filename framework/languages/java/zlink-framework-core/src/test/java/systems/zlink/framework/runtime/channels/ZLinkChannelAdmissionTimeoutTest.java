package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.channels.ZLinkChannelAdmissionTimeout;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkChannelAdmissionTimeoutTest {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(5);
    private static final Duration DEFAULT_TIMEOUT = Duration.ofSeconds(10);

    @RepeatedTest(5)
    void clientSendAndRequestAdmitWhenServerBecomesReadyWithinSendTimeout() throws Exception {
        for (String operation : new String[] {"send", "request"}) {
            AtomicLong now = new AtomicLong();
            CountDownLatch waitingForServer = new CountDownLatch(1);
            CountDownLatch serverStarted = new CountDownLatch(1);
            AtomicReference<Duration> submittedTimeout = new AtomicReference<>();
            ZLinkChannelSocketRegistry sockets =
                    new ZLinkChannelSocketRegistry(
                            null,
                            now::get,
                            ignored -> {
                                waitingForServer.countDown();
                                try {
                                    serverStarted.await();
                                } catch (InterruptedException failure) {
                                    Thread.currentThread().interrupt();
                                    throw new AssertionError(failure);
                                }
                            });
            setupClient(sockets);
            ExecutorService executor = Executors.newSingleThreadExecutor();
            try {
                var submission =
                        executor.submit(
                                () ->
                                        sockets.submitToChannel(
                                                "work",
                                                REQUEST_TIMEOUT,
                                                DEFAULT_TIMEOUT,
                                                false,
                                                (dealer, timeout) -> {
                                                    submittedTimeout.set(timeout);
                                                    return CompletableFuture.completedFuture(
                                                            operation);
                                                },
                                                (node, timeout) -> {
                                                    throw new AssertionError(
                                                            "ClientServer must use its DEALER");
                                                }));

                assertTrue(waitingForServer.await(2, TimeUnit.SECONDS));
                now.set(TimeUnit.MILLISECONDS.toNanos(500));
                assertTrue(sockets.admitClientServerConnection("server", descriptor()));
                serverStarted.countDown();

                CompletionStage<String> accepted = submission.get(2, TimeUnit.SECONDS);
                assertEquals(operation, accepted.toCompletableFuture().get(2, TimeUnit.SECONDS));
                assertEquals(REQUEST_TIMEOUT, submittedTimeout.get());
            } catch (ExecutionException failure) {
                throw new AssertionError("Client " + operation + " failed", failure.getCause());
            } finally {
                serverStarted.countDown();
                executor.shutdownNow();
                sockets.closeAll();
            }
        }
    }

    @RepeatedTest(5)
    void clientSendAndRequestExpireBeforeServerStartsAfterSendTimeout() {
        for (String operation : new String[] {"send", "request"}) {
            AtomicLong now = new AtomicLong();
            AtomicReference<Duration> submittedTimeout = new AtomicReference<>();
            ZLinkChannelSocketRegistry sockets =
                    new ZLinkChannelSocketRegistry(
                            null,
                            now::get,
                            ignored ->
                                    now.set(
                                            ZLinkChannelAdmissionTimeout.DEFAULT_SEND_TIMEOUT
                                                    .toNanos()));
            setupClient(sockets);
            try {
                ZLinkFrameworkException failure =
                        assertThrows(
                                ZLinkFrameworkException.class,
                                () ->
                                        sockets.submitToChannel(
                                                "work",
                                                REQUEST_TIMEOUT,
                                                DEFAULT_TIMEOUT,
                                                false,
                                                (dealer, timeout) -> {
                                                    submittedTimeout.set(timeout);
                                                    return CompletableFuture.completedFuture(
                                                            operation);
                                                },
                                                (node, timeout) -> {
                                                    throw new AssertionError(
                                                            "ClientServer must use its DEALER");
                                                }));
                assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, failure.kind());

                assertTrue(sockets.admitClientServerConnection("server", descriptor()));

                assertNull(submittedTimeout.get());
            } finally {
                sockets.closeAll();
            }
        }
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
