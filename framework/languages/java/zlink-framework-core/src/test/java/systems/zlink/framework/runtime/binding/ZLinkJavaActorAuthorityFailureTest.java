package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReceivedTesting;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.ReplySubmitOperation;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

/** Exercises authority failures through the actor ingress without constructing a socket. */
final class ZLinkJavaActorAuthorityFailureTest {
    @Test
    void unexpectedFailureRetainsInternalFailureAndNextMessageIsChecked() throws Exception {
        int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.INTERNAL_FAILURE);
        verifyFailure(new IllegalStateException("authority provider failed"), pair[0], pair[1]);
    }

    @Test
    void cancellationUsesTerminatedAndNextMessageIsChecked() throws Exception {
        int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.SHUTTING_DOWN);
        verifyFailure(new CancellationException("authority provider cancelled"), pair[0], pair[1]);
    }

    @Test
    void classifiedFailureRetainsDeadlineAndNextMessageIsChecked() throws Exception {
        int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED);
        verifyFailure(
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, "authority deadline"),
                pair[0],
                pair[1]);
    }

    @Test
    void synchronousFailureUsesTheSameMessageTerminal() throws Exception {
        int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.INTERNAL_FAILURE);
        verifyFailure(
                new IllegalStateException("synchronous authority failed"), pair[0], pair[1], true);
    }

    @Test
    void synchronousCancellationUsesTheSameMessageTerminal() throws Exception {
        int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.SHUTTING_DOWN);
        verifyFailure(
                new CancellationException("synchronous authority cancelled"),
                pair[0],
                pair[1],
                true);
    }

    @Test
    void providerArgumentFailureIsInternalRatherThanWireProtocolFailure() throws Exception {
        int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.INTERNAL_FAILURE);
        verifyFailure(
                new IllegalArgumentException("authority provider argument failed"),
                pair[0],
                pair[1]);
    }

    @Test
    void synchronousProviderArgumentFailureIsInternal() throws Exception {
        int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.INTERNAL_FAILURE);
        verifyFailure(
                new IllegalArgumentException("synchronous authority argument failed"),
                pair[0],
                pair[1],
                true);
    }

    @Test
    void oneWayProviderArgumentFailureRecordsOriginalCause() throws Exception {
        verifyOneWay(new IllegalArgumentException("one-way authority argument failed"));
    }

    @Test
    void absentAuthorityRemainsUnavailable() throws Exception {
        try (var fixture = new Fixture()) {
            var inbound = fixture.dispatch(1);
            fixture.authority.complete(Optional.empty());
            int[] pair = ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.UNAVAILABLE);
            assertEquals(
                    List.of(new ZLinkServiceM6AWireCodec.Reply(1, pair[0], pair[1])),
                    fixture.replies);
            assertTrue(ReceivedTesting.isClosed(inbound.received()));
            assertEquals(2, fixture.lookups.get());
            assertEquals(0, fixture.reporter.reportedCount());
        }
    }

    @Test
    void oneWayFailureRecordsOriginalCauseWithoutReply() throws Exception {
        verifyOneWay(new IllegalStateException("one-way authority failed"));
    }

    @Test
    void oneWayCancellationRecordsOriginalCauseWithoutReply() throws Exception {
        verifyOneWay(new CancellationException("one-way authority cancelled"));
    }

    private static void verifyOneWay(Throwable failure) throws Exception {
        try (var fixture = new Fixture()) {
            var inbound = fixture.dispatch(1, false);
            fixture.authority.completeExceptionally(failure);
            assertTrue(fixture.replies.isEmpty());
            assertTrue(ReceivedTesting.isClosed(inbound.received()));
            fixture.assertReported(failure, "drop");
            fixture.authority = CompletableFuture.completedFuture(Optional.empty());
            fixture.dispatch(2);
            assertEquals(4, fixture.lookups.get());
            assertEquals(1, fixture.replies.size());
        }
    }

    private static void verifyFailure(Throwable failure, int terminal, int code) throws Exception {
        verifyFailure(failure, terminal, code, false);
    }

    private static void verifyFailure(
            Throwable failure, int terminal, int code, boolean synchronous) throws Exception {
        try (var fixture = new Fixture()) {
            fixture.synchronousFailure = synchronous ? (RuntimeException) failure : null;
            var failed = fixture.dispatch(1);
            if (!synchronous) fixture.authority.completeExceptionally(failure);
            assertEquals(
                    List.of(new ZLinkServiceM6AWireCodec.Reply(1, terminal, code)),
                    fixture.replies);
            assertTrue(ReceivedTesting.isClosed(failed.received()));
            fixture.assertReported(failure, "reply_error");
            fixture.synchronousFailure = null;
            fixture.authority = CompletableFuture.completedFuture(Optional.empty());
            var suffix = fixture.dispatch(2);
            assertEquals(
                    synchronous ? 3 : 4,
                    fixture.lookups.get(),
                    "the next message still resolves both authorities");
            int[] unavailable =
                    ZLinkRequestFailureMapping.outgoing(ZLinkFrameworkErrorKind.UNAVAILABLE);
            assertEquals(
                    new ZLinkServiceM6AWireCodec.Reply(2, unavailable[0], unavailable[1]),
                    fixture.replies.get(1));
            assertTrue(ReceivedTesting.isClosed(suffix.received()));
        }
    }

    private static final class Fixture implements AutoCloseable {
        final RoutingId source = RoutingId.from("authority-source");
        final RoutingId target = RoutingId.from("authority-target");
        final ZLinkServiceM6AWireCodec wire = new ZLinkServiceM6AWireCodec();
        final ZLinkServiceM6BWireCodec actors = new ZLinkServiceM6BWireCodec();
        final List<ZLinkServiceM6AWireCodec.Reply> replies = new ArrayList<>();
        final AtomicInteger lookups = new AtomicInteger();
        CompletableFuture<Optional<ZLinkInternalMeshNode.PeerAuthorityFence>> authority =
                new CompletableFuture<>();
        RuntimeException synchronousFailure;
        final ZLinkJavaRawMeshNode node;
        final RouterSocket router;
        final ReplyToken token;
        final ZLinkDispatchErrorReporter reporter;
        final List<String> records = new ArrayList<>();
        final Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        final Level previousLevel = logger.getLevel();
        final Handler logHandler =
                new Handler() {
                    public void publish(LogRecord record) {
                        records.add(record.getMessage());
                    }

                    public void flush() {}

                    public void close() {}
                };

        Fixture() throws Exception {
            ReplySubmitOperation submit =
                    (ReplySubmitOperation)
                            Proxy.newProxyInstance(
                                    ReplySubmitOperation.class.getClassLoader(),
                                    new Class<?>[] {ReplySubmitOperation.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("submit")) return null;
                                        if (method.getName().equals("message")) return proxy;
                                        throw new AssertionError(method.getName());
                                    });
            ReplyOperation reply =
                    message -> {
                        replies.add(wire.decodeReplyHeader(message.toByteArray()));
                        return submit;
                    };
            router =
                    (RouterSocket)
                            Proxy.newProxyInstance(
                                    RouterSocket.class.getClassLoader(),
                                    new Class<?>[] {RouterSocket.class},
                                    (proxy, method, args) -> {
                                        return switch (method.getName()) {
                                            case "reply" -> reply;
                                            case "setRoutingId" -> null;
                                            case "hashCode" -> System.identityHashCode(proxy);
                                            case "equals" -> proxy == args[0];
                                            default -> throw new AssertionError(method.getName());
                                        };
                                    });
            Context context =
                    (Context)
                            Proxy.newProxyInstance(
                                    Context.class.getClassLoader(),
                                    new Class<?>[] {Context.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("createRouterSocket"))
                                            return router;
                                        throw new AssertionError(
                                                "unexpected context call: " + method.getName());
                                    });
            token = ReceivedTesting.replyToken(router, 1L);
            var descriptor = descriptor(target);
            var topology = new ZLinkServiceTopologyRegistry(descriptor);
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    topology.admit(descriptor(source), "1", ServiceWireConstants.COMMAND_HELLO));
            var port =
                    new ZLinkJavaRawServicePort(
                            context,
                            new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane(),
                            (socket, observesRoutes) -> null);
            assertSame(router, port.openRouter(target));
            node =
                    ZLinkJavaRawMeshNodeTesting.create(
                            "authority-mesh",
                            port,
                            router,
                            descriptor,
                            topology,
                            Map.of(),
                            Map.of(),
                            Runnable::run);
            node.setRoutingId(target);
            var options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            reporter =
                    new ZLinkDispatchErrorReporter(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run);
            node.setDispatchErrorReporter(reporter);
            logger.addHandler(logHandler);
            logger.setLevel(Level.ALL);
            node.setPeerAuthorityResolver(
                    (mesh, rid, generation) -> {
                        lookups.incrementAndGet();
                        if (synchronousFailure != null) throw synchronousFailure;
                        return authority;
                    });
        }

        ZLinkJavaRawServicePort.Inbound dispatch(long correlation) throws Exception {
            return dispatch(correlation, true);
        }

        ZLinkJavaRawServicePort.Inbound dispatch(long correlation, boolean request)
                throws Exception {
            var header =
                    new ZLinkServiceM6BWireCodec.ActorMessage(
                            request,
                            0,
                            request ? correlation : null,
                            null,
                            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                    new ZLinkBackendActorRef(target, "actor", 1), 1, 1, 1));
            byte[] payload;
            try (var message = Message.from("{}")) {
                payload = ZLinkServiceM6AWireCodec.encodeFrameworkMultipartFrame(List.of(message));
            }
            var received = new Received();
            ReceivedTesting.setRouteGeneration(received, 1);
            var inbound =
                    new ZLinkJavaRawServicePort.Inbound(
                            source,
                            request ? token : null,
                            List.of(
                                    actors.encodeActorHeader(
                                            request,
                                            0,
                                            request ? correlation : null,
                                            1,
                                            1,
                                            0,
                                            null,
                                            header.target()),
                                    payload),
                            received);
            node.dispatch(inbound);
            return inbound;
        }

        void assertReported(Throwable failure, String action) {
            assertEquals(1, reporter.reportedCount());
            assertTrue(
                    records.stream()
                            .anyMatch(
                                    record ->
                                            record.contains(failure.getClass().getSimpleName())
                                                    && record.contains(failure.getMessage())
                                                    && record.contains("action=" + action)),
                    records.toString());
        }

        public void close() {
            logger.removeHandler(logHandler);
            logger.setLevel(previousLevel);
        }

        ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
            return new ZLinkServiceNodeDescriptor(
                    "authority-mesh",
                    rid,
                    1,
                    1,
                    "inproc://authority-test",
                    List.of(),
                    ZLinkServiceNodeDescriptor.State.SERVING,
                    "default",
                    0,
                    List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                    ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                    1,
                    128,
                    128,
                    0,
                    0);
        }
    }
}
