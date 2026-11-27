package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.testutil.PerCallAllocation;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.regex.Pattern;

class ZLinkActorRequestDeadlineDiagnosticsTest {
    private static final String FLAG = "ZLINK_DEBUG_FRAMEWORK_RELOCATION";
    private static final String MARKER = "[zlink.runtime.relocation] node_request_deadline";
    private static final String MESH = "deadline-actors";
    private static final String TARGET = "deadline-target";
    private static final String SOURCE = "deadline-source";
    private static final String ROOM = "deadline-room";
    private static final Duration DEADLINE = Duration.ofSeconds(2);
    private static final CompletableFuture<Void> ENTERED = new CompletableFuture<>();
    private static final CompletableFuture<Void> RELEASE = new CompletableFuture<>();
    private static final BlockingQueue<ZLinkActorJoinCompletion> JOINS =
            new LinkedBlockingQueue<>();

    @TempDir Path outputDir;

    @Test
    void reportsExpiredActorRequestOnce() throws Exception {
        assertDeadline(run("actor", "1"));
    }

    @Test
    void reportsExpiredActorJoinOnce() throws Exception {
        assertDeadline(run("join", "1"));
    }

    @Test
    void omitsActorDeadlinesWithDiagnosticsDisabled() throws Exception {
        for (String kind : new String[] {"actor", "join"}) {
            String output = run(kind, "0");
            assertFalse(output.contains(MARKER), output);
        }
    }

    @Test
    void disabledDiagnosticsAllocateNoAdditionalBytes() throws Exception {
        String disabled = run("allocations", "0");
        assertTrue(disabled.contains("added="), disabled);
        run("allocations", "1");
    }

    private static void assertDeadline(String output) {
        var record = Pattern.compile(Pattern.quote(MARKER) + " \\{[^}]*}").matcher(output);
        assertTrue(record.find(), output);
        String event = record.group();
        assertFalse(record.find(), output);
        assertTrue(event.contains("meshName: '" + MESH + "'"), event);
        assertTrue(event.contains("targetRid: '" + TARGET + "'"), event);
        assertTrue(event.contains("stage: 'reply_wait'"), event);
        assertTrue(event.matches("(?s).*correlationId: '[1-9][0-9]*'.*"), event);
        assertTrue(event.matches("(?s).*routeGeneration: [1-9][0-9]*n.*"), event);
        double previous = 0;
        for (String field :
                new String[] {
                    "routeSelectedMs",
                    "peerAdmittedMs",
                    "bindingSubmittedMs",
                    "bindingAdmittedMs",
                    "elapsedMs"
                }) {
            var timestamp = Pattern.compile(field + ": ([0-9.E-]+)").matcher(event);
            assertTrue(timestamp.find(), event);
            double elapsed = Double.parseDouble(timestamp.group(1));
            assertTrue(elapsed >= previous, event);
            previous = elapsed;
        }
        assertTrue(event.contains("replyReceivedMs: undefined"), event);
        assertTrue(event.contains("localDispatchMs: undefined"), event);
    }

    private String run(String kind, String flag) throws Exception {
        Path output = outputDir.resolve(kind + "-" + flag + ".log");
        var process =
                new ProcessBuilder(
                                Path.of(System.getProperty("java.home"), "bin", "java").toString(),
                                "--enable-native-access=ALL-UNNAMED",
                                kind.equals("allocations") ? "-XX:-DoEscapeAnalysis" : "-ea",
                                "-cp",
                                System.getProperty("java.class.path"),
                                getClass().getName(),
                                kind)
                        .redirectErrorStream(true)
                        .redirectOutput(output.toFile());
        process.environment().put(FLAG, flag);
        var child = process.start();
        try {
            assertTrue(child.waitFor(25, TimeUnit.SECONDS), () -> "child timed out: " + output);
            String log = Files.readString(output);
            assertEquals(0, child.exitValue(), log);
            System.out.print(log);
            return log;
        } finally {
            if (child.isAlive()) {
                child.destroyForcibly();
                child.waitFor();
            }
        }
    }

    public static void main(String[] args) throws Exception {
        if (args[0].equals("allocations")) {
            measureAllocations();
            return;
        }
        boolean join = args[0].equals("join");
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        try (var source = start(SOURCE, null, "player", false, locations, relocations)) {
            if (join) {
                createActor(source, "warmup-player", "player", SOURCE);
                createActor(source, "local-player", "player", SOURCE);
            }
            String targetType = join ? "player" : "remote";
            try (var target =
                    start(
                            TARGET,
                            source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint(),
                            targetType,
                            join,
                            locations,
                            relocations)) {
                if (join) {
                    var room = await(target.spotManager().getOrCreate(ROOM, "room").submit());
                    assertEquals(RoutingId.from(TARGET), room.spot().nodeRid());
                } else {
                    createActor(target, "remote-player", targetType, TARGET);
                }
                assertEquals(new Echo(false), await(request(source, join, false)));
                if (join) {
                    var accepted =
                            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, nextJoin());
                    assertEquals(RoutingId.from(TARGET), accepted.actor().nodeRid());
                }
                try {
                    var pending = request(source, join, true);
                    await(ENTERED);
                    if (join) {
                        assertEquals(new Echo(true), await(pending));
                        assertEquals(
                                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                                assertInstanceOf(ZLinkActorJoinCompletion.Failed.class, nextJoin())
                                        .kind());
                    } else {
                        var failure = assertThrows(ExecutionException.class, () -> await(pending));
                        assertEquals(
                                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                                        .kind());
                    }
                } finally {
                    RELEASE.complete(null);
                }
            }
        }
    }

    private static void measureAllocations() {
        var lane = new ZLinkStateLane();
        var target = RoutingId.from(TARGET);
        try (var registry =
                new ZLinkServiceOperationRegistry(
                        lane,
                        new IllegalStateException("closed"),
                        TimeoutException::new,
                        () -> 0)) {
            for (boolean actorJoin : new boolean[] {false, true}) {
                var cost =
                        lane.awaitTurn(
                                () ->
                                        PerCallAllocation.minAlternatingWindow(
                                                new Supplier<>() {
                                                    private boolean observed;

                                                    @Override
                                                    public Object get() {
                                                        var id = ZLinkServiceOperationIds.next();
                                                        var operation =
                                                                prepareOperation(
                                                                        id, actorJoin, observed);
                                                        registry.<List<Message>>submitTerminal(
                                                                id,
                                                                DEADLINE,
                                                                terminal -> null,
                                                                Message::closeAll,
                                                                operation);
                                                        if (observed) {
                                                            var progress =
                                                                    operation.requestProgress();
                                                            progress.routeSelected(
                                                                    MESH, target, 1, 1, false);
                                                            progress.peerAdmitted();
                                                            progress.bindingSubmitted();
                                                            var admission =
                                                                    progress.bindingAdmission(null);
                                                            if (admission != null)
                                                                admission.complete(null);
                                                            progress.<List<Message>>replyDecoder(
                                                                            Function.identity())
                                                                    .apply(List.of());
                                                        }
                                                        assertTrue(registry.discard(id));
                                                        observed = !observed;
                                                        return operation;
                                                    }
                                                },
                                                1_000,
                                                10_000));
                long added = cost.measuredBytes() - cost.baselineBytes();
                System.out.printf(
                        "%s baseline=%d measured=%d added=%d%n",
                        actorJoin ? "join" : "actor",
                        cost.baselineBytes(),
                        cost.measuredBytes(),
                        added);
                if ("1".equals(System.getenv(FLAG))) assertTrue(added > 0, cost.toString());
                else {
                    // Disabled diagnostics may add no bytes. The baseline can include one-off JVM
                    // allocations, so only an increase over it is a cost.
                    assertTrue(added <= 0, cost.toString());
                }
            }
        }
    }

    private static ZLinkServiceOperationRegistry.Operation<List<Message>> prepareOperation(
            UUID id, boolean actorJoin, boolean observed) {
        if (actorJoin) {
            return observed
                    ? ZLinkServiceOperationRegistry.prepareRequestOperation(id)
                    : ZLinkServiceOperationRegistry.prepareOperation(id);
        }
        return observed
                ? ZLinkServiceOperationRegistry.prepareRequestOperation(
                        id, Function.identity(), null, Function.identity(), null)
                : ZLinkServiceOperationRegistry.prepareOperation(
                        id, Function.identity(), null, Function.identity(), null);
    }

    private static void createActor(
            ZLinkFrameworkRuntime runtime, String id, String actorType, String owner)
            throws Exception {
        var created =
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class,
                        await(runtime.actorManager().create(id, actorType).submit()));
        assertEquals(RoutingId.from(owner), created.actor().nodeRid());
    }

    private static ZLinkFrameworkRuntime start(
            String rid,
            String peer,
            String actorType,
            boolean room,
            ZLinkInMemoryLocationStore locations,
            InMemoryRelocationStore relocations)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var node = options.addRouteMesh(MESH);
        node.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from(rid));
        if (peer != null) node.peerConnections().connect(peer);
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class)
                .addActorFactory(
                        actorType,
                        Player.class,
                        Factory.class,
                        f -> f.preserveStateWith(StateAdapter.class));
        if (room) objects.addSpotFactory("room", Room.class, f -> f.disableRelocation());
        var runtime = ZLinkFrameworkRuntimeTestAccess.start(options);
        try {
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
            if (!runtime.isReady())
                assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
            assertTrue(runtime.isReady());
            return runtime;
        } catch (Exception | Error failure) {
            runtime.close();
            throw failure;
        }
    }

    private static CompletionStage<Echo> request(
            ZLinkFrameworkRuntime source, boolean join, boolean blocked) {
        String actorId = join ? (blocked ? "local-player" : "warmup-player") : "remote-player";
        return source.actorClient()
                .requestToActor(actorId, new Ping(join, blocked))
                .timeout(DEADLINE)
                .submit(Echo.class);
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(5, TimeUnit.SECONDS);
    }

    private static ZLinkActorJoinCompletion nextJoin() throws InterruptedException {
        var result = JOINS.poll(5, TimeUnit.SECONDS);
        assertNotNull(result);
        return result;
    }

    public record Ping(boolean join, boolean blocked) {}

    public record Echo(boolean blocked) {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            JOINS.add(completion);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class StateAdapter implements ZLinkActorRelocationAdapter<Player> {
        public CompletionStage<byte[]> capture(
                Player actor, ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        public CompletionStage<Void> restore(
                Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
            assertEquals(0, state.length);
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        public void configure() {
            context.handlers().addHandler(Handler.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Handler {
        @ZLinkSpotActorRequest
        public CompletionStage<Echo> ping(
                Entry spot, Player actor, ZLinkMessageContext context, Ping request) {
            var echo = new Echo(request.blocked());
            if (request.join()) {
                actor.context().joinSpot(ROOM, echo).timeout(DEADLINE).defer();
            } else if (request.blocked()) {
                ENTERED.complete(null);
                return RELEASE.thenApply(ignored -> echo);
            }
            return CompletableFuture.completedFuture(echo);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            if (request.decode(Echo.class).blocked()) {
                ENTERED.complete(null);
                return RELEASE.thenApply(ignored -> ZLinkSpotActorJoinResult.reject());
            }
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
