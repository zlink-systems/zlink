package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkNodeSubmitTurnTest {
    private static final String CHANNEL = "orders";
    private static final RoutingId TARGET = RoutingId.from("target-node");
    private static final Duration DEFAULT_TIMEOUT = Duration.ofSeconds(30);
    private static final IllegalStateException TERMINAL =
        new IllegalStateException("test terminal");

    @Test
    void buildersResolveTheNodeAndTimeoutAtSubmitAndPreserveCallOptions() throws Exception {
        try (Fixture f = new Fixture()) {
            NodeProbe oldNode = new NodeProbe(f.lane);
            NodeProbe newNode = new NodeProbe(f.lane);
            f.register(Duration.ofSeconds(2));
            f.runtime.registerSpotRouterNode(CHANNEL, oldNode.node);

            ZLinkRequestCall request = f.request().metadata("tenant", "blue");
            ZLinkSendCall send = f.send().metadata(Map.of("tenant", "blue"));

            f.register(Duration.ofSeconds(7));
            f.runtime.registerSpotRouterNode(CHANNEL, newNode.node);

            assertTerminal(request.submit(String.class));
            send.submit().toCompletableFuture().join();
            assertEquals(0, oldNode.nodeCalls());
            assertEquals(2, newNode.nodeCalls());
            assertEquals(Duration.ofSeconds(7), newNode.timeout.get());
            assertEquals(Map.of("tenant", "blue"), newNode.metadata.get());

            assertTerminal(f.request().timeout(Duration.ofSeconds(3)).submit(String.class));
            assertEquals(Duration.ofSeconds(3), newNode.timeout.get());

            assertThrows(CompletionException.class,
                () -> request.submit(String.class).toCompletableFuture().join());
            assertThrows(CompletionException.class,
                () -> send.submit().toCompletableFuture().join());
        }
    }

    @Test
    void buildersCanPrecedeNodeRegistrationAndMissingRoutesKeepTheirErrorType() throws Exception {
        try (Fixture f = new Fixture()) {
            ZLinkRequestCall request = f.request();
            ZLinkSendCall send = f.send();
            NodeProbe node = new NodeProbe(f.lane);
            f.runtime.registerSpotRouterNode(CHANNEL, node.node);

            assertTerminal(request.submit(String.class));
            send.submit().toCompletableFuture().join();
            assertEquals(2, node.nodeCalls());
            assertEquals(DEFAULT_TIMEOUT, node.timeout.get());

            ZLinkRequestCall missingRequest = f.runtime.requestToNode(
                "missing", TARGET, "request");
            ZLinkSendCall missingSend = f.runtime.sendToNode(
                "missing", TARGET, "send");
            assertThrows(ZLinkConfigurationException.class,
                () -> missingRequest.submit(String.class));
            assertThrows(ZLinkConfigurationException.class, missingSend::submit);
        }
    }

    @Test
    void routeAndMetadataRejectionReleaseTheSingleUsePayload() throws Exception {
        for (boolean configuredRouter : new boolean[] {false, true}) {
            try (Fixture f = new Fixture()) {
                if (configuredRouter) {
                    RouterProbe router = new RouterProbe(f.lane);
                    f.sockets.registerRouteRouter(CHANNEL, router.router);
                }
                ZLinkSendCall send = f.send().metadata(Map.of());
                ZLinkRequestCall request = f.request().metadata(Map.of());
                Message sendPayload = (Message) field(send, "payload");
                Message requestPayload = (Message) field(request, "payload");
                if (configuredRouter) {
                    assertThrows(UnsupportedOperationException.class, send::submit);
                    assertInstanceOf(UnsupportedOperationException.class,
                        assertThrows(CompletionException.class,
                            () -> request.submit(String.class).toCompletableFuture().join()).getCause());
                } else {
                    assertThrows(ZLinkConfigurationException.class, send::submit);
                    assertThrows(ZLinkConfigurationException.class, () -> request.submit(String.class));
                }
                assertTrue(sendPayload.empty(), "terminal send rejection must release encoded payload");
                assertTrue(requestPayload.empty(), "terminal request rejection must release encoded payload");
                assertThrows(CompletionException.class, () -> send.submit().toCompletableFuture().join());
                assertThrows(CompletionException.class,
                    () -> request.submit(String.class).toCompletableFuture().join());
            }
        }
    }

    @Test
    void explicitNullTimeoutIsRejectedWithoutSubmittingToBinding() throws Exception {
        try (Fixture f = new Fixture(); Message part = Message.from("spot")) {
            NodeProbe node = new NodeProbe(f.lane);
            f.runtime.registerSpotRouterNode(CHANNEL, node.node);
            assertThrows(NullPointerException.class, () -> f.request().timeout(null));
            assertInstanceOf(NullPointerException.class,
                assertThrows(CompletionException.class,
                    () -> f.runtime.requestToSpotViaRouterChannel(
                            CHANNEL, TARGET, "target-spot", List.of(part), null)
                        .toCompletableFuture().join()).getCause());
            assertEquals(0, node.nodeCalls());
            assertEquals(0, node.spotCalls());
        }
    }

    @Test
    void nodeRequestAndSendStartBindingInTheRegistryTurnIncludingAnExistingTurn()
        throws Exception {
        for (boolean alreadyOnLane : new boolean[] {false, true}) {
            try (Fixture f = new Fixture()) {
                NodeProbe node = new NodeProbe(f.lane);
                node.onNodeBinding = () -> assertSame(f.lane, ZLinkStateLane.current());
                f.runtime.registerSpotRouterNode(CHANNEL, node.node);

                CompletionStage<String> reply = alreadyOnLane
                    ? f.lane.runAsync(() -> f.request().submit(String.class))
                        .toCompletableFuture().join()
                    : f.request().submit(String.class);
                CompletionStage<Void> admission = alreadyOnLane
                    ? f.lane.runAsync(() -> f.send().submit())
                        .toCompletableFuture().join()
                    : f.send().submit();

                assertTerminal(reply);
                admission.toCompletableFuture().join();
                assertEquals(2, node.nodeCalls());
            }
        }
    }

    @Test
    void nodeAndDirectSpotSubmissionsUseExactlyOneRegistryTurn() throws Exception {
        for (boolean alreadyOnLane : new boolean[] {false, true}) {
            CountingDirectExecutor executor = new CountingDirectExecutor();
            try (Fixture f = new Fixture(executor);
                 Message requestPart = Message.from("spot-request");
                 Message sendPart = Message.from("spot-send")) {
                NodeProbe node = new NodeProbe(f.lane);
                f.runtime.registerSpotRouterNode(CHANNEL, node.node);

                CompletionStage<String> nodeReply = submitAndAssertOneRegistryTurn(
                    f, executor, alreadyOnLane, "requestToNode",
                    () -> f.request().submit(String.class));
                assertTerminal(nodeReply);

                CompletionStage<Void> nodeAdmission = submitAndAssertOneRegistryTurn(
                    f, executor, alreadyOnLane, "sendToNode", () -> f.send().submit());
                nodeAdmission.toCompletableFuture().join();

                CompletionStage<List<Message>> spotReply = submitAndAssertOneRegistryTurn(
                    f, executor, alreadyOnLane, "requestToSpotViaRouterChannel",
                    () -> f.runtime.requestToSpotViaRouterChannel(
                        CHANNEL, TARGET, "target-spot", List.of(requestPart),
                        Duration.ofSeconds(3)));
                assertTerminal(spotReply);

                CompletionStage<Void> spotAdmission = submitAndAssertOneRegistryTurn(
                    f, executor, alreadyOnLane, "sendToSpotViaRouterChannel",
                    () -> f.runtime.sendToSpotViaRouterChannel(
                        CHANNEL, TARGET, "target-spot", List.of(sendPart)));
                spotAdmission.toCompletableFuture().join();
            }
        }
    }

    @Test
    void routeRouterKeepsPriorityAndSubmitsInTheRegistryTurn() throws Exception {
        try (Fixture f = new Fixture()) {
            NodeProbe node = new NodeProbe(f.lane);
            RouterProbe router = new RouterProbe(f.lane);
            f.register(Duration.ofSeconds(4));
            f.runtime.registerSpotRouterNode(CHANNEL, node.node);
            f.sockets.registerRouteRouter(CHANNEL, router.router);

            assertTerminal(f.request().submit(String.class));
            f.send().submit().toCompletableFuture().join();

            assertEquals(0, node.nodeCalls());
            assertEquals(1, router.requests.get());
            assertEquals(1, router.sends.get());
            assertEquals(Duration.ofSeconds(4), router.timeout.get());
        }
    }

    @Test
    void nodeReplacementCannotInterleaveBetweenSelectionAndBindingSubmit() throws Exception {
        for (boolean request : new boolean[] {false, true}) {
            try (Fixture f = new Fixture(); var workers = Executors.newVirtualThreadPerTaskExecutor()) {
                CountDownLatch bindingEntered = new CountDownLatch(1);
                CountDownLatch releaseBinding = new CountDownLatch(1);
                NodeProbe oldNode = new NodeProbe(f.lane);
                NodeProbe newNode = new NodeProbe(f.lane);
                oldNode.onNodeBinding = () -> {
                    assertSame(f.lane, ZLinkStateLane.current());
                    bindingEntered.countDown();
                    await(releaseBinding);
                };
                f.register(Duration.ofSeconds(2));
                f.runtime.registerSpotRouterNode(CHANNEL, oldNode.node);

                var submission = workers.submit(() -> request
                    ? f.request().submit(String.class) : f.send().submit());
                try {
                    await(bindingEntered);
                    var replacement = workers.submit(() -> {
                        f.register(Duration.ofSeconds(7));
                        f.runtime.registerSpotRouterNode(CHANNEL, newNode.node);
                    });
                    awaitQueued(f.lane);
                    assertFalse(replacement.isDone());
                    releaseBinding.countDown();

                    CompletionStage<?> completion = submission.get(5, TimeUnit.SECONDS);
                    if (request) {
                        assertTerminal(completion);
                    } else {
                        completion.toCompletableFuture().join();
                    }
                    replacement.get(5, TimeUnit.SECONDS);
                    if (request) {
                        assertEquals(Duration.ofSeconds(2), oldNode.timeout.get());
                    }
                    assertTerminal(f.request().submit(String.class));
                    assertEquals(Duration.ofSeconds(7), newNode.timeout.get());
                    assertEquals(1, oldNode.nodeCalls());
                    assertEquals(1, newNode.nodeCalls());
                } finally {
                    releaseBinding.countDown();
                }
            }
        }
    }

    @Test
    void unfinishedNodeRequestDoesNotHoldTheRegistryLane() throws Exception {
        try (Fixture f = new Fixture(); var workers = Executors.newVirtualThreadPerTaskExecutor()) {
            NodeProbe oldNode = new NodeProbe(f.lane);
            NodeProbe newNode = new NodeProbe(f.lane);
            oldNode.pendingNodeRequest = true;
            f.runtime.registerSpotRouterNode(CHANNEL, oldNode.node);

            CompletionStage<String> request = f.request().submit(String.class);
            CompletableFuture<ZLinkBackendReceived> binding =
                oldNode.pendingBinding.get();
            assertNotNull(binding);
            assertFalse(request.toCompletableFuture().isDone());

            var replacement = workers.submit(
                () -> f.runtime.registerSpotRouterNode(CHANNEL, newNode.node));
            replacement.get(5, TimeUnit.SECONDS);
            f.send().submit().toCompletableFuture().join();
            assertEquals(1, newNode.nodeCalls());

            binding.completeExceptionally(TERMINAL);
            assertTerminal(request);
        }
    }

    @Test
    void directSpotRequestAndSendStartBindingInTheRegistryTurnIncludingAnExistingTurn()
        throws Exception {
        for (boolean alreadyOnLane : new boolean[] {false, true}) {
            try (Fixture f = new Fixture();
                 Message requestPart = Message.from("spot-request");
                 Message sendPart = Message.from("spot-send")) {
                NodeProbe node = new NodeProbe(f.lane);
                node.onSpotBinding = () -> assertSame(f.lane, ZLinkStateLane.current());
                f.runtime.registerSpotRouterNode(CHANNEL, node.node);

                CompletionStage<List<Message>> reply = alreadyOnLane
                    ? f.lane.runAsync(() -> f.runtime.requestToSpotViaRouterChannel(
                            CHANNEL, TARGET, "target-spot", List.of(requestPart),
                            Duration.ofSeconds(3)))
                        .toCompletableFuture().join()
                    : f.runtime.requestToSpotViaRouterChannel(
                        CHANNEL, TARGET, "target-spot", List.of(requestPart),
                        Duration.ofSeconds(3));
                CompletionStage<Void> admission = alreadyOnLane
                    ? f.lane.runAsync(() -> f.runtime.sendToSpotViaRouterChannel(
                            CHANNEL, TARGET, "target-spot", List.of(sendPart)))
                        .toCompletableFuture().join()
                    : f.runtime.sendToSpotViaRouterChannel(
                        CHANNEL, TARGET, "target-spot", List.of(sendPart));

                assertTerminal(reply);
                admission.toCompletableFuture().join();
                assertEquals(1, node.spotRequests.get());
                assertEquals(1, node.spotSends.get());
                assertEquals(Duration.ofSeconds(3), node.timeout.get());
            }
        }
    }

    @Test
    void spotNodeReplacementCannotInterleaveBetweenSelectionAndBindingSubmit() throws Exception {
        for (boolean request : new boolean[] {false, true}) {
            try (Fixture f = new Fixture(); var workers = Executors.newVirtualThreadPerTaskExecutor();
                 Message part = Message.from("spot")) {
                CountDownLatch bindingEntered = new CountDownLatch(1);
                CountDownLatch releaseBinding = new CountDownLatch(1);
                NodeProbe oldNode = new NodeProbe(f.lane);
                NodeProbe newNode = new NodeProbe(f.lane);
                oldNode.onSpotBinding = () -> {
                    assertSame(f.lane, ZLinkStateLane.current());
                    bindingEntered.countDown();
                    await(releaseBinding);
                };
                f.runtime.registerSpotRouterNode(CHANNEL, oldNode.node);

                var submission = workers.submit(() -> request
                    ? f.runtime.requestToSpotViaRouterChannel(
                        CHANNEL, TARGET, "target-spot", List.of(part), Duration.ofSeconds(3))
                    : f.runtime.sendToSpotViaRouterChannel(
                        CHANNEL, TARGET, "target-spot", List.of(part)));
                try {
                    await(bindingEntered);
                    var replacement = workers.submit(
                        () -> f.runtime.registerSpotRouterNode(CHANNEL, newNode.node));
                    awaitQueued(f.lane);
                    assertFalse(replacement.isDone());
                    releaseBinding.countDown();

                    CompletionStage<?> completion = submission.get(5, TimeUnit.SECONDS);
                    if (request) {
                        assertTerminal(completion);
                    } else {
                        completion.toCompletableFuture().join();
                    }
                    replacement.get(5, TimeUnit.SECONDS);
                    try (Message next = Message.from("next")) {
                        f.runtime.sendToSpotViaRouterChannel(
                            CHANNEL, TARGET, "target-spot", List.of(next))
                            .toCompletableFuture().join();
                    }
                    assertEquals(1, oldNode.spotCalls());
                    assertEquals(1, newNode.spotCalls());
                } finally {
                    releaseBinding.countDown();
                }
            }
        }
    }

    private static void assertTerminal(CompletionStage<?> stage) {
        assertSame(TERMINAL, assertThrows(CompletionException.class,
            () -> stage.toCompletableFuture().join()).getCause());
    }

    private static <T> T submitAndAssertOneRegistryTurn(
        Fixture fixture,
        CountingDirectExecutor executor,
        boolean alreadyOnLane,
        String operation,
        Supplier<T> submission) {
        int before = executor.turns();
        T result = alreadyOnLane
            ? fixture.lane.runAsync(submission).toCompletableFuture().join()
            : submission.get();
        assertEquals(before + 1, executor.turns(),
            operation + (alreadyOnLane ? " inline" : " off-lane"));
        return result;
    }

    private static void await(CountDownLatch latch) {
        try {
            assertTrue(latch.await(5, TimeUnit.SECONDS));
        } catch (InterruptedException failure) {
            Thread.currentThread().interrupt();
            throw new AssertionError(failure);
        }
    }

    private static void awaitQueued(ZLinkStateLane lane) throws Exception {
        Queue<?> mailbox = (Queue<?>) field(lane, "mailbox");
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (mailbox.isEmpty() && System.nanoTime() < deadline) {
            Thread.yield();
        }
        assertFalse(mailbox.isEmpty(), "the replacement must be queued behind the submission turn");
    }

    private static Object field(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }

    private static void setField(Object object, String name, Object value) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(object, value);
    }

    private static Object defaultValue(Class<?> type) {
        if (type == void.class) {
            return null;
        }
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == int.class) {
            return 0;
        }
        if (type == long.class) {
            return 0L;
        }
        throw new AssertionError("unexpected primitive proxy method: " + type);
    }

    private static final class Fixture implements AutoCloseable {
        final ZLinkChannelRuntime runtime;
        final ZLinkChannelSocketRegistry sockets;
        final ZLinkStateLane lane;

        Fixture() throws Exception {
            this(null);
        }

        Fixture(Executor laneExecutor) throws Exception {
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            options.setDefaultRequestTimeout(DEFAULT_TIMEOUT);
            runtime = new ZLinkChannelRuntime(
                new EmptyBackend(), options.registration(), new ZLinkJsonMessageSerializer());
            sockets = (ZLinkChannelSocketRegistry) field(runtime, "sockets");
            if (laneExecutor == null) {
                lane = (ZLinkStateLane) field(sockets, "stateLane");
            } else {
                lane = new ZLinkStateLane(laneExecutor);
                setField(sockets, "stateLane", lane);
            }
        }

        void register(Duration timeout) {
            ChannelRegistration registration = new ChannelRegistration(CHANNEL, ChannelKind.ROUTE_MESH);
            registration.setDefaultRequestTimeout(timeout);
            sockets.registerChannel(registration);
        }

        ZLinkRequestCall request() {
            return runtime.requestToNode(CHANNEL, TARGET, "request");
        }

        ZLinkSendCall send() {
            return runtime.sendToNode(CHANNEL, TARGET, "send");
        }

        @Override
        public void close() {
            runtime.close();
        }
    }

    private static final class CountingDirectExecutor implements Executor {
        private final AtomicInteger turns = new AtomicInteger();
        private final ThreadLocal<Integer> depth = ThreadLocal.withInitial(() -> 0);

        @Override
        public void execute(Runnable command) {
            int currentDepth = depth.get();
            // A completed lane item schedules its drain continuation recursively.
            // Both executor calls belong to the same externally submitted turn.
            if (currentDepth == 0) {
                turns.incrementAndGet();
            }
            depth.set(currentDepth + 1);
            try {
                command.run();
            } finally {
                if (currentDepth == 0) {
                    depth.remove();
                } else {
                    depth.set(currentDepth);
                }
            }
        }

        int turns() {
            return turns.get();
        }
    }

    private static final class NodeProbe {
        private static final RoutingId SOURCE = RoutingId.from("source-node");
        final AtomicInteger nodeRequests = new AtomicInteger();
        final AtomicInteger nodeSends = new AtomicInteger();
        final AtomicInteger spotRequests = new AtomicInteger();
        final AtomicInteger spotSends = new AtomicInteger();
        final AtomicReference<Duration> timeout = new AtomicReference<>();
        final AtomicReference<Map<String, String>> metadata = new AtomicReference<>();
        final AtomicReference<CompletableFuture<ZLinkBackendReceived>> pendingBinding =
            new AtomicReference<>();
        final ZLinkInternalSpotNode node;
        Runnable onNodeBinding = () -> { };
        Runnable onSpotBinding = () -> { };
        boolean pendingNodeRequest;

        NodeProbe(ZLinkStateLane lane) {
            Object spot = Proxy.newProxyInstance(getClass().getClassLoader(),
                new Class<?>[] {
                    systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot.class
                }, (proxy, method, args) -> {
                    if (method.getName().equals("sendToSpot") && args.length == 4) {
                        spotSends.incrementAndGet();
                        onSpotBinding.run();
                        return CompletableFuture.completedFuture(null);
                    }
                    if (method.getName().equals("requestToSpot") && args.length == 8) {
                        timeout.set((Duration) args[5]);
                        ZLinkServiceOperationRegistry operations =
                            (ZLinkServiceOperationRegistry) args[6];
                        UUID operationId = (UUID) args[7];
                        return operations.submit(operationId, (Duration) args[5], () -> {
                            assertEquals(1, operations.pendingCount());
                            spotRequests.incrementAndGet();
                            onSpotBinding.run();
                            return CompletableFuture.failedFuture(TERMINAL);
                        }, ZLinkBackendReceived::close);
                    }
                    return defaultValue(method.getReturnType());
                });
            node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(getClass().getClassLoader(),
                new Class<?>[] {ZLinkInternalSpotNode.class}, (proxy, method, args) -> {
                    switch (method.getName()) {
                        case "routingId":
                            return SOURCE;
                        case "classifyNodeSendTarget":
                            return Optional.empty();
                        case "entrySpot":
                            return spot;
                        case "sendToNode":
                            if (args.length == 3) {
                                metadata.set(ZLinkApplicationMetadata.decode((byte[]) args[1]));
                                nodeSends.incrementAndGet();
                                onNodeBinding.run();
                                return CompletableFuture.completedFuture(null);
                            }
                            break;
                        case "requestToNode":
                            if (args.length == 6) {
                                timeout.set((Duration) args[3]);
                                metadata.set(ZLinkApplicationMetadata.decode((byte[]) args[1]));
                                ZLinkServiceOperationRegistry operations =
                                    (ZLinkServiceOperationRegistry) args[4];
                                UUID operationId = (UUID) args[5];
                                return operations.submit(operationId, (Duration) args[3], () -> {
                                    assertEquals(1, operations.pendingCount());
                                    nodeRequests.incrementAndGet();
                                    onNodeBinding.run();
                                    if (pendingNodeRequest) {
                                        CompletableFuture<ZLinkBackendReceived> pending =
                                            new CompletableFuture<>();
                                        pendingBinding.set(pending);
                                        return pending;
                                    }
                                    return CompletableFuture.failedFuture(TERMINAL);
                                }, ZLinkBackendReceived::close);
                            }
                            break;
                        default:
                            break;
                    }
                    return defaultValue(method.getReturnType());
                });
        }

        int nodeCalls() {
            return nodeRequests.get() + nodeSends.get();
        }

        int spotCalls() {
            return spotRequests.get() + spotSends.get();
        }
    }

    private static final class RouterProbe {
        final AtomicInteger requests = new AtomicInteger();
        final AtomicInteger sends = new AtomicInteger();
        final AtomicReference<Duration> timeout = new AtomicReference<>();
        final ZLinkBackendRouterSocket router;

        RouterProbe(ZLinkStateLane lane) {
            router = (ZLinkBackendRouterSocket) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[] {ZLinkBackendRouterSocket.class},
                (proxy, method, args) -> {
                    if (method.getName().equals("send")) {
                        assertSame(lane, ZLinkStateLane.current());
                        sends.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    }
                    if (method.getName().equals("request")) {
                        assertSame(lane, ZLinkStateLane.current());
                        requests.incrementAndGet();
                        timeout.set((Duration) args[2]);
                        return CompletableFuture.failedFuture(TERMINAL);
                    }
                    if (method.getName().equals("name")) {
                        return "test-router";
                    }
                    return defaultValue(method.getReturnType());
                });
        }
    }

    private static final class EmptyBackend implements ZLinkChannelBackendAdapter {
        private final ZLinkBackendContext context = (ZLinkBackendContext) Proxy.newProxyInstance(
            getClass().getClassLoader(), new Class<?>[] {ZLinkBackendContext.class},
            (proxy, method, args) -> method.getName().equals("name")
                ? "test-context" : defaultValue(method.getReturnType()));

        @Override
        public ZLinkBackendContext createContext() {
            return context;
        }

        @Override
        public systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket
            createDealerSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket
            createPublisherSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket
            createSubscriberSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
    }
}
