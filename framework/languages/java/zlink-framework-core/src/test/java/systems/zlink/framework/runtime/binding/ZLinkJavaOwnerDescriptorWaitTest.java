package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Path;
import java.time.Duration;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;
import java.util.logging.FileHandler;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.logging.SimpleFormatter;

final class ZLinkJavaOwnerDescriptorWaitTest {
    private static FileHandler fileLog;
    private static Level previousLogLevel;

    @BeforeAll
    static void enableFileLog() throws Exception {
        var directory = System.getenv("ZLINK_DESCRIPTOR_WAIT_LOG_DIR");
        if (directory == null) return;
        var root = Logger.getLogger("");
        previousLogLevel = root.getLevel();
        fileLog = new FileHandler(Path.of(directory, "descriptor-wait-%u.log").toString());
        fileLog.setFormatter(new SimpleFormatter());
        fileLog.setLevel(Level.ALL);
        root.setLevel(Level.ALL);
        root.addHandler(fileLog);
    }

    @AfterAll
    static void closeFileLog() {
        if (fileLog == null) return;
        systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence.awaitIdle();
        var root = Logger.getLogger("");
        root.removeHandler(fileLog);
        fileLog.close();
        root.setLevel(previousLogLevel);
    }

    @Test
    void authoritySelectedOwnerDescriptorMayArriveAfterActorJoinStarts() throws Exception {
        try (var fixture = new Fixture()) {
            var join = fixture.join(Duration.ofSeconds(5));
            fixture.assertWaiting(join);

            fixture.publishDescriptor();

            ZLinkInternalMeshNode.CanonicalActorJoinReply reply;
            try {
                reply = join.get(3, TimeUnit.SECONDS);
            } catch (TimeoutException failure) {
                var observation =
                        "route="
                                + fixture.source.observePeerRoute(fixture.target.routingId())
                                + ", selected="
                                + fixture.source.selectedRoutes
                                + ", requests="
                                + fixture.bindingRequests
                                + ", received="
                                + fixture.received
                                + ", pending="
                                + fixture.source.operationRegistry().pendingCount();
                throw new AssertionError(observation, failure);
            }
            assertTrue(reply.accepted());
            assertEquals(1, fixture.bindingRequests.get());
            assertEquals(1, fixture.received.get());
            fixture.source
                    .sourceTransportLane()
                    .runNowOrQueue(() -> null)
                    .toCompletableFuture()
                    .join();
            assertEquals(0, fixture.source.operationRegistry().pendingCount());
        }
    }

    @Test
    void absentOwnerDescriptorExpiresAsDeadlineExceededWithoutSubmittingActorJoin()
            throws Exception {
        var clock = new AtomicLong(System.nanoTime());
        try (var fixture = new Fixture(clock::get)) {
            var join = fixture.join(Duration.ofMillis(100));
            fixture.assertWaiting(join);

            clock.addAndGet(Duration.ofSeconds(1).toNanos());

            assertFailure(join, ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED);
            assertEquals(0, fixture.bindingRequests.get());
            assertEquals(0, fixture.received.get());
            assertEquals(0, fixture.source.operationRegistry().pendingCount());
            fixture.publishDescriptor();
            assertFailure(join, ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED);
            assertEquals(0, fixture.bindingRequests.get());
            assertEquals(0, fixture.received.get());
        }
    }

    @Test
    void ownerFenceRemovalEndsDescriptorWaitAsUnavailableWithoutSubmittingActorJoin()
            throws Exception {
        try (var fixture = new Fixture()) {
            var join = fixture.join(Duration.ofSeconds(5));
            fixture.assertWaiting(join);

            fixture.source
                    .forgetPeerAdmissionExpectation(fixture.target.routingId())
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);

            assertFailure(join, ZLinkFrameworkErrorKind.UNAVAILABLE);
            assertEquals(0, fixture.bindingRequests.get());
            assertEquals(0, fixture.received.get());
            fixture.publishDescriptor();
            assertFailure(join, ZLinkFrameworkErrorKind.UNAVAILABLE);
            assertEquals(0, fixture.bindingRequests.get());
            assertEquals(0, fixture.received.get());
        }
    }

    private static void assertFailure(CompletableFuture<?> result, ZLinkFrameworkErrorKind kind) {
        var failure = assertThrows(ExecutionException.class, () -> result.get(3, TimeUnit.SECONDS));
        assertEquals(
                kind, assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    private static final class Fixture implements AutoCloseable {
        final Context context = Zlink.createContext();
        final AtomicInteger received = new AtomicInteger();
        final AtomicInteger bindingRequests = new AtomicInteger();
        final ZLinkJavaRawMeshNode source;
        final ZLinkJavaRawMeshNode target = new ZLinkJavaRawMeshNode(context, "descriptor-wait");

        Fixture() throws Exception {
            this(System::nanoTime);
        }

        Fixture(LongSupplier nanoTime) throws Exception {
            var sockets = new IdentityHashMap<Socket, Socket>();
            Context countingContext =
                    (Context)
                            Proxy.newProxyInstance(
                                    Context.class.getClassLoader(),
                                    new Class<?>[] {Context.class},
                                    (proxy, method, arguments) -> {
                                        if (!method.getName().equals("createRouterSocket"))
                                            return invoke(context, method, arguments);
                                        var nativeSocket =
                                                (RouterSocket) invoke(context, method, arguments);
                                        var counted =
                                                (RouterSocket)
                                                        Proxy.newProxyInstance(
                                                                RouterSocket.class.getClassLoader(),
                                                                new Class<?>[] {RouterSocket.class},
                                                                (socketProxy,
                                                                        socketMethod,
                                                                        socketArguments) -> {
                                                                    if (socketMethod
                                                                            .getName()
                                                                            .equals("hashCode"))
                                                                        return System
                                                                                .identityHashCode(
                                                                                        socketProxy);
                                                                    if (socketMethod
                                                                            .getName()
                                                                            .equals("equals"))
                                                                        return socketProxy
                                                                                == socketArguments[
                                                                                        0];
                                                                    if (socketMethod
                                                                            .getName()
                                                                            .equals("request"))
                                                                        bindingRequests
                                                                                .incrementAndGet();
                                                                    return invoke(
                                                                            nativeSocket,
                                                                            socketMethod,
                                                                            socketArguments);
                                                                });
                                        sockets.put(counted, nativeSocket);
                                        return counted;
                                    });
            source =
                    new ZLinkJavaRawMeshNode(
                            countingContext,
                            "descriptor-wait",
                            System::currentTimeMillis,
                            nanoTime,
                            ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                            ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT,
                            (socket, observesRoutes) ->
                                    new ZLinkJavaSocketReceivePoller(
                                            socket,
                                            true,
                                            observesRoutes,
                                            () -> countingPoller(sockets),
                                            observesRoutes ? Zlink::createTimer : () -> null));
            source.setRoutingId(RoutingId.from("descriptor-wait-source"));
            source.setBind("inproc://descriptor-wait-source-" + UUID.randomUUID());
            source.setObjectRole(ZLinkMeshNodeObjectRole.SERVER);
            target.setRoutingId(RoutingId.from("descriptor-wait-target"));
            target.setBind("inproc://descriptor-wait-target-" + UUID.randomUUID());
            target.setObjectRole(ZLinkMeshNodeObjectRole.SERVER);
            target.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    Optional.of(
                                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                    rid, generation, "source-owner", 1))));
            target.setCanonicalActorJoinHandler(
                    (peer, request) -> {
                        received.incrementAndGet();
                        return CompletableFuture.completedFuture(
                                new ZLinkInternalMeshNode.CanonicalActorJoinResponse(
                                        true, 7, List.of()));
                    });
            var options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            source.setDispatchErrorReporter(
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run));
            target.setDispatchErrorReporter(
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run));
            source.start();
            target.start();
            // The authority-selected lifecycle is known before discovery publishes its descriptor.
            source.observePeerAdmissionExpectation(
                            target.routingId(),
                            target.advertisedEndpoint(),
                            target.lifecycleGeneration(),
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            ZLinkMeshNodeObjectRole.SERVER)
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
        }

        CompletableFuture<ZLinkInternalMeshNode.CanonicalActorJoinReply> join(Duration timeout) {
            return source.requestCanonicalActorJoin(
                            new ZLinkInternalMeshNode.CanonicalActorJoinRequest(
                                    new ZLinkBackendActorRef(source.routingId(), "actor", 3),
                                    source.lifecycleGeneration(),
                                    1,
                                    1,
                                    target.routingId(),
                                    target.lifecycleGeneration(),
                                    "target-spot",
                                    5,
                                    9,
                                    11,
                                    false,
                                    "ZLinkFrameworkActorJoinRequest",
                                    "application/json",
                                    new byte[] {'{', '}'}),
                            timeout)
                    .toCompletableFuture();
        }

        void assertWaiting(CompletableFuture<?> join) throws Exception {
            source.sourceTransportLane()
                    .runNowOrQueue(() -> null)
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            assertFalse(join.isDone());
            assertTrue(source.peers().isEmpty());
            assertEquals(1, source.operationRegistry().pendingCount());
            assertEquals(0, bindingRequests.get());
            assertEquals(0, received.get());
        }

        void publishDescriptor() throws Exception {
            source.preparePeerConnectionAsync(
                            target.advertisedEndpoint(),
                            target.routingId(),
                            target.lifecycleGeneration(),
                            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                            Duration.ofSeconds(2))
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
        }

        private static Poller countingPoller(Map<Socket, Socket> sockets) {
            var nativePoller = Zlink.createPoller();
            return (Poller)
                    Proxy.newProxyInstance(
                            Poller.class.getClassLoader(),
                            new Class<?>[] {Poller.class},
                            (proxy, method, arguments) -> {
                                if (arguments != null
                                        && arguments.length > 0
                                        && arguments[0] instanceof Socket socket)
                                    arguments[0] = sockets.getOrDefault(socket, socket);
                                return invoke(nativePoller, method, arguments);
                            });
        }

        private static Object invoke(Object target, Method method, Object[] arguments)
                throws Throwable {
            try {
                return method.invoke(target, arguments);
            } catch (InvocationTargetException failure) {
                throw failure.getCause();
            }
        }

        @Override
        public void close() {
            source.close();
            target.close();
            context.close();
        }
    }
}
