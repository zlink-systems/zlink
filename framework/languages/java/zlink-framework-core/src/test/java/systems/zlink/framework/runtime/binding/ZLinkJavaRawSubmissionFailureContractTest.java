package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.*;
import systems.zlink.contracts.errors.*;
import systems.zlink.contracts.eventing.*;
import systems.zlink.contracts.messaging.*;
import systems.zlink.contracts.sockets.*;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaRawSubmissionFailureContractTest {
    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void rawNodeAndSpotUnexpectedSubmissionFailure(boolean spot, boolean asynchronous)
            throws Exception {
        try (Fixture fixture = new Fixture()) {
            RuntimeException original = new RuntimeException("native request failed");
            Throwable failure = fixture.failure(spot, asynchronous, original);
            var framework = assertInstanceOf(ZLinkFrameworkException.class, failure);
            assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, framework.kind());
            assertSame(original, framework.getCause());
        }
    }

    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void rawNodeAndSpotKnownSubmissionFailure(boolean spot, boolean asynchronous) throws Exception {
        try (Fixture fixture = new Fixture()) {
            var standard =
                    List.of(
                            new IllegalArgumentException("argument"),
                            new NullPointerException("argument"),
                            new IllegalStateException("state"),
                            new UnsupportedOperationException("unsupported"),
                            new java.util.concurrent.CancellationException("cancelled"),
                            new ZlinkConfigException(ConfigResult.INVALID_ARGUMENT),
                            new ZLinkConfigurationException("configuration"));
            for (var original : standard) {
                assertSame(original, fixture.failure(spot, asynchronous, original));
            }
            for (var kind : ZLinkFrameworkErrorKind.values()) {
                var original = new ZLinkFrameworkException(kind, "typed");
                assertSame(original, fixture.failure(spot, asynchronous, original));
            }
            RequestResult[] results = {
                RequestResult.TIMED_OUT,
                RequestResult.NOT_FOUND,
                RequestResult.TERMINATED,
                RequestResult.PROTOCOL_ERROR,
                RequestResult.INTERNAL_ERROR,
                RequestResult.REJECTED,
                RequestResult.CONFLICT,
                RequestResult.BUSY,
                RequestResult.NOT_CONNECTED,
                RequestResult.INVALID_ARGUMENT,
                RequestResult.INVALID_STATE,
                RequestResult.NOT_SUPPORTED,
                RequestResult.BACKPRESSURED
            };
            ZLinkFrameworkErrorKind[] kinds = {
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, ZLinkFrameworkErrorKind.NOT_FOUND,
                ZLinkFrameworkErrorKind.SHUTTING_DOWN, ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE, ZLinkFrameworkErrorKind.REJECTED,
                ZLinkFrameworkErrorKind.UNAVAILABLE, ZLinkFrameworkErrorKind.UNAVAILABLE,
                ZLinkFrameworkErrorKind.UNAVAILABLE, ZLinkFrameworkErrorKind.INVALID_OPERATION,
                ZLinkFrameworkErrorKind.INVALID_OPERATION, ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                ZLinkFrameworkErrorKind.UNAVAILABLE
            };
            for (int i = 0; i < results.length; i++) {
                var original = new ZlinkRequestException(results[i]);
                var failure =
                        assertInstanceOf(
                                ZLinkFrameworkException.class,
                                fixture.failure(spot, asynchronous, original));
                assertEquals(kinds[i], failure.kind(), results[i].name());
                assertSame(original, failure.getCause());
            }
            var original = new ZlinkSubmitException(SubmitResult.NOT_CONNECTED);
            var failure =
                    assertInstanceOf(
                            ZLinkFrameworkException.class,
                            fixture.failure(spot, asynchronous, original));
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, failure.kind());
            assertSame(original, failure.getCause());
        }
    }

    private static final class Fixture implements AutoCloseable {
        private final Context nativeContext = Zlink.createContext();
        private final FaultContext context = new FaultContext(nativeContext);
        private final ZLinkJavaRawMeshNode source =
                new ZLinkJavaRawMeshNode(
                        context,
                        "mesh",
                        System::currentTimeMillis,
                        System::nanoTime,
                        ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                        ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT,
                        (socket, observes) ->
                                new ZLinkJavaSocketReceivePoller(
                                        socket,
                                        true,
                                        observes,
                                        FaultPoller::new,
                                        Zlink::createTimer));
        private final ZLinkJavaRawMeshNode target = new ZLinkJavaRawMeshNode(nativeContext, "mesh");
        private final RoutingId targetRid = RoutingId.from("failure-target");
        private final ZLinkBackendSpot spot;

        Fixture() throws Exception {
            boolean initialized = false;
            try {
                source.setRoutingId(RoutingId.from("failure-source"));
                target.setRoutingId(targetRid);
                source.setBind("inproc://failure-source-" + java.util.UUID.randomUUID());
                target.setBind("inproc://failure-target-" + java.util.UUID.randomUUID());
                source.start();
                target.start();
                var admitted = new CompletableFuture<Void>();
                try (var registration =
                        source.onPeerStateChanged(
                                targetRid,
                                () -> {
                                    if (source.peers().stream()
                                            .anyMatch(p -> p.state() == MeshPeerState.ADMITTED)) {
                                        admitted.complete(null);
                                    }
                                },
                                () -> {})) {
                    source.connectPeer(target.status().localEndpoint(), targetRid);
                    admitted.get(5, TimeUnit.SECONDS);
                }
                var spots = (ZLinkJavaRawSpotNode) source.spotNode();
                spot = spots.createSpot("source-spot", 1);
                spots.rememberSpotAuthority(targetRid, "target-spot", 1, 1, 1);
                initialized = true;
            } finally {
                if (!initialized) close();
            }
        }

        Throwable failure(boolean toSpot, boolean asynchronous, RuntimeException original)
                throws Exception {
            var router = context.router;
            int before = router.attempts.get();
            router.original = original;
            router.asynchronous = asynchronous;
            try (Message payload = Message.from("request")) {
                var admission = new CompletableFuture<Void>();
                var request =
                        source.inSourceTurn(
                                () ->
                                        toSpot
                                                ? spot.requestToSpot(
                                                        targetRid,
                                                        "target-spot",
                                                        1,
                                                        new byte[0],
                                                        List.of(payload),
                                                        Duration.ofSeconds(5),
                                                        admission)
                                                : source.spotNode()
                                                        .requestToNode(
                                                                targetRid,
                                                                new byte[0],
                                                                List.of(payload),
                                                                Duration.ofSeconds(5),
                                                                null,
                                                                null,
                                                                admission));
                if (asynchronous) {
                    boolean admitted = admission.isDone();
                    boolean completed = request.toCompletableFuture().isDone();
                    router.pendingAdmission.completeExceptionally(original);
                    router.pending.completeExceptionally(new CompletionException(original));
                    assertFalse(admitted, "binding failure must precede admission");
                    assertFalse(completed);
                }
                Throwable failure =
                        request.handle(
                                        (value, error) -> {
                                            if (value != null) value.close();
                                            assertNull(
                                                    value,
                                                    "a binding failure must not become a terminal reply without its cause");
                                            assertNotNull(error);
                                            return error;
                                        })
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS);
                assertEquals(
                        before + 1, router.attempts.get(), "one native submission per request");
                while (failure instanceof CompletionException && failure.getCause() != null)
                    failure = failure.getCause();
                return failure;
            } finally {
                router.original = null;
            }
        }

        @Override
        public void close() {
            try {
                source.close();
            } finally {
                try {
                    target.close();
                } finally {
                    nativeContext.close();
                }
            }
        }
    }

    /**
     * Keeps native receive, admission, and peer control; replaces only the binding request
     * terminal.
     */
    private static final class FaultRouter implements RouterSocket {
        private final RouterSocket nativeSocket;
        private final AtomicInteger attempts = new AtomicInteger();
        private RuntimeException original;
        private boolean asynchronous;
        private CompletableFuture<List<Message>> pending;
        private CompletableFuture<Void> pendingAdmission;

        FaultRouter(RouterSocket nativeSocket) {
            this.nativeSocket = nativeSocket;
        }

        @Override
        public RequestOperation request(RoutingId rid) {
            if (original == null) return nativeSocket.request(rid);
            return new FaultRequest(this);
        }

        @Override
        public void bind(String v) {
            nativeSocket.bind(v);
        }

        @Override
        public void connect(String v) {
            nativeSocket.connect(v);
        }

        @Override
        public void unbind(String v) {
            nativeSocket.unbind(v);
        }

        @Override
        public void disconnect(String v) {
            nativeSocket.disconnect(v);
        }

        @Override
        public void disconnectRid(RoutingId v) {
            nativeSocket.disconnectRid(v);
        }

        @Override
        public void setRoutingId(RoutingId v) {
            nativeSocket.setRoutingId(v);
        }

        @Override
        public RoutingId getRoutingId() {
            return nativeSocket.getRoutingId();
        }

        @Override
        public List<RouterRoute> routesSnapshot() {
            return nativeSocket.routesSnapshot();
        }

        @Override
        public SendOperation send(RoutingId v) {
            return nativeSocket.send(v);
        }

        @Override
        public boolean recv(Received v, RecvFlags f) {
            return nativeSocket.recv(v, f);
        }

        @Override
        public ReplyOperation reply(RoutingId v, ReplyToken t) {
            return nativeSocket.reply(v, t);
        }

        @Override
        public RouterSocketOptions options() {
            return nativeSocket.options();
        }

        @Override
        public SocketMonitor monitorOpen() {
            return nativeSocket.monitorOpen();
        }

        @Override
        public SocketMonitor monitorOpen(MonitorEventType... e) {
            return nativeSocket.monitorOpen(e);
        }

        @Override
        public SocketMonitor monitorOpen(long h, MonitorEventType... e) {
            return nativeSocket.monitorOpen(h, e);
        }

        @Override
        public void setTlsServer(String c, String k, boolean r) {
            nativeSocket.setTlsServer(c, k, r);
        }

        @Override
        public void setTlsClient(String c, String h, boolean t) {
            nativeSocket.setTlsClient(c, h, t);
        }

        @Override
        public void close() {
            nativeSocket.close();
        }
    }

    private static final class FaultRequest implements RequestOperation, RequestSubmitOperation {
        private final FaultRouter router;
        private final List<Message> parts = new ArrayList<>();

        FaultRequest(FaultRouter router) {
            this.router = router;
        }

        @Override
        public RequestSubmitOperation message(Message part) {
            parts.add(part);
            return this;
        }

        @Override
        public RequestSubmitOperation timeout(Duration timeout) {
            return this;
        }

        @Override
        public RequestSubmission submit() {
            router.attempts.incrementAndGet();
            if (!router.asynchronous) throw router.original;
            Message.closeAll(parts);
            router.pending = new CompletableFuture<>();
            router.pendingAdmission = new CompletableFuture<>();
            return new RequestSubmission() {
                @Override
                public SubmitResult result() {
                    return SubmitResult.BACKPRESSURED;
                }

                @Override
                public CompletionStage<Void> admitted() {
                    return router.pendingAdmission;
                }

                @Override
                public CompletionStage<List<Message>> reply() {
                    return router.pending;
                }
            };
        }

        @Override
        public List<Message> submit_sync() {
            throw new AssertionError("async request expected");
        }
    }

    private static final class FaultContext implements Context {
        private final Context nativeContext;
        private FaultRouter router;

        FaultContext(Context nativeContext) {
            this.nativeContext = nativeContext;
        }

        @Override
        public RouterSocket createRouterSocket() {
            router = new FaultRouter(nativeContext.createRouterSocket());
            return router;
        }

        @Override
        public ContextOptions options() {
            return nativeContext.options();
        }

        @Override
        public PairSocket createPairSocket() {
            return nativeContext.createPairSocket();
        }

        @Override
        public DealerSocket createDealerSocket() {
            return nativeContext.createDealerSocket();
        }

        @Override
        public PubSocket createPubSocket() {
            return nativeContext.createPubSocket();
        }

        @Override
        public SubSocket createSubSocket() {
            return nativeContext.createSubSocket();
        }

        @Override
        public XPubSocket createXPubSocket() {
            return nativeContext.createXPubSocket();
        }

        @Override
        public XSubSocket createXSubSocket() {
            return nativeContext.createXSubSocket();
        }

        @Override
        public StreamSocket createStreamSocket() {
            return nativeContext.createStreamSocket();
        }

        @Override
        public void shutdown() {
            nativeContext.shutdown();
        }

        @Override
        public void recalculateAutoHwm() {
            nativeContext.recalculateAutoHwm();
        }

        @Override
        public CoreHwmBudgetSnapshot coreHwmBudgetSnapshot() {
            return nativeContext.coreHwmBudgetSnapshot();
        }

        @Override
        public void resetCoreHwmBudgetMetrics() {
            nativeContext.resetCoreHwmBudgetMetrics();
        }

        @Override
        public void close() {
            nativeContext.close();
        }
    }

    private static final class FaultPoller implements Poller {
        private final Poller nativePoller = Zlink.createPoller();

        private Socket nativeSocket(Socket socket) {
            return socket instanceof FaultRouter router ? router.nativeSocket : socket;
        }

        @Override
        public void add(Socket s, long slot, PollEventFlags... f) {
            nativePoller.add(nativeSocket(s), slot, f);
        }

        @Override
        public void add(SocketMonitor m, long slot, PollEventFlags... f) {
            nativePoller.add(m, slot, f);
        }

        @Override
        public void addFd(int fd, long slot, PollEventFlags... f) {
            nativePoller.addFd(fd, slot, f);
        }

        @Override
        public void add(ZlinkTimer t, long slot) {
            nativePoller.add(t, slot);
        }

        @Override
        public void modify(Socket s, PollEventFlags... f) {
            nativePoller.modify(nativeSocket(s), f);
        }

        @Override
        public void modify(SocketMonitor m, PollEventFlags... f) {
            nativePoller.modify(m, f);
        }

        @Override
        public void modifyFd(int fd, PollEventFlags... f) {
            nativePoller.modifyFd(fd, f);
        }

        @Override
        public boolean remove(Socket s) {
            return nativePoller.remove(nativeSocket(s));
        }

        @Override
        public boolean remove(SocketMonitor m) {
            return nativePoller.remove(m);
        }

        @Override
        public boolean remove(int fd) {
            return nativePoller.remove(fd);
        }

        @Override
        public boolean remove(ZlinkTimer t) {
            return nativePoller.remove(t);
        }

        @Override
        public void clear() {
            nativePoller.clear();
        }

        @Override
        public int size() {
            return nativePoller.size();
        }

        @Override
        public int wait(PollEvents e, Duration t) {
            return nativePoller.wait(e, t);
        }

        @Override
        public void close() {
            nativePoller.close();
        }
    }
}
