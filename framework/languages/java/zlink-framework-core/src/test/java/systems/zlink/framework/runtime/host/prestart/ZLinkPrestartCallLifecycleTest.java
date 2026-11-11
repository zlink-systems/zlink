package systems.zlink.framework.runtime.host.prestart;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.channels.*;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.handlers.*;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.*;
import systems.zlink.framework.runtime.internal.host.ZLinkFrameworkRuntimeBootstrap;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.util.*;
import java.util.concurrent.*;
import java.util.function.Supplier;

final class ZLinkPrestartCallLifecycleTest {
    private static java.util.logging.FileHandler flowLog;

    @org.junit.jupiter.api.BeforeAll
    static void captureFlow() throws Exception {
        java.nio.file.Files.createDirectories(java.nio.file.Path.of("build"));
        flowLog = new java.util.logging.FileHandler("build/prestart-call.flow");
        flowLog.setFormatter(new java.util.logging.SimpleFormatter());
        java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
                .addHandler(flowLog);
    }

    @org.junit.jupiter.api.AfterAll
    static void closeFlow() {
        java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
                .removeHandler(flowLog);
        flowLog.close();
    }

    enum Entry {
        CHANNEL_SEND,
        CHANNEL_REQUEST,
        NODE_SEND,
        NODE_REQUEST,
        SPOT_SEND,
        SPOT_REQUEST,
        INSTANCE_SEND,
        INSTANCE_REQUEST,
        ACTOR_SEND,
        ACTOR_REQUEST,
        FANOUT_PUBLISH,
        SPOT_PUBLISH,
        ACTOR_FIND,
        SPOT_FIND
    }

    private static final RoutingId NODE = RoutingId.from("prestart-node");

    @ParameterizedTest
    @EnumSource(Entry.class)
    void sameRejectedAndUnsubmittedCallsWorkAfterStartAndStopRejectsWithoutUsingCall(Entry entry)
            throws Exception {
        var target = target(entry);
        var options =
                options(
                        entry,
                        target == null
                                ? null
                                : target.listenerStatus(
                                                systems.zlink.framework.monitoring.ZLinkListenerKind
                                                        .ROUTE_MESH,
                                                "mesh")
                                        .endpoint());
        try (target;
                var runtime = ZLinkFrameworkRuntimeBootstrap.prepare(options, null)) {
            var reused = pending(runtime, entry);
            var unsubmitted = pending(runtime, entry);
            var stopped = pending(runtime, entry);
            var before = reused.get();
            failure(before, ZLinkFrameworkErrorKind.INVALID_OPERATION);
            failure(reused.get(), ZLinkFrameworkErrorKind.INVALID_OPERATION);
            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            assertNull(ZLinkFrameworkRuntimeTestAccess.channels(runtime));
            ZLinkFrameworkRuntimeBootstrap.startPrepared(
                    runtime,
                    options,
                    new ZLinkJavaBackendAdapterFactory(),
                    systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
                            .reflection());
            if (target != null) {
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (runtime.routeMeshRuntime().snapshot("mesh").readyPeerCount() == 0
                        && System.nanoTime() < deadline) Thread.onSpinWait();
                assertTrue(runtime.routeMeshRuntime().snapshot("mesh").readyPeerCount() > 0);
            }
            if (objects(entry)) {
                if (entry == Entry.ACTOR_SEND
                        || entry == Entry.ACTOR_REQUEST
                        || entry == Entry.ACTOR_FIND) {
                    runtime.actorManager()
                            .getOrCreate("actor", "actor")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                } else if (entry == Entry.SPOT_SEND
                        || entry == Entry.SPOT_REQUEST
                        || entry == Entry.SPOT_FIND) {
                    runtime.spotManager()
                            .getOrCreate("spot", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
                }
            }
            success(reused.get(), entry);
            success(unsubmitted.get(), entry);
            failure(before, ZLinkFrameworkErrorKind.INVALID_OPERATION);
            runtime.close();
            failure(stopped.get(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            failure(stopped.get(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            failure(pending(runtime, entry).get(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            if (entry != Entry.ACTOR_FIND && entry != Entry.SPOT_FIND) {
                failure(reused.get(), ZLinkFrameworkErrorKind.INVALID_OPERATION);
            }
        }
    }

    @Test
    void hostAdmissionPrecedesConfigurationAndOnlyAcceptedCallsAreUsed() {
        var options = options(Entry.FANOUT_PUBLISH);
        try (var runtime = ZLinkFrameworkRuntimeBootstrap.prepare(options, null)) {
            var call = runtime.fanout().publish("missing", "topic", new Ping("value"));
            failure(call.submit(), ZLinkFrameworkErrorKind.INVALID_OPERATION);
            ZLinkFrameworkRuntimeBootstrap.startPrepared(
                    runtime,
                    options,
                    new ZLinkJavaBackendAdapterFactory(),
                    systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
                            .reflection());
            assertThrows(ZLinkConfigurationException.class, call::submit);
            assertThrows(ZLinkConfigurationException.class, call::submit);
            runtime.close();
            failure(call.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            failure(call.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
        }
    }

    @Test
    void managerCreationBuildersUseTheSameAdmissionAndEntriesKeepTheirIdentity() throws Exception {
        var options = options(Entry.ACTOR_FIND);
        try (var runtime = ZLinkFrameworkRuntimeBootstrap.prepare(options, null)) {
            var entries = entries(runtime);
            var actor = runtime.actorManager().create("created-actor", "actor");
            var existingActor = runtime.actorManager().getOrCreate("existing-actor", "actor");
            var spot = runtime.spotManager().create("room");
            var existingSpot = runtime.spotManager().getOrCreate("existing-spot", "room");
            failure(actor.submit(), ZLinkFrameworkErrorKind.INVALID_OPERATION);
            failure(existingActor.submit(), ZLinkFrameworkErrorKind.INVALID_OPERATION);
            failure(spot.submit(), ZLinkFrameworkErrorKind.INVALID_OPERATION);
            failure(existingSpot.submit(), ZLinkFrameworkErrorKind.INVALID_OPERATION);
            ZLinkFrameworkRuntimeBootstrap.startPrepared(
                    runtime,
                    options,
                    new ZLinkJavaBackendAdapterFactory(),
                    systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
                            .reflection());
            assertNotNull(
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    actor.submit().toCompletableFuture().get(5, TimeUnit.SECONDS))
                            .actor());
            assertNotNull(
                    assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    existingActor
                                            .submit()
                                            .toCompletableFuture()
                                            .get(5, TimeUnit.SECONDS))
                            .actor());
            assertNotNull(spot.submit().toCompletableFuture().get(5, TimeUnit.SECONDS).spot());
            assertNotNull(
                    existingSpot.submit().toCompletableFuture().get(5, TimeUnit.SECONDS).spot());
            for (int index = 0; index < entries.size(); index++)
                assertSame(entries.get(index), entries(runtime).get(index));
            runtime.close();
            for (int index = 0; index < entries.size(); index++)
                assertSame(entries.get(index), entries(runtime).get(index));
            failure(
                    runtime.actorManager().create("late-actor", "actor").submit(),
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            failure(
                    runtime.spotManager().getOrCreate("late-spot", "room").submit(),
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN);
        }
    }

    private static List<Object> entries(ZLinkFrameworkRuntime runtime) {
        return List.of(
                runtime.client(),
                runtime.route(),
                runtime.fanout(),
                runtime.actorClient(),
                runtime.actorManager(),
                runtime.spotManager(),
                runtime.spotPublisherClient(),
                runtime.spotOutbound());
    }

    @ParameterizedTest
    @EnumSource(Entry.class)
    void closingBeforeInstallationTakesPriorityOverPrestart(Entry entry) {
        var options = options(entry);
        try (var runtime = ZLinkFrameworkRuntimeBootstrap.prepare(options, null)) {
            var call = pending(runtime, entry);
            runtime.close();
            failure(call.get(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
            failure(call.get(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
        }
    }

    @Test
    void publicationIsTheBoundaryWhileStartupIsStillPreparing() throws Exception {
        var options = options(Entry.FANOUT_PUBLISH);
        var beforePublication = new CountDownLatch(1);
        var publish = new CountDownLatch(1);
        var afterPublication = new CountDownLatch(1);
        var finishStartup = new CountDownLatch(1);
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .callServicesPublication(
                                publication -> {
                                    beforePublication.countDown();
                                    await(publish);
                                    publication.run();
                                    afterPublication.countDown();
                                    await(finishStartup);
                                });
        try (var runtime =
                        ZLinkFrameworkRuntimeTestAccess.prepareWithConstruction(
                                options, construction);
                var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            var call = runtime.fanout().publish("fanout", "topic", new Ping("value"));
            var startup =
                    executor.submit(
                            () ->
                                    ZLinkFrameworkRuntimeTestAccess.startPreparedWithConstruction(
                                            runtime,
                                            options,
                                            new ZLinkJavaBackendAdapterFactory(),
                                            construction));
            try {
                assertTrue(beforePublication.await(5, TimeUnit.SECONDS));
                var rejected = call.submit();
                failure(rejected, ZLinkFrameworkErrorKind.INVALID_OPERATION);
                publish.countDown();
                assertTrue(afterPublication.await(5, TimeUnit.SECONDS));
                assertEquals(
                        ZLinkFrameworkRuntimeState.PREPARING,
                        ZLinkFrameworkRuntimeTestAccess.state(runtime));
                assertFalse(
                        ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                                .toCompletableFuture()
                                .isDone());
                call.submit().toCompletableFuture().get(5, TimeUnit.SECONDS);
                failure(rejected, ZLinkFrameworkErrorKind.INVALID_OPERATION);
                finishStartup.countDown();
                startup.get(5, TimeUnit.SECONDS);
            } finally {
                publish.countDown();
                finishStartup.countDown();
            }
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(booleans = {true, false})
    void shutdownOnEitherSideOfPublicationPreservesAdmissionAndOwnership(
            boolean closeBeforePublication) throws Exception {
        var options = options(Entry.FANOUT_PUBLISH);
        var before = new CountDownLatch(1);
        var allowPublication = new CountDownLatch(1);
        var after = new CountDownLatch(1);
        var finish = new CountDownLatch(1);
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .callServicesPublication(
                                publication -> {
                                    before.countDown();
                                    await(allowPublication);
                                    publication.run();
                                    after.countDown();
                                    await(finish);
                                });
        try (var runtime =
                        ZLinkFrameworkRuntimeTestAccess.prepareWithConstruction(
                                options, construction);
                var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            var unused = runtime.fanout().publish("fanout", "topic", new Ping("stop"));
            var startup =
                    executor.submit(
                            () ->
                                    ZLinkFrameworkRuntimeTestAccess.startPreparedWithConstruction(
                                            runtime,
                                            options,
                                            new ZLinkJavaBackendAdapterFactory(),
                                            construction));
            try {
                assertTrue(before.await(5, TimeUnit.SECONDS));
                CompletionStage<Void> accepted = null;
                if (!closeBeforePublication) {
                    allowPublication.countDown();
                    assertTrue(after.await(5, TimeUnit.SECONDS));
                    accepted =
                            runtime.fanout()
                                    .publish("fanout", "topic", new Ping("accepted"))
                                    .submit();
                    accepted.toCompletableFuture().get(5, TimeUnit.SECONDS);
                }
                var stopping = executor.submit(runtime::close);
                long stopDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (ZLinkFrameworkRuntimeTestAccess.state(runtime)
                                != ZLinkFrameworkRuntimeState.DRAINING
                        && ZLinkFrameworkRuntimeTestAccess.state(runtime)
                                != ZLinkFrameworkRuntimeState.STOPPED
                        && System.nanoTime() < stopDeadline) Thread.onSpinWait();
                assertTrue(
                        ZLinkFrameworkRuntimeTestAccess.state(runtime)
                                        == ZLinkFrameworkRuntimeState.DRAINING
                                || ZLinkFrameworkRuntimeTestAccess.state(runtime)
                                        == ZLinkFrameworkRuntimeState.STOPPED);
                failure(unused.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
                failure(unused.submit(), ZLinkFrameworkErrorKind.SHUTTING_DOWN);
                if (accepted != null) accepted.toCompletableFuture().join();
                allowPublication.countDown();
                finish.countDown();
                var failed =
                        assertThrows(
                                ExecutionException.class, () -> startup.get(5, TimeUnit.SECONDS));
                assertInstanceOf(IllegalStateException.class, failed.getCause());
                stopping.get(5, TimeUnit.SECONDS);
                if (closeBeforePublication)
                    assertNull(ZLinkFrameworkRuntimeTestAccess.channels(runtime));
            } finally {
                allowPublication.countDown();
                finish.countDown();
            }
        }
    }

    private static void await(CountDownLatch latch) {
        try {
            assertTrue(latch.await(5, TimeUnit.SECONDS));
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new AssertionError(interrupted);
        }
    }

    private static ZLinkFrameworkRuntime target(Entry entry) {
        if (entry != Entry.NODE_REQUEST) return null;
        var options = new DefaultZLinkFrameworkOptions();
        options.addHandlersFromPackageOf(ZLinkPrestartCallLifecycleTest.class);
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.addRouteMesh("mesh")
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(NODE)
                .addRouteRequestHandler(NodeRouteRequest.class, Ping.class, String.class);
        return ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static boolean objects(Entry entry) {
        return switch (entry) {
            case CHANNEL_SEND, CHANNEL_REQUEST, NODE_SEND, NODE_REQUEST, FANOUT_PUBLISH -> false;
            default -> true;
        };
    }

    private static DefaultZLinkFrameworkOptions options(Entry entry) {
        return options(entry, null);
    }

    private static DefaultZLinkFrameworkOptions options(Entry entry, String peerEndpoint) {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.addHandlersFromPackageOf(ZLinkPrestartCallLifecycleTest.class);
        if (entry == Entry.FANOUT_PUBLISH) {
            options.addFanoutChannel("fanout")
                    .enablePublisher("inproc://prestart-fanout-" + UUID.randomUUID());
            return options;
        }
        if (entry == Entry.CHANNEL_SEND || entry == Entry.CHANNEL_REQUEST) {
            options.addClientServerChannel("channel")
                    .server()
                    .listen()
                    .addSendHandler(NodeSend.class, Notice.class)
                    .addRequestHandler(NodeRequest.class, Ping.class, String.class);
            options.addClientServerChannel("channel").client();
            return options;
        }
        var mesh =
                options.addRouteMesh("mesh").listen("inproc://prestart-mesh-" + UUID.randomUUID());
        mesh.addRouteSendHandler(NodeRouteSend.class, Notice.class)
                .addRouteRequestHandler(NodeRouteRequest.class, Ping.class, String.class);
        mesh.channelName("channel")
                .server()
                .addSendHandler(NodeSend.class, Notice.class)
                .addRequestHandler(NodeRequest.class, Ping.class, String.class);
        if (objects(entry)) {
            options.addLocationStore(new ZLinkInMemoryLocationStore());
            options.addRelocationStore(
                    new systems.zlink.framework.runtime.InMemoryRelocationStore());
            mesh.setRoutingIdPrefix("prestart");
            mesh.objects()
                    .server()
                    .addEntrySpot(EntrySpot.class)
                    .addSpotFactory("room", Room.class, factory -> factory.disableRelocation())
                    .addInstanceSpotFactory(
                            "instance", Instance.class, factory -> factory.disableRelocation())
                    .addActorFactory(
                            "actor",
                            Player.class,
                            PlayerFactory.class,
                            factory -> factory.disableRelocation());
        } else {
            mesh.setRoutingId(peerEndpoint == null ? NODE : RoutingId.from("prestart-source"));
            if (peerEndpoint != null) mesh.peerConnections().connect(peerEndpoint);
        }
        return options;
    }

    private static Supplier<CompletionStage<?>> pending(
            ZLinkFrameworkRuntime runtime, Entry entry) {
        Ping message = new Ping("value");
        Notice notice = new Notice("value");
        return switch (entry) {
            case CHANNEL_SEND -> {
                var call = runtime.client().sendToChannel("channel", notice);
                yield call::submit;
            }
            case CHANNEL_REQUEST -> {
                var call = runtime.client().requestToChannel("channel", message);
                yield () -> call.submit(String.class);
            }
            case NODE_SEND -> {
                var call = runtime.route().sendToNode("mesh", NODE, notice);
                yield call::submit;
            }
            case NODE_REQUEST -> {
                var call = runtime.route().requestToNode("mesh", NODE, message);
                yield () -> call.submit(String.class);
            }
            case SPOT_SEND -> {
                var call = runtime.route().sendToSpot("spot", notice);
                yield call::submit;
            }
            case SPOT_REQUEST -> {
                var call = runtime.route().requestToSpot("spot", message);
                yield () -> call.submit(String.class);
            }
            case INSTANCE_SEND -> {
                var call =
                        runtime.route()
                                .sendToSpot("instance", notice)
                                .instanceSpot("instance")
                                .inMesh("mesh");
                yield call::submit;
            }
            case INSTANCE_REQUEST -> {
                var call =
                        runtime.route()
                                .requestToSpot("instance", message)
                                .instanceSpot("instance")
                                .inMesh("mesh");
                yield () -> call.submit(String.class);
            }
            case ACTOR_SEND -> {
                var call = runtime.actorClient().sendToActor("actor", notice);
                yield call::submit;
            }
            case ACTOR_REQUEST -> {
                var call = runtime.actorClient().requestToActor("actor", message);
                yield () -> call.submit(String.class);
            }
            case FANOUT_PUBLISH -> {
                var call = runtime.fanout().publish("fanout", "topic", message);
                yield call::submit;
            }
            case SPOT_PUBLISH -> {
                var call =
                        runtime.spotPublisherClient().publish("mesh", "channel", "topic", message);
                yield call::submit;
            }
            case ACTOR_FIND -> () -> runtime.actorManager().find("actor");
            case SPOT_FIND -> () -> runtime.spotManager().find("spot");
        };
    }

    private static void success(CompletionStage<?> stage, Entry entry) throws Exception {
        Object result = stage.toCompletableFuture().get(5, TimeUnit.SECONDS);
        if (entry.name().endsWith("REQUEST")) assertEquals("ok", result);
        if (entry.name().endsWith("FIND")) assertTrue(((Optional<?>) result).isPresent());
    }

    private static void failure(CompletionStage<?> stage, ZLinkFrameworkErrorKind expected) {
        var thrown =
                assertThrows(CompletionException.class, () -> stage.toCompletableFuture().join());
        assertEquals(
                expected,
                assertInstanceOf(ZLinkFrameworkException.class, thrown.getCause()).kind());
    }

    public record Ping(String value) {}

    public record Notice(String value) {}

    public static final class NodeRequest implements ZLinkRequestHandler<Ping, String> {
        public CompletionStage<String> handle(Ping message, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture("ok");
        }
    }

    public static final class NodeSend implements ZLinkSendHandler<Notice> {
        public CompletionStage<Void> handle(Notice message, ZLinkMessageContext context) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class NodeRouteRequest implements ZLinkRouteRequestHandler<Ping, String> {
        public CompletionStage<String> handle(Ping message, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture("ok");
        }
    }

    public static final class NodeRouteSend implements ZLinkRouteSendHandler<Notice> {
        public CompletionStage<Void> handle(Notice message, ZLinkRouteMessageContext context) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Room implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(SpotRequest.class);
            context.handlers().addHandler(SpotSend.class);
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String id, systems.zlink.framework.messaging.ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class SpotRequest implements ZLinkSpotRequestHandler<Room, Ping, String> {
        public CompletionStage<String> handle(Room room, Ping message) {
            return CompletableFuture.completedFuture("ok");
        }
    }

    public static final class SpotSend implements ZLinkSpotPacketHandler<Room, Notice> {
        public CompletionStage<Void> handle(Room room, Notice message) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Instance implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;

        public Instance(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        public ZLinkInstanceSpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addPacket(InstanceRequest.class);
            context.handlers().addPacket(InstanceSend.class);
        }
    }

    public static final class InstanceRequest
            implements ZLinkSpotRequestHandler<Instance, Ping, String> {
        public CompletionStage<String> handle(Instance room, Ping message) {
            return CompletableFuture.completedFuture("ok");
        }
    }

    public static final class InstanceSend implements ZLinkSpotPacketHandler<Instance, Notice> {
        public CompletionStage<Void> handle(Instance room, Notice message) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class EntrySpot implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public EntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(ActorRequest.class);
            context.handlers().addHandler(ActorSend.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class ActorRequest {
        @ZLinkSpotActorRequest
        public CompletionStage<String> handle(Player actor, Ping message) {
            return CompletableFuture.completedFuture("ok");
        }
    }

    public static final class ActorSend {
        @ZLinkSpotActorSend
        public CompletionStage<Void> handle(Player actor, Notice message) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
