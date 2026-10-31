package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.*;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.spots.*;
import systems.zlink.framework.runtime.internal.spots.SpotTerminalProbe;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.messaging.*;
import systems.zlink.framework.spots.*;

import java.lang.reflect.*;
import java.nio.file.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

final class InstanceIntentAuthorityFollowTest {
    static JsonNode fixture() throws Exception {
        Path root = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (root != null) {
            Path candidate = root.resolve("framework/runtime/conformance/spot-close-v1.json");
            if (Files.isRegularFile(candidate))
                return new ObjectMapper().readTree(Files.readString(candidate));
            root = root.getParent();
        }
        throw new IllegalStateException("spot-close-v1.json");
    }

    @TestFactory
    List<DynamicTest> authorityFollowConformance() throws Exception {
        List<DynamicTest> tests = new ArrayList<>();
        for (JsonNode scenario : fixture().path("authorityFollowCases")) {
            tests.add(DynamicTest.dynamicTest(scenario.path("name").asText(), () -> run(scenario)));
        }
        return tests;
    }

    static void run(JsonNode scenario) throws Exception {
        if (scenario.path("authority").asText().equals("Creating")) {
            new ZLinkInstanceSpotStaleCacheTest().followAuthority("Creating");
            return;
        }
        if (!scenario.path("intent").asBoolean()) {
            withoutIntent();
            return;
        }
        String spotId = "follow-" + UUID.randomUUID();
        var backend = new Backend(scenario);
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.addRelocationStore(new InMemoryRelocationStore());
        options.addRouteMesh("mesh")
                .listen("inproc://" + spotId)
                .setRoutingId(RoutingId.from("new-owner"))
                .objects()
                .server()
                .addInstanceSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options, backend);
                var payload =
                        Message.from(
                                new ZLinkJsonMessageSerializer()
                                        .serialize(new Echo("follow"))
                                        .bytes())) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            if (!scenario.path("authority").asText().equals("Missing")) {
                assertEquals(
                        new Echo("seed"),
                        runtime.route()
                                .requestToSpot(spotId, new Echo("seed"))
                                .instanceSpot("room")
                                .inMesh("mesh")
                                .timeout(Duration.ofSeconds(2))
                                .submit(Echo.class)
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS));
            }
            backend.enabled = true;
            CompletableFuture<?> pending;
            if (scenario.path("intent").asBoolean()) {
                {
                    var stale =
                            new SpotTransportAddress(
                                    "mesh",
                                    RoutingId.from("old-owner"),
                                    spotId,
                                    1,
                                    1,
                                    1,
                                    1,
                                    ZLinkSpotKind.INSTANCE,
                                    "room",
                                    "old-owner",
                                    "old-version");
                    var resolver =
                            new SpotTransportAddressResolver() {
                                public CompletionStage<Optional<SpotTransportAddress>> resolve(
                                        String id) {
                                    throw new AssertionError(
                                            "authority is resolved by Instance call runtime");
                                }
                            };
                    pending =
                            ((systems.zlink.framework.runtime.spots.ZLinkSpotRuntime)
                                            runtime.spotManager())
                                    .instanceSpotCalls()
                                    .request(
                                            spotId,
                                            "room",
                                            "mesh",
                                            payload,
                                            Optional.of(Echo.class.getSimpleName()),
                                            null,
                                            Map.of(),
                                            scenario.path("refusals").asInt() < 0
                                                    ? Duration.ofMillis(40)
                                                    : Duration.ofSeconds(2),
                                            stale,
                                            resolver,
                                            new CompletableFuture<>())
                                    .toCompletableFuture();
                }
            } else {
                pending =
                        runtime.route()
                                .requestToSpot(spotId, new Echo("follow"))
                                .timeout(Duration.ofSeconds(2))
                                .submit(Echo.class)
                                .toCompletableFuture();
            }
            if (scenario.path("terminal").asText().equals("reply")) {
                Object reply = pending.get(5, TimeUnit.SECONDS);
                assertInstanceOf(List.class, reply);
                @SuppressWarnings("unchecked")
                List<Message> parts = (List<Message>) reply;
                try {
                    assertFalse(
                            ZLinkChannelEnvelope.decodeHeader(parts.getFirst(), false).isError());
                    assertEquals(
                            new Echo("follow"),
                            ZLinkMessagePayloads.deserialize(
                                    new ZLinkJsonMessageSerializer(),
                                    parts.getLast().dataBuffer(),
                                    Echo.class));
                } finally {
                    parts.forEach(Message::close);
                }
                assertEquals(scenario.path("refusals").asInt() + 1, backend.attempts.get());
            } else {
                ExecutionException failure =
                        assertThrows(
                                ExecutionException.class, () -> pending.get(5, TimeUnit.SECONDS));
                Throwable cause = failure.getCause();
                while (cause instanceof CompletionException) cause = cause.getCause();
                assertEquals(
                        scenario.path("terminal").asText().equals("DeadlineExceeded")
                                ? ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED
                                : ZLinkFrameworkErrorKind.UNAVAILABLE,
                        assertInstanceOf(ZLinkFrameworkException.class, cause).kind());
                if (!scenario.path("intent").asBoolean()) assertEquals(1, backend.attempts.get());
            }
        }
    }

    private static void withoutIntent() {
        var terminal =
                ZLinkRequestFailureMapping.receivedFailure(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        "pre-admission fence",
                        34,
                        Map.of("zlink.origin", "framework"));
        var probe = new SpotTerminalProbe(terminal, false);
        AtomicInteger attempts = new AtomicInteger();
        try (var operations =
                new systems.zlink.framework.runtime.internal.service
                        .ZLinkServiceOperationRegistry()) {
            var backend =
                    (ZLinkBackendSpot)
                            Proxy.newProxyInstance(
                                    ZLinkBackendSpot.class.getClassLoader(),
                                    new Class<?>[] {ZLinkBackendSpot.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("requestToSpot")) {
                                            attempts.incrementAndGet();
                                            return operations.submit(
                                                    systems.zlink.framework.runtime.internal.service
                                                            .ZLinkServiceOperationIds.next(),
                                                    Duration.ofSeconds(2),
                                                    () -> CompletableFuture.failedFuture(terminal),
                                                    ZLinkBackendReceived::close);
                                        }
                                        if (method.getReturnType() == void.class) return null;
                                        throw new AssertionError(
                                                "unexpected backend call: " + method.getName());
                                    });
            var serializer = new ZLinkJsonMessageSerializer();
            var direct =
                    new ZLinkSpotDirectOutbound(
                            new ZLinkSpotRouteMessages(serializer),
                            new ZLinkMessageFlowTracer(
                                    new ZLinkDispatchOptionsRegistration(),
                                    systems.zlink.framework.runtime.internal.handlers
                                            .ZLinkHandlerActivator.reflection(),
                                    Runnable::run));
            var outbound =
                    new DefaultSpotOutbound(
                            backend,
                            "mesh",
                            null,
                            serializer,
                            ignored -> ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                            null,
                            direct,
                            null,
                            null,
                            false,
                            Duration.ofSeconds(2),
                            () -> probe,
                            probe);
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    outbound.requestToSpot("spot", new Echo("follow"))
                                            .submit(Echo.class)
                                            .toCompletableFuture()
                                            .join());
            assertSame(terminal, failure.getCause());
            assertEquals(1, attempts.get());
            assertEquals(1, probe.resolves);
            assertEquals(0, probe.activations);
        }
    }

    public record Echo(String value) {}

    public static final class Room implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;

        public Room(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(Handler.class);
        }
    }

    public static final class Handler implements ZLinkSpotRequestHandler<Room, Echo, Echo> {
        public CompletionStage<Echo> handle(Room room, Echo request) {
            return CompletableFuture.completedFuture(request);
        }
    }

    static final class Backend implements ZLinkBackendAdapterProvider {
        final ZLinkJavaBackendAdapterFactory delegate = new ZLinkJavaBackendAdapterFactory();
        final JsonNode scenario;
        final AtomicInteger attempts = new AtomicInteger();
        volatile boolean enabled;
        Object originalParts;
        Object originalAdmission;
        Duration previousTimeout;

        Backend(JsonNode scenario) {
            this.scenario = scenario;
        }

        public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createChannelAdapter(options);
        }

        public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createSpotAdapter(options);
        }

        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            return delegate.createStreamAdapter(options);
        }

        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                ZLinkBackendAdapterOptions options) {
            return delegate.createMonitoringAdapter(options);
        }

        public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
            var adapter = delegate.createMeshAdapter(options);
            return (context, name) -> {
                var node = adapter.createMeshNode(context, name);
                return (ZLinkInternalMeshNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalMeshNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalMeshNode.class},
                                (proxy, method, args) -> {
                                    if (enabled && method.getName().equals("requestInstanceSpot")) {
                                        int attempt = attempts.incrementAndGet();
                                        if (originalParts == null) {
                                            originalParts = args[4];
                                            originalAdmission = args[6];
                                        }
                                        assertSame(originalParts, args[4]);
                                        assertSame(originalAdmission, args[6]);
                                        Duration timeout = (Duration) args[5];
                                        if (previousTimeout != null)
                                            assertTrue(timeout.compareTo(previousTimeout) <= 0);
                                        previousTimeout = timeout;
                                        if (attempt == 1)
                                            assertEquals(
                                                    RoutingId.from("old-owner"),
                                                    ((ZLinkServiceM6BWireCodec.InstanceRoute)
                                                                    args[0])
                                                            .targetNodeRid());
                                        else
                                            assertEquals(
                                                    RoutingId.from("new-owner"),
                                                    ((ZLinkServiceM6BWireCodec.InstanceRoute)
                                                                    args[0])
                                                            .targetNodeRid());
                                        if (scenario.path("refusals").asInt() < 0
                                                || attempt <= scenario.path("refusals").asInt()) {
                                            ((CompletableFuture<?>) args[6]).complete(null);
                                            return CompletableFuture.failedFuture(
                                                    ZLinkRequestFailureMapping.receivedFailure(
                                                            scenario.path("failureCode").asInt()
                                                                            == 33
                                                                    ? ZLinkFrameworkErrorKind
                                                                            .INVALID_OPERATION
                                                                    : ZLinkFrameworkErrorKind
                                                                            .UNAVAILABLE,
                                                            "pre-admission fence",
                                                            scenario.path("failureCode").asInt(34),
                                                            Map.of("zlink.origin", "framework")));
                                        }
                                    }
                                    Object result = invoke(node, method, args);
                                    return result;
                                });
            };
        }

        static Object invoke(Object target, Method method, Object[] args) throws Throwable {
            try {
                return method.invoke(target, args);
            } catch (InvocationTargetException failure) {
                throw failure.getCause();
            }
        }
    }
}
