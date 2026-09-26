package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.framework.monitoring.ZLinkTopologyState;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;

/**
 * Consumes {@code framework/runtime/conformance/route-mesh-placement-v1.json}: RouteMesh placement
 * counts come from the reporting MeshNode's activation records, the activation admission record
 * counts only the operations of MeshNode §5.1, and {@code IsAvailable} follows runtime
 * monitoring §5.
 */
final class ZLinkRouteMeshPlacementConformanceTest {
    private static final String SPOT_TYPE_PREFIX = "placement-spot-";
    private static final String INSTANCE_TYPE_PREFIX = "placement-instance-";
    private static final String ACTOR_TYPE = "placement-actor";
    private static final CompletableFuture<Void> OPEN = CompletableFuture.completedFuture(null);

    /** Gates a held operation keeps waiting on (fixture field {@code hold}). */
    static volatile CompletableFuture<Void> actorFactoryGate = OPEN;

    static volatile CompletableFuture<Void> spotCreateGate = OPEN;
    static volatile CompletableFuture<Void> instanceInitializeGate = OPEN;
    static volatile CompletableFuture<Void> actorJoinGate = OPEN;

    @Test
    void placementFollowsTheReportingMeshNode() throws Exception {
        JsonNode fixture = new ObjectMapper().readTree(Files.readString(sharedFixture()));
        assertEquals("zlink.framework.route-mesh-placement", fixture.path("fixture").asText());
        assertEquals(1, fixture.path("version").asInt());
        for (JsonNode scenario : fixture.path("scenarios")) {
            try {
                runScenario(fixture.path("ownerLease"), scenario);
            } catch (Exception | AssertionError failure) {
                throw new AssertionError(scenario.path("name").asText(), failure);
            } finally {
                releaseGates();
            }
        }
    }

    private static void runScenario(JsonNode lease, JsonNode scenario) throws Exception {
        String name = scenario.path("name").asText();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        var store = new ZLinkInMemoryLocationStore();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var locations = options.configureLocations();
        locations.setOwnerLeaseRenewInterval(millis(lease, "renewIntervalMs"));
        locations.setOwnerLeaseRenewTimeout(millis(lease, "renewTimeoutMs"));
        locations.setOwnerLeaseTtl(millis(lease, "ttlMs"));
        locations.setOwnerLeaseFencingMargin(millis(lease, "fencingMarginMs"));
        for (JsonNode node : scenario.path("meshNodes")) {
            String meshName = node.path("meshName").asText();
            assertEquals("disabled", node.path("spotRelocation").asText(), name + ":" + meshName);
            var mesh =
                    options.addRouteMesh(meshName)
                            .listen("inproc://placement-" + meshName + "-" + suffix)
                            .setRoutingId(nodeRid(meshName, suffix))
                            .setActorCapacity(node.path("actorLimit").asInt())
                            .setSpotCapacity(node.path("spotLimit").asInt())
                            .setActivationConcurrency(node.path("activationConcurrency").asInt());
            var server =
                    mesh.objects()
                            .server()
                            .addSpotFactory(
                                    SPOT_TYPE_PREFIX + meshName,
                                    PlacementSpot.class,
                                    factory -> factory.disableRelocation());
            // Every Object Server MeshNode has its Entry Spot; it never holds an activation.
            server.addEntrySpot(PlacementEntrySpot.class);
            if (node.path("actorFactory").asBoolean()) {
                server.addActorFactory(
                        ACTOR_TYPE,
                        PlacementActor.class,
                        PlacementActorFactory.class,
                        factory -> factory.disableRelocation());
            }
            if (node.path("instanceSpotFactory").asBoolean()) {
                server.addInstanceSpotFactory(
                        INSTANCE_TYPE_PREFIX + meshName,
                        PlacementInstanceSpot.class,
                        factory -> factory.disableRelocation());
            }
        }

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            List<CompletableFuture<?>> held = new ArrayList<>();
            String lastActorId = null;
            String lastSpotId = null;
            int objectIndex = 0;
            for (JsonNode objects : scenario.path("objects")) {
                String kind = objects.path("kind").asText();
                String meshName = objects.path("meshName").asText();
                boolean hold = objects.path("hold").asBoolean();
                for (int index = 0; index < objects.path("count").asInt(); index++) {
                    CompletableFuture<Void> gate = hold ? new CompletableFuture<>() : OPEN;
                    String objectId = name + "-" + kind + "-" + objectIndex++;
                    CompletableFuture<?> operation =
                            switch (kind) {
                                case "actor" -> {
                                    actorFactoryGate = gate;
                                    lastActorId = objectId;
                                    yield runtime.actorManager()
                                            .create(objectId, ACTOR_TYPE)
                                            .inMesh(meshName)
                                            .submit()
                                            .toCompletableFuture();
                                }
                                case "userSpot" -> {
                                    spotCreateGate = gate;
                                    lastSpotId = objectId;
                                    yield runtime.spotManager()
                                            .getOrCreate(objectId, SPOT_TYPE_PREFIX + meshName)
                                            .inMesh(meshName)
                                            .submit()
                                            .toCompletableFuture();
                                }
                                case "instanceSpot" -> {
                                    instanceInitializeGate = gate;
                                    PlacementInstanceSpot.initialized = new CompletableFuture<>();
                                    CompletableFuture<Void> initialized =
                                            PlacementInstanceSpot.initialized;
                                    // The send completes once the Instance Spot accepts it; the
                                    // cold activation ends when onInitialize finishes.
                                    yield PlacementEntrySpot.contexts
                                            .get(nodeRid(meshName, suffix))
                                            .outbound()
                                            .sendToSpot(objectId, "wake")
                                            .instanceSpot(INSTANCE_TYPE_PREFIX + meshName)
                                            .inMesh(meshName)
                                            .submit()
                                            .toCompletableFuture()
                                            .thenCompose(accepted -> initialized);
                                }
                                case "actorJoin" -> {
                                    actorJoinGate = gate;
                                    PlacementSpot.joined = new CompletableFuture<>();
                                    CompletableFuture<Void> joined = PlacementSpot.joined;
                                    yield runtime.actorClient()
                                            .requestToActor(
                                                    Objects.requireNonNull(lastActorId),
                                                    new JoinRequest(
                                                            Objects.requireNonNull(lastSpotId)))
                                            .timeout(Duration.ofSeconds(10))
                                            .submit(String.class)
                                            .toCompletableFuture()
                                            .thenCompose(scheduled -> joined);
                                }
                                default ->
                                        throw new AssertionError(name + ": unknown kind " + kind);
                            };
                    if (hold) {
                        held.add(operation);
                    } else {
                        operation.get(10, TimeUnit.SECONDS);
                    }
                }
            }
            var runtimeOptions = (ZLinkRouteMeshRuntimeOptions) runtime.routeMeshRuntime();
            for (JsonNode node : scenario.path("meshNodes")) {
                runtimeOptions
                        .mesh(node.path("meshName").asText())
                        .setPlacementWeight(node.path("placementWeightAfterStartup").asInt());
            }
            var monitoring = runtime.routeMeshRuntime();
            for (JsonNode expected : scenario.path("expected")) {
                String meshName = expected.path("meshName").asText();
                boolean expectedAvailable = expected.path("isAvailable").asBoolean();
                ZLinkTopologyState expectedState =
                        ZLinkTopologyState.valueOf(
                                expected.path("state").asText().toUpperCase(Locale.ROOT));
                ZLinkMeshNodeSnapshot status = monitoring.snapshot(meshName);
                String label = name + ":" + meshName;
                for (CompletableFuture<?> operation : held) {
                    if (operation.isDone()) {
                        throw new AssertionError(
                                label + ": a held operation ended before the check",
                                operation.handle((ignored, failure) -> failure).join());
                    }
                }
                assertEquals(
                        expected.path("activeActorCount").asInt(),
                        status.placement().activeActorCount(),
                        label + ":activeActorCount");
                assertEquals(
                        expected.path("activeSpotCount").asInt(),
                        status.placement().activeSpotCount(),
                        label + ":activeSpotCount");
                assertEquals(
                        expectedAvailable,
                        status.placement().isAvailable(),
                        label + ":isAvailable");
                assertEquals(expectedState, status.state(), label + ":state");
            }
            releaseGates();
            for (CompletableFuture<?> operation : held) {
                operation.get(10, TimeUnit.SECONDS);
            }
        }
    }

    private static RoutingId nodeRid(String meshName, String suffix) {
        return RoutingId.from("placement-" + meshName + "-" + suffix);
    }

    private static void releaseGates() {
        actorFactoryGate.complete(null);
        spotCreateGate.complete(null);
        instanceInitializeGate.complete(null);
        actorJoinGate.complete(null);
        actorFactoryGate = OPEN;
        spotCreateGate = OPEN;
        instanceInitializeGate = OPEN;
        actorJoinGate = OPEN;
    }

    private static Duration millis(JsonNode lease, String field) {
        return Duration.ofMillis(lease.path(field).asLong());
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve("runtime/conformance/route-mesh-placement-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared RouteMesh placement fixture was not found");
    }

    public record JoinRequest(String spotId) {}

    public static final class PlacementSpot implements ZLinkSpot<PlacementActor> {
        static volatile CompletableFuture<Void> joined = new CompletableFuture<>();
        private final ZLinkSpotContext context;

        public PlacementSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            return spotCreateGate.thenApply(ignored -> ZLinkSpotCreateResponse.accept());
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return actorJoinGate.thenApply(ignored -> ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(PlacementActor actor) {
            joined.complete(null);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(PlacementActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlacementEntrySpot implements ZLinkEntrySpot<PlacementActor> {
        /** The Entry Spot of each MeshNode, which sends the message that wakes an Instance Spot. */
        static final Map<RoutingId, ZLinkEntrySpotContext> contexts = new ConcurrentHashMap<>();

        private final ZLinkEntrySpotContext context;

        public PlacementEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
            contexts.put(context.nodeRid(), context);
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(PlacementActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(PlacementActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    /** Starts the Actor Join the fixture holds in the Spot's join callback. */
    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<
                    PlacementEntrySpot, PlacementActor, JoinRequest, String> {
        @Override
        public CompletionStage<String> handle(
                PlacementEntrySpot entrySpot,
                PlacementActor actor,
                ZLinkMessageContext context,
                JoinRequest request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class PlacementInstanceSpot implements ZLinkInstanceSpot {
        static volatile CompletableFuture<Void> initialized = new CompletableFuture<>();
        private final ZLinkInstanceSpotContext context;

        public PlacementInstanceSpot(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkInstanceSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            CompletableFuture<Void> done = initialized;
            return instanceInitializeGate.thenRun(() -> done.complete(null));
        }
    }

    public record PlacementActor(String actorId, ZLinkActorContext context) implements ZLinkActor {}

    public static final class PlacementActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return actorFactoryGate.thenApply(
                    ignored -> new PlacementActor(context.actorId(), context));
        }
    }
}
