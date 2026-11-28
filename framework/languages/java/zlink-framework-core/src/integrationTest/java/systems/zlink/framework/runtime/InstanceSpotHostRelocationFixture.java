package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.api.BeforeEach;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationResult;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

abstract class InstanceSpotHostRelocationFixture {
    static final String MESH = "instance-relocation";
    static final String SPOT_ID = "room";
    static final String SPOT_TYPE = "Room";
    static final RoutingId CLIENT = RoutingId.from("client");
    static final RoutingId SOURCE = RoutingId.from("source");
    static final RoutingId TARGET = RoutingId.from("target");
    static final Duration DEADLINE = Duration.ofSeconds(10);
    static final String AUTHORITY = ZLinkAuthorityKeyCodec.spot(SPOT_ID);

    private ZLinkInMemoryLocationStore store;
    private final boolean traceMessageFlow;
    ZLinkProviderLocationRepository authorityStore;

    InstanceSpotHostRelocationFixture() {
        this(false);
    }

    InstanceSpotHostRelocationFixture(boolean traceMessageFlow) {
        this.traceMessageFlow = traceMessageFlow;
    }

    @BeforeEach
    void prepare() {
        store = new ZLinkInMemoryLocationStore();
        authorityStore = new ZLinkProviderLocationRepository(store);
        Client.address = new CompletableFuture<>();
        Room.sourceContext = new CompletableFuture<>();
        Room.closingEntered = new CompletableFuture<>();
        Room.releaseClose = CompletableFuture.completedFuture(null);
        Room.followHandler = null;
        Room.restoreEntered = new CompletableFuture<>();
        Room.releaseRestore = CompletableFuture.completedFuture(null);
    }

    ZLinkEntrySpotContext activateFrom(ZLinkFrameworkRuntime caller) throws Exception {
        awaitPeer(caller, SOURCE);
        var client = await(Client.address);
        assertEquals(
                "room:state-before-move",
                await(
                        client.outbound()
                                .requestToSpot(SPOT_ID, "state-before-move")
                                .instanceSpot(SPOT_TYPE)
                                .inMesh(MESH)
                                .submit(String.class)));
        return client;
    }

    static CompletionStage<ZLinkFrameworkRelocationResult> relocate(ZLinkFrameworkRuntime source) {
        return source.relocate(
                new ZLinkFrameworkRelocationOptions(
                        ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE, null, DEADLINE));
    }

    ZLinkAuthoritySnapshot authority() throws Exception {
        return assertInstanceOf(
                ZLinkAuthoritySnapshot.class, await(authorityStore.read(AUTHORITY, () -> false)));
    }

    ZLinkAuthoritySnapshot awaitReadyAuthority() throws Exception {
        long deadline = System.nanoTime() + DEADLINE.toNanos();
        do {
            if (await(authorityStore.read(AUTHORITY, () -> false))
                            instanceof ZLinkAuthoritySnapshot current
                    && current.steadyReady()) return current;
            Thread.sleep(2);
        } while (System.nanoTime() < deadline);
        throw new AssertionError("Instance Spot authority did not become Ready");
    }

    ZLinkFrameworkRuntime start(RoutingId rid) {
        return start(rid, null);
    }

    ZLinkFrameworkRuntime start(
            RoutingId rid, ZLinkFrameworkRuntimeTestAccess.Construction construction) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRelocationStore(new InMemoryRelocationStore());
        options.configureLocations().setPollingInterval(Duration.ofMillis(20));
        if (traceMessageFlow) {
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        }
        var node = options.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(rid);
        if (rid.equals(CLIENT)) {
            node.objects().client();
            node.objects().server().addEntrySpot(Client.class);
        } else {
            node.objects()
                    .server()
                    .addInstanceSpotFactory(
                            SPOT_TYPE,
                            Room.class,
                            factory -> factory.preserveStateWith(RoomRelocation.class));
        }
        return RuntimeTestSupport.startFixedRidFramework(
                options, new ZLinkJavaBackendAdapterFactory(), construction);
    }

    static void awaitPeer(ZLinkFrameworkRuntime runtime, RoutingId rid) throws Exception {
        long deadline = System.nanoTime() + DEADLINE.toNanos();
        while (runtime.routeMeshRuntime().snapshot(MESH).peers().stream()
                .noneMatch(
                        peer ->
                                peer.nodeRid().equals(rid)
                                        && peer.state() == ZLinkPeerState.READY)) {
            if (System.nanoTime() >= deadline)
                throw new AssertionError("RouteMesh peer not ready: " + rid);
            Thread.sleep(2);
        }
    }

    static <T> T await(CompletionStage<T> completion) throws Exception {
        return completion.toCompletableFuture().get(20, TimeUnit.SECONDS);
    }

    public record Client(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<ZLinkActor> {
        static CompletableFuture<ZLinkEntrySpotContext> address;

        @Override
        public CompletionStage<Void> onInitialize() {
            address.complete(context);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Room implements ZLinkInstanceSpot {
        static Class<? extends ZLinkSpotPacketHandler<Room, ?>> followHandler;
        static CompletableFuture<ZLinkInstanceSpotContext> sourceContext;
        static CompletableFuture<Void> closingEntered;
        static CompletableFuture<Void> releaseClose;
        static CompletableFuture<Void> restoreEntered;
        static CompletableFuture<Void> releaseRestore;

        private final ZLinkInstanceSpotContext context;
        private String state = "";

        public Room(ZLinkInstanceSpotContext context) {
            this.context = context;
            if (context.nodeRid().equals(SOURCE)) sourceContext.complete(context);
        }

        @Override
        public ZLinkInstanceSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addPacket(Echo.class);
            if (followHandler != null) context.handlers().addPacket(followHandler);
        }

        @Override
        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
            if (context.nodeRid().equals(SOURCE)) {
                closingEntered.complete(null);
                return releaseClose;
            }
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Echo implements ZLinkSpotRequestHandler<Room, String, String> {
        @Override
        public CompletionStage<String> handle(Room spot, String request) {
            if (!request.isEmpty()) spot.state = request;
            return CompletableFuture.completedFuture(spot.context().spotId() + ":" + spot.state);
        }
    }

    public static final class RoomRelocation implements ZLinkSpotRelocationAdapter<Room> {
        @Override
        public CompletionStage<byte[]> capture(
                Room spot, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(spot.state.getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public CompletionStage<Void> restore(
                Room spot, byte[] state, ZLinkRelocationCancellation cancellation) {
            Room.restoreEntered.complete(null);
            return Room.releaseRestore.thenRun(
                    () -> spot.state = new String(state, StandardCharsets.UTF_8));
        }
    }
}
