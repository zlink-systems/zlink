package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

/**
 * Runs every scenario of the shared {@code framework/runtime/conformance/spot-close-v1.json}
 * fixture against the Java runtime with an in-memory Location Store (spec 06-spot-address-messaging
 * §7 and §9, handler turn and execution gate §7).
 */
final class ZLinkSpotCloseConformanceTest {
    private static final long WAIT_SECONDS = 10;
    private static final String SPOT_TYPE = "close-spot";
    private static final String ACTOR_ID = "close-player";
    private static final Set<String> SCENARIOS =
            Set.of(
                    "close-absent-incarnation-is-false",
                    "close-other-generation-is-invalid-operation",
                    "close-with-membership-is-false-and-keeps-authority",
                    "close-after-accepted-join-observes-its-membership",
                    "failure-before-closing-commit-keeps-authority",
                    "on-closing-failure-is-diagnostic-and-cleanup-continues");
    private static final Set<String> INVARIANTS =
            Set.of(
                    "contextCloseReturnsValue",
                    "onClosingCallsPerAcceptedClose",
                    "onClosingFailureChangesCloseResult",
                    "managerCloseResubmitsToNewOwner",
                    "closeRetargetsCurrentIncarnation");

    // One scenario runs at a time; the Spot and Actor types reach these through static state.
    static final AtomicInteger ON_CLOSING_CALLS = new AtomicInteger();
    static final AtomicInteger HANDLER_CALLS = new AtomicInteger();
    static final List<String> EVENTS = new CopyOnWriteArrayList<>();
    static final AtomicReference<String> HANDLER_MODE = new AtomicReference<>("reply");
    static final AtomicBoolean ON_CLOSING_THROWS = new AtomicBoolean();
    static final AtomicBoolean HOLD_JOIN_CALLBACK = new AtomicBoolean();
    static volatile CompletableFuture<Void> handlerBlock = CompletableFuture.completedFuture(null);
    static volatile CompletableFuture<Void> handlerEntered = new CompletableFuture<>();
    static volatile CompletableFuture<Void> spotJoined = new CompletableFuture<>();
    static volatile CompletableFuture<Void> joinCallbackRelease = new CompletableFuture<>();
    static volatile CompletableFuture<Void> joinCompleted = new CompletableFuture<>();

    @Test
    void runsEverySpotCloseFixtureScenario() throws Exception {
        JsonNode fixture = new ObjectMapper().readTree(Files.readString(sharedFixture()));
        assertEquals("zlink.framework.spot-close", fixture.path("fixture").asText());
        assertEquals(1, fixture.path("version").asInt());
        fixture.path("invariants")
                .fieldNames()
                .forEachRemaining(
                        name -> assertTrue(INVARIANTS.contains(name), "unknown invariant " + name));
        List<String> failures = new ArrayList<>();
        // Both context surfaces return the Close completion result.
        for (Class<?> surface :
                List.of(
                        systems.zlink.framework.spots.ZLinkSpotContext.class,
                        systems.zlink.framework.spots.ZLinkInstanceSpotContext.class)) {
            boolean returnsValue = surface.getMethod("close").getReturnType() != void.class;
            if (returnsValue
                    != fixture.path("invariants").path("contextCloseReturnsValue").asBoolean()) {
                failures.add(surface.getSimpleName() + ".close returns a value");
            }
        }
        List<String> ran = new ArrayList<>();
        for (JsonNode scenario : fixture.path("scenarios")) {
            String name = scenario.path("name").asText();
            if (!SCENARIOS.contains(name)) {
                fail("unknown spot-close scenario " + name);
            }
            try {
                runScenario(scenario);
            } catch (AssertionError | Exception failure) {
                failures.add(name + ": " + failure);
            }
            ran.add(name);
        }
        assertEquals(SCENARIOS, Set.copyOf(ran));
        assertEquals(List.of(), failures);
    }

    private static void runScenario(JsonNode scenario) throws Exception {
        resetStatics();
        JsonNode given = scenario.path("given");
        JsonNode expect = scenario.path("expect");
        given.fieldNames()
                .forEachRemaining(
                        key ->
                                assertTrue(
                                        Set.of(
                                                        "runtime",
                                                        "authority",
                                                        "members",
                                                        "closeGeneration",
                                                        "closingCommit",
                                                        "failOnce",
                                                        "onClosing",
                                                        "handler")
                                                .contains(key),
                                        "unknown given " + key));
        String spotId = "close-" + UUID.randomUUID();
        FaultStore store = new FaultStore(new ZLinkInMemoryLocationStore(), spotId);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        String meshName = "close-mesh";
        var node = options.addRouteMesh(meshName);
        node.listen("inproc://spot-close-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("spot-close-" + UUID.randomUUID()));
        var objects = node.objects().server();
        objects.addEntrySpot(EntrySpot.class);
        objects.addSpotFactory(SPOT_TYPE, CloseSpot.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.disableRelocation());
        RoutingId nodeRid = options.registration().meshNodes().getFirst().routingId();
        ZLinkLocationRepository repository = new ZLinkProviderLocationRepository(store);
        Outcome outcome = new Outcome();

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            String authority = given.path("authority").asText();
            SpotRef ref;
            if ("Missing".equals(authority)) {
                ref = new SpotRef(spotId, 1, meshName, nodeRid);
            } else {
                ref = create(runtime, spotId);
                if ("previous".equals(given.path("closeGeneration").asText(""))) {
                    SpotRef previous = ref;
                    assertEquals(true, managerClose(runtime, previous));
                    ref = create(runtime, spotId);
                    assertNotEquals(previous.objectGeneration(), ref.objectGeneration());
                    resetStatics();
                    ref = previous;
                }
                if ("Closing".equals(authority)) {
                    commitClosing(repository, spotId);
                }
            }
            if (given.path("members").asInt(0) > 0) {
                join(runtime, spotId);
                assertTrue(joinCompleted.isDone());
            }
            if ("ownerFenceMismatch".equals(given.path("closingCommit").asText(""))) {
                store.conflictNextAuthorityPut.set(true);
            }
            if ("authorityReleased".equals(given.path("failOnce").asText(""))) {
                store.failNextAuthorityDelete.set(true);
            }
            if ("throws".equals(given.path("onClosing").asText(""))) {
                ON_CLOSING_THROWS.set(true);
            }
            if (given.has("handler")) {
                HANDLER_MODE.set(given.path("handler").asText());
            }
            CompletionStage<?> draining = null;
            if ("Draining".equals(given.path("runtime").asText())) {
                handlerBlock = new CompletableFuture<>();
                CompletableFuture<Reply> blocker = request(runtime, spotId);
                handlerEntered.get(WAIT_SECONDS, TimeUnit.SECONDS);
                draining = runtime.shutdown(Duration.ofSeconds(30));
                HANDLER_CALLS.set(0);
                outcome.blocker = blocker;
            }

            CapturedLog log = CapturedLog.install();
            try {
                act(
                        scenario.path("act").asText(),
                        runtime,
                        spotId,
                        ref,
                        store,
                        repository,
                        outcome);
            } finally {
                log.close();
                handlerBlock.complete(null);
            }
            if (draining != null) {
                draining.toCompletableFuture().get(WAIT_SECONDS * 3, TimeUnit.SECONDS);
            }
            verify(expect, outcome, repository, runtime, spotId, log);
        }
    }

    private static void act(
            String act,
            ZLinkFrameworkRuntime runtime,
            String spotId,
            SpotRef ref,
            FaultStore store,
            ZLinkLocationRepository repository,
            Outcome outcome)
            throws Exception {
        switch (act) {
            case "directRequestWithoutInstanceIntent" -> {
                CompletableFuture<Reply> reply = request(runtime, spotId);
                outcome.result = settle(reply);
                if (!"reply".equals(HANDLER_MODE.get())) {
                    store.authorityDeleted.get(WAIT_SECONDS, TimeUnit.SECONDS);
                }
            }
            case "managerClose" -> outcome.result = settle(closeStage(runtime, ref));
            case "managerCloseThenManagerCloseAgain" -> {
                outcome.results.add(settle(closeStage(runtime, ref)));
                outcome.authorityAfterFailure = authority(repository, spotId);
                outcome.results.add(settle(closeStage(runtime, ref)));
            }
            case "joinAcceptedThenManagerClose" -> {
                HOLD_JOIN_CALLBACK.set(true);
                scheduleJoin(runtime, spotId);
                spotJoined.get(WAIT_SECONDS, TimeUnit.SECONDS);
                CompletableFuture<Boolean> close = closeStage(runtime, ref);
                close.whenComplete((ignored, failure) -> EVENTS.add("closeCompleted"));
                joinCallbackRelease.complete(null);
                outcome.result = settle(close);
                joinCompleted.get(WAIT_SECONDS, TimeUnit.SECONDS);
            }
            default -> fail("unknown spot-close act " + act);
        }
    }

    private static void verify(
            JsonNode expect,
            Outcome outcome,
            ZLinkLocationRepository repository,
            ZLinkFrameworkRuntime runtime,
            String spotId,
            CapturedLog log)
            throws Exception {
        for (var field : (Iterable<java.util.Map.Entry<String, JsonNode>>) expect::fields) {
            String key = field.getKey();
            JsonNode value = field.getValue();
            switch (key) {
                case "result" -> assertResult(value, outcome.result);
                case "results" -> {
                    assertEquals(value.size(), outcome.results.size());
                    for (int index = 0; index < value.size(); index++) {
                        assertResult(value.get(index), outcome.results.get(index));
                    }
                }
                case "authority" -> assertEquals(value.asText(), authority(repository, spotId));
                case "authorityAfterFailure" ->
                        assertEquals(value.asText(), outcome.authorityAfterFailure);
                case "admission" -> {
                    assertEquals("open", value.asText());
                    int before = HANDLER_CALLS.get();
                    HANDLER_MODE.set("reply");
                    assertEquals(
                            spotId,
                            request(runtime, spotId).get(WAIT_SECONDS, TimeUnit.SECONDS).spotId());
                    assertEquals(before + 1, HANDLER_CALLS.get());
                }
                case "onClosingCalls" -> assertEquals(value.asInt(), ON_CLOSING_CALLS.get());
                case "handlerCalls" -> assertEquals(value.asInt(), HANDLER_CALLS.get());
                case "creationIntent" ->
                        // The act sends through requestToSpot without instanceSpot(...).
                        assertFalse(value.asBoolean());
                case "order" -> {
                    List<String> expected = new ArrayList<>();
                    value.forEach(item -> expected.add(item.asText()));
                    assertEquals(expected, List.copyOf(EVENTS));
                }
                case "diagnostics" -> {
                    for (JsonNode item : value) {
                        assertEquals("onClosingFailed", item.asText());
                        assertTrue(
                                log.sawOnClosingFailure(),
                                "OnClosing failure was not recorded in diagnostics");
                    }
                }
                default -> fail("unknown spot-close expectation " + key);
            }
        }
    }

    private static void assertResult(JsonNode expected, Object actual) {
        if (expected.isBoolean()) {
            assertEquals(expected.asBoolean(), actual);
            return;
        }
        String text = expected.asText();
        switch (text) {
            case "handlerReply" -> assertInstanceOf(Reply.class, actual);
            case "handlerFailure" -> {
                Throwable failure = assertInstanceOf(Throwable.class, actual);
                assertTrue(
                        messageChain(failure).contains("close-then-throw"),
                        "handler failure was not returned: " + failure);
            }
            case "failure" -> assertInstanceOf(Throwable.class, actual);
            default -> {
                ZLinkFrameworkException failure =
                        assertInstanceOf(
                                ZLinkFrameworkException.class,
                                actual,
                                "expected " + text + " but was " + actual);
                assertEquals(kind(text), failure.kind(), failure.getMessage());
            }
        }
    }

    private static ZLinkFrameworkErrorKind kind(String pascal) {
        return ZLinkFrameworkErrorKind.valueOf(
                pascal.replaceAll("([a-z])([A-Z])", "$1_$2").toUpperCase(Locale.ROOT));
    }

    private static Object settle(CompletionStage<?> stage) throws Exception {
        try {
            return stage.toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
        } catch (ExecutionException failure) {
            Throwable cause = failure.getCause();
            while (cause instanceof CompletionException && cause.getCause() != null) {
                cause = cause.getCause();
            }
            return cause;
        }
    }

    private static String messageChain(Throwable failure) {
        StringBuilder text = new StringBuilder();
        for (Throwable current = failure; current != null; current = current.getCause()) {
            text.append(current.getMessage()).append('\n');
        }
        return text.toString();
    }

    private static SpotRef create(ZLinkFrameworkRuntime runtime, String spotId) throws Exception {
        ZLinkSpotCreateResult created =
                runtime.spotManager()
                        .getOrCreate(spotId, SPOT_TYPE)
                        .submit()
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
        return created.spot();
    }

    private static CompletableFuture<Boolean> closeStage(
            ZLinkFrameworkRuntime runtime, SpotRef ref) {
        return runtime.spotManager().close(ref).toCompletableFuture();
    }

    private static boolean managerClose(ZLinkFrameworkRuntime runtime, SpotRef ref)
            throws Exception {
        return closeStage(runtime, ref).get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static CompletableFuture<Reply> request(ZLinkFrameworkRuntime runtime, String spotId) {
        return runtime.route()
                .requestToSpot(spotId, new Probe(spotId))
                .timeout(Duration.ofSeconds(WAIT_SECONDS))
                .submit(Reply.class)
                .toCompletableFuture();
    }

    private static void scheduleJoin(ZLinkFrameworkRuntime runtime, String spotId)
            throws Exception {
        ZLinkActorCreateResult.Created created =
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class,
                        runtime.actorManager()
                                .create(ACTOR_ID, "player")
                                .submit()
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS));
        assertEquals(
                "scheduled",
                runtime.actorClient()
                        .requestToActor(created.actor().actorId(), new JoinRequest(spotId))
                        .timeout(Duration.ofSeconds(WAIT_SECONDS))
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS));
    }

    private static void join(ZLinkFrameworkRuntime runtime, String spotId) throws Exception {
        scheduleJoin(runtime, spotId);
        joinCompleted.get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static void commitClosing(ZLinkLocationRepository repository, String spotId)
            throws Exception {
        String key = ZLinkAuthorityKeyCodec.spot(spotId);
        ZLinkAuthoritySnapshot snapshot =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        repository.read(key, () -> false).toCompletableFuture().get());
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        var current = codec.decode(snapshot.payload()).orElseThrow();
        byte[] closing =
                codec.encodeUser(
                        ZLinkServiceAuthorityPayloadCodec.State.CLOSING,
                        current.stableType(),
                        current.spotId(),
                        current.ownerId(),
                        current.ownerLeaseGeneration(),
                        current.meshName(),
                        current.nodeRid(),
                        current.nodeGeneration());
        repository
                .compareExchange(
                        key,
                        new ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                        new ZLinkAuthorityPut(closing),
                        () -> false)
                .toCompletableFuture()
                .get();
    }

    private static String authority(ZLinkLocationRepository repository, String spotId)
            throws Exception {
        var read =
                repository
                        .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                        .toCompletableFuture()
                        .get();
        if (read instanceof ZLinkAuthorityMissing) {
            return "Missing";
        }
        ZLinkAuthoritySnapshot snapshot = assertInstanceOf(ZLinkAuthoritySnapshot.class, read);
        return switch (new ZLinkServiceAuthorityPayloadCodec()
                .decode(snapshot.payload())
                .orElseThrow()
                .state()) {
            case CREATING -> "Creating";
            case READY -> "Ready";
            case CLOSING -> "Closing";
        };
    }

    private static void resetStatics() {
        ON_CLOSING_CALLS.set(0);
        HANDLER_CALLS.set(0);
        EVENTS.clear();
        HANDLER_MODE.set("reply");
        ON_CLOSING_THROWS.set(false);
        HOLD_JOIN_CALLBACK.set(false);
        handlerBlock = CompletableFuture.completedFuture(null);
        handlerEntered = new CompletableFuture<>();
        spotJoined = new CompletableFuture<>();
        joinCallbackRelease = new CompletableFuture<>();
        joinCompleted = new CompletableFuture<>();
    }

    private static Path sharedFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve("runtime/conformance/spot-close-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            candidate = current.resolve("framework/runtime/conformance/spot-close-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException("spot-close-v1.json fixture was not found");
    }

    private static final class Outcome {
        Object result;
        final List<Object> results = new ArrayList<>();
        String authorityAfterFailure;
        CompletableFuture<Reply> blocker;
    }

    /** Captures runtime log records so the test can find the OnClosing diagnostics record. */
    private static final class CapturedLog extends Handler implements AutoCloseable {
        private final List<LogRecord> records = new CopyOnWriteArrayList<>();

        static CapturedLog install() {
            CapturedLog log = new CapturedLog();
            Logger.getLogger("").addHandler(log);
            return log;
        }

        boolean sawOnClosingFailure() {
            for (LogRecord record : records) {
                if (String.valueOf(record.getMessage()).contains(OnClosingFailure.MARKER)) {
                    return true;
                }
                for (Throwable current = record.getThrown();
                        current != null;
                        current = current.getCause()) {
                    if (current instanceof OnClosingFailure) {
                        return true;
                    }
                }
                Object[] parameters = record.getParameters();
                if (parameters != null) {
                    for (Object parameter : parameters) {
                        if (String.valueOf(parameter).contains(OnClosingFailure.MARKER)) {
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        @Override
        public void publish(LogRecord record) {
            records.add(record);
        }

        @Override
        public void flush() {}

        @Override
        public void close() {
            Logger.getLogger("").removeHandler(this);
        }
    }

    /** Location Store that injects one Closing-commit conflict or one release failure. */
    private static final class FaultStore implements ZLinkLocationStore {
        private final ZLinkLocationStore inner;
        private final String spotId;
        final AtomicBoolean conflictNextAuthorityPut = new AtomicBoolean();
        final AtomicBoolean failNextAuthorityDelete = new AtomicBoolean();
        final CompletableFuture<Void> authorityDeleted = new CompletableFuture<>();

        FaultStore(ZLinkLocationStore inner, String spotId) {
            this.inner = inner;
            this.spotId = spotId;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            // The provider repository keys the authority row as "authority", kind and id joined by
            // NUL.
            String authorityKey = "\0" + spotId;
            boolean authorityPut =
                    request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStorePut put
                                                    && put.key().value().startsWith("authority\0")
                                                    && put.key().value().endsWith(authorityKey));
            boolean authorityDelete =
                    request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStoreDelete delete
                                                    && delete.key()
                                                            .value()
                                                            .startsWith("authority\0")
                                                    && delete.key().value().endsWith(authorityKey));
            if (authorityPut && conflictNextAuthorityPut.compareAndSet(true, false)) {
                return CompletableFuture.completedFuture(
                        new ZLinkStoreWriteConflict(java.time.Instant.now()));
            }
            if (authorityDelete && failNextAuthorityDelete.compareAndSet(true, false)) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("injected authority release failure"));
            }
            CompletionStage<ZLinkStoreWriteResult> written = inner.write(request, cancellation);
            if (!authorityDelete) {
                return written;
            }
            return written.thenApply(
                    result -> {
                        if (result instanceof ZLinkStoreWriteApplied) {
                            EVENTS.add("authorityReleased");
                            authorityDeleted.complete(null);
                        }
                        return result;
                    });
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    static final class OnClosingFailure extends RuntimeException {
        static final String MARKER = "spot-close-on-closing-failure";

        OnClosingFailure() {
            super(MARKER);
        }
    }

    public record Probe(String spotId) {}

    public record Reply(String spotId) {}

    public record JoinRequest(String spotId) {}

    public static final class CloseSpot implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public CloseSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
            ON_CLOSING_CALLS.incrementAndGet();
            EVENTS.add("onClosing");
            if (ON_CLOSING_THROWS.get()) {
                throw new OnClosingFailure();
            }
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            spotJoined.complete(null);
            if (HOLD_JOIN_CALLBACK.compareAndSet(true, false)) {
                return joinCallbackRelease.thenRun(() -> EVENTS.add("joinCompleted"));
            }
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotRequestHandler<CloseSpot, Probe, Reply> {
        @Override
        public CompletionStage<Reply> handle(CloseSpot spot, Probe request) {
            HANDLER_CALLS.incrementAndGet();
            handlerEntered.complete(null);
            switch (HANDLER_MODE.get()) {
                case "closeTwiceThenComplete" -> {
                    spot.context().close();
                    spot.context().close();
                    EVENTS.add("handlerReturned");
                    return CompletableFuture.completedFuture(new Reply(request.spotId()));
                }
                case "closeThenThrow" -> {
                    spot.context().close();
                    EVENTS.add("handlerThrew");
                    throw new IllegalStateException("close-then-throw");
                }
                default -> {
                    return handlerBlock.thenApply(ignored -> new Reply(request.spotId()));
                }
            }
        }
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, completion);

            joinCompleted.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class EntrySpot implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public EntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
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
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<EntrySpot, Player, JoinRequest, String> {
        @Override
        public CompletionStage<String> handle(
                EntrySpot spot, Player actor, ZLinkMessageContext context, JoinRequest request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }
}
