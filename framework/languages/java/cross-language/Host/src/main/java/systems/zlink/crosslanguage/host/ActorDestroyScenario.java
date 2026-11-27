package systems.zlink.crosslanguage.host;

import static systems.zlink.crosslanguage.host.ActorDestroyStore.await;

import org.springframework.context.ApplicationContext;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.monitoring.ZLinkPeerState;
import systems.zlink.framework.monitoring.ZLinkRouteMeshRuntime;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.BasicFileAttributes;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import java.util.function.BooleanSupplier;

public final class ActorDestroyScenario {
    private static final String ACTOR_TYPE = "cross-lang-actor-destroy-type";
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(5);
    private static final Set<String> SCENARIOS =
            Set.of("a", "b", "c", "d", "e", "f", "admissionCheck");
    private static final Set<String> DESTROY_LABELS =
            Set.of("remote", "duplicate", "stale", "fail", "retry", "admission");

    private final HostArgs args;
    private final ActorDestroyStore store;
    private final ActorDestroyEvents events;
    private final ZLinkActorManager actors;
    private final ZLinkActorClient client;
    private final ZLinkRouteMeshRuntime topology;
    private final ZLinkRouteMeshRuntimeOptions runtimeOptions;
    private final Path commandFile;
    private final Path admissionFile;
    private final Path startFile;
    private final Path stopFile;
    private final String actorId;
    private final String mesh;
    private final String scenario;
    private final Map<String, CompletableFuture<?>> destroys = new HashMap<>();
    private final List<CompletableFuture<?>> operations = new ArrayList<>();
    private CompletableFuture<?> creation;
    private ActorRef original;
    private ProbePhase probePhase = ProbePhase.ORDINARY;

    ActorDestroyScenario(HostArgs args, ApplicationContext context, ActorDestroyStore store) {
        this.args = args;
        this.store = store;
        events = store.events();
        actors = context.getBean(ZLinkActorManager.class);
        client = context.getBean(ZLinkActorClient.class);
        topology = context.getBean(ZLinkRouteMeshRuntime.class);
        runtimeOptions = context.getBean(ZLinkRouteMeshRuntimeOptions.class);
        actorId = args.require("actor-id");
        mesh = args.require("mesh-name");
        scenario = args.require("actor-destroy-scenario");
        String prefix = args.require("actor-destroy-control-prefix");
        commandFile = Path.of(prefix + ".command");
        admissionFile = Path.of(prefix + ".admission");
        startFile = Path.of(args.require("start-file"));
        String stop = args.option("stop-file", "");
        stopFile = stop.isBlank() ? null : Path.of(stop);
    }

    static boolean enabled(HostArgs args) {
        return args.mode().equals("actor-destroy-owner")
                || args.mode().equals("actor-destroy-caller");
    }

    static void configure(ZLinkFrameworkOptions options, HostArgs args, ZLinkLocationStore store) {
        String scenario = args.require("actor-destroy-scenario");
        if (!SCENARIOS.contains(scenario))
            throw new IllegalArgumentException("unknown scenario " + scenario);
        options.addLocationStore(store);
        options.setDefaultRequestTimeout(REQUEST_TIMEOUT);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        String meshName = args.require("mesh-name");
        var mesh =
                options.addRouteMesh(meshName)
                        .listen(args.require("bind-endpoint"))
                        .setRoutingId(RoutingId.from(args.require("node-rid")))
                        .setPlacementWeight(args.mode().equals("actor-destroy-owner") ? 100 : 0);
        String advertiseHost = args.option("advertise-host", "");
        if (!advertiseHost.isBlank()) mesh.setAdvertiseHost(advertiseHost);
        mesh.channelName(meshName).server();
        mesh.objects().client();
        var objects = mesh.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                ACTOR_TYPE, Actor.class, Factory.class, factory -> factory.disableRelocation());
    }

    void run() throws Exception {
        long sequence = 0;
        CommandRevision lastRead = null;
        try {
            while (!stopping() && !Files.exists(startFile)) Thread.sleep(5);
            while (!stopping()) {
                if (!Files.exists(commandFile)) {
                    Thread.sleep(5);
                    continue;
                }
                var attributes = Files.readAttributes(commandFile, BasicFileAttributes.class);
                var revision =
                        new CommandRevision(
                                attributes.fileKey(),
                                attributes.lastModifiedTime(),
                                attributes.size());
                if (revision.equals(lastRead)) {
                    Thread.sleep(5);
                    continue;
                }
                String row = Files.readString(commandFile);
                lastRead = revision;
                String[] command = row.split("\\R", 2)[0].split("\\t", -1);
                if (command.length < 3 || !command[0].equals(events.runId())) {
                    Thread.sleep(5);
                    continue;
                }
                long next = Long.parseLong(command[1]);
                if (next <= sequence) {
                    Thread.sleep(5);
                    continue;
                }
                if (next != sequence + 1)
                    throw new IllegalArgumentException("command sequence skipped");
                sequence = next;
                String verb = command[2];
                String arg = command.length == 4 ? command[3] : "";
                try {
                    execute(verb, arg);
                } catch (RuntimeException failure) {
                    events.emit(
                            "error", "seq", sequence, "command", verb, "message", message(failure));
                }
                events.emit("ack", "seq", sequence);
                if (verb.equals("finish")) return;
            }
        } finally {
            store.cancelHolds();
        }
    }

    private boolean stopping() {
        return stopFile != null && Files.exists(stopFile);
    }

    private void execute(String verb, String arg) {
        switch (verb) {
            case "create" -> create();
            case "placement" -> placement(Integer.parseInt(arg));
            case "peer-placement" -> waitForPlacement("peer-rid", Integer.parseInt(arg));
            case "peer" -> peer();
            case "find" -> find(arg);
            case "authority" -> authority(arg);
            case "hold-delete" -> store.armDelete();
            case "resume-delete" -> store.resumeDelete();
            case "hold-cas" -> {
                store.armClosing();
                probePhase = ProbePhase.PENDING_CAS;
            }
            case "fail-cas" -> {
                store.failClosing();
                probePhase = ProbePhase.AFTER_FAILURE;
            }
            case "destroy" -> destroy(arg);
            case "pending-destroy" -> pendingDestroy(arg);
            case "get-or-create" -> getOrCreate();
            case "pending-create" -> {
                requirePending(creation, "get-or-create");
                events.emit("createPending");
            }
            case "probe" -> submitProbe(arg);
            case "lease" -> events.emit("lease", "label", arg, "kind", store.lease());
            case "wait-lease-expired" -> {
                until(
                        () -> store.lease().equals("expired"),
                        "original owner lease expiration",
                        Duration.ofSeconds(45));
                events.emit("ownerLeaseExpired");
            }
            case "end-lease" -> store.endLease();
            case "allow-admission" -> allowAdmission();
            case "finish" -> {
                store.cancelHolds();
                CompletableFuture.allOf(operations.toArray(CompletableFuture[]::new)).join();
            }
            default -> throw new IllegalArgumentException("unknown command " + verb);
        }
    }

    private void allowAdmission() {
        if (!scenario.equals("admissionCheck") || !args.mode().equals("actor-destroy-caller")) {
            throw new IllegalStateException("allow-admission requires the admissionCheck caller");
        }
        try {
            Files.writeString(admissionFile, "");
        } catch (IOException failure) {
            throw new UncheckedIOException(failure);
        }
    }

    private void create() {
        var result = await(actors.create(actorId, ACTOR_TYPE).inMesh(mesh).submit());
        if (!(result instanceof ZLinkActorCreateResult.Created created)) {
            throw new IllegalStateException("initial actor creation did not create: " + result);
        }
        ActorRef ref = created.actor();
        if (!ref.nodeRid().equals(RoutingId.from(args.require("node-rid")))) {
            throw new IllegalStateException("initial actor is not local: " + ref);
        }
        remember(ref);
        long generation = await(probe("create"));
        if (generation != ref.objectGeneration())
            throw new IllegalStateException("initial probe generation differs");
        events.emit("created", "generation", generation);
    }

    private void placement(int weight) {
        runtimeOptions.mesh(mesh).setPlacementWeight(weight);
        waitForPlacement("node-rid", weight);
    }

    private void waitForPlacement(String ridOption, int weight) {
        var rid = RoutingId.from(args.require(ridOption));
        until(
                () -> store.placementPublished(mesh, rid, weight),
                "placement publication for " + rid,
                Duration.ofSeconds(30));
    }

    private void peer() {
        var peer = RoutingId.from(args.require("peer-rid"));
        until(
                () ->
                        topology.snapshot(mesh).peers().stream()
                                .anyMatch(
                                        item ->
                                                item.nodeRid().equals(peer)
                                                        && item.state() == ZLinkPeerState.READY),
                "peer admission for " + peer,
                Duration.ofSeconds(30));
    }

    private void find(String label) {
        var ref = await(actors.find(actorId));
        ref.ifPresent(this::remember);
        events.emit(
                "found",
                "label",
                label,
                "generation",
                ref.map(actor -> Long.toString(actor.objectGeneration())).orElse("none"));
    }

    private void remember(ActorRef ref) {
        if (original != null) return;
        var authority = store.authority();
        if (authority == null
                || !authority.generation().equals(Long.toString(ref.objectGeneration()))) {
            throw new IllegalStateException("cannot observe the original authority");
        }
        store.rememberOriginal(authority);
        original = ref;
    }

    private void authority(String label) {
        var authority = store.authority();
        if (authority == null) {
            events.emit("authority", "label", label, "read", "Missing");
            return;
        }
        events.emit(
                "authority",
                "label",
                label,
                "read",
                "Found",
                "version",
                authority.version(),
                "generation",
                authority.generation(),
                "state",
                authority.state(),
                "operationKind",
                authority.operation(),
                "owner",
                RoutingId.fromHex(authority.ridHex()),
                "ownerId",
                authority.ownerId(),
                "ownerGeneration",
                authority.ownerGeneration(),
                "leaseGeneration",
                authority.leaseGeneration(),
                "nodeGeneration",
                authority.nodeGeneration(),
                "actorId",
                authority.payload().actorId(),
                "actorType",
                authority.payload().stableType(),
                "spot",
                authority.payload().spot(),
                "spotKind",
                authority.payload().spotKind());
    }

    private void destroy(String label) {
        if (!DESTROY_LABELS.contains(label))
            throw new IllegalArgumentException("unknown destroy label " + label);
        if (original == null) throw new IllegalStateException("original ActorRef was not observed");
        if (label.equals("retry") && scenario.equals("e")) probePhase = ProbePhase.AFTER_COMMIT;
        String event =
                switch (label) {
                    case "duplicate" -> "ownerDuplicateReturned";
                    case "fail" -> "failedDestroyReturned";
                    default -> "destroyReturned";
                };
        var deadline = Instant.now().plus(REQUEST_TIMEOUT);
        var call = actors.destroy(original).toCompletableFuture();
        if (label.equals("admission")) events.emitAt(deadline, "destroyDeadline");
        destroys.put(label, call);
        operations.add(
                call.handle(
                        (result, failure) -> {
                            if (failure == null) events.emit(event, "label", label, "kind", result);
                            else {
                                var cause = unwrap(failure);
                                if (label.equals("admission")
                                        && kind(cause).equals("Unavailable")
                                        && "Actor destroy owner route is unavailable"
                                                .equals(cause.getMessage())) {
                                    events.emit("submissionRejected");
                                }
                                if (label.equals("fail") && injectedClosingFailure(cause)) {
                                    events.emit(
                                            event,
                                            "label",
                                            label,
                                            "kind",
                                            "error",
                                            "frameworkKind",
                                            kind(cause),
                                            "message",
                                            message(cause));
                                } else {
                                    events.emit(
                                            event,
                                            "label",
                                            label,
                                            "kind",
                                            kind(cause),
                                            "message",
                                            message(cause));
                                }
                            }
                            return null;
                        }));
    }

    private void pendingDestroy(String label) {
        requirePending(destroys.get(label), "destroy " + label);
        if (label.equals("duplicate")) events.emit("ownerDuplicatePending");
    }

    private void getOrCreate() {
        store.startingCreation();
        var call =
                actors.getOrCreate(actorId, ACTOR_TYPE).inMesh(mesh).submit().toCompletableFuture();
        creation = call;
        operations.add(
                call.handle(
                        (result, failure) -> {
                            if (failure != null) {
                                events.emit(
                                        "getOrCreateReturned",
                                        "kind",
                                        "error",
                                        "generation",
                                        "none",
                                        "message",
                                        message(failure));
                            } else {
                                switch (result) {
                                    case ZLinkActorCreateResult.Created created ->
                                            events.emit(
                                                    "getOrCreateReturned",
                                                    "kind",
                                                    "Created",
                                                    "generation",
                                                    created.actor().objectGeneration());
                                    case ZLinkActorCreateResult.Existing existing ->
                                            events.emit(
                                                    "getOrCreateReturned",
                                                    "kind",
                                                    "Existing",
                                                    "generation",
                                                    existing.actor().objectGeneration());
                                    case ZLinkActorCreateResult.Rejected ignored ->
                                            events.emit(
                                                    "getOrCreateReturned",
                                                    "kind",
                                                    "error",
                                                    "generation",
                                                    "none",
                                                    "message",
                                                    "actor creation rejected");
                                }
                            }
                            return null;
                        }));
    }

    private CompletionStage<Long> probe(String label) {
        return client.requestToActor(actorId, new ActorDestroyProbe(label))
                .submit(String.class)
                .thenApply(Long::parseLong);
    }

    private void submitProbe(String label) {
        var phase = probePhase;
        if (phase == ProbePhase.AFTER_FAILURE) store.retryProbe(label);
        operations.add(
                probe(label)
                        .handle(
                                (generation, failure) -> {
                                    if (failure == null) {
                                        events.emit(
                                                "probeReturned",
                                                "label",
                                                label,
                                                "kind",
                                                "return",
                                                "generation",
                                                generation);
                                    } else {
                                        String kind = kind(failure);
                                        events.emit(
                                                "probeReturned",
                                                "label",
                                                label,
                                                "kind",
                                                kind,
                                                "retryable",
                                                kind.equals("Unavailable"),
                                                "message",
                                                message(failure));
                                    }
                                    if (phase == ProbePhase.PENDING_CAS)
                                        events.emit("pendingCasProbeReturned", "label", label);
                                    if (phase == ProbePhase.AFTER_COMMIT)
                                        events.emit("postCommitProbeReturned", "label", label);
                                    return null;
                                })
                        .toCompletableFuture());
    }

    private static void requirePending(CompletableFuture<?> call, String name) {
        if (call == null || call.isDone())
            throw new IllegalStateException(name + " is not pending");
    }

    private static void until(BooleanSupplier condition, String description, Duration timeout) {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline)
                throw new IllegalStateException("timed out awaiting " + description);
            try {
                Thread.sleep(20);
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted awaiting " + description, failure);
            }
        }
    }

    private static Throwable unwrap(Throwable failure) {
        while (failure instanceof CompletionException && failure.getCause() != null) {
            failure = failure.getCause();
        }
        return failure;
    }

    private static String kind(Throwable failure) {
        return switch (unwrap(failure)) {
            case ZLinkFrameworkException framework ->
                    switch (framework.kind()) {
                        case INVALID_OPERATION -> "InvalidOperation";
                        case UNAVAILABLE -> "Unavailable";
                        case NOT_FOUND -> "NotFound";
                        case DEADLINE_EXCEEDED -> "deadline";
                        default -> "error";
                    };
            case TimeoutException ignored -> "deadline";
            default -> "error";
        };
    }

    private static boolean injectedClosingFailure(Throwable failure) {
        for (Throwable cause = failure; cause != null; cause = cause.getCause()) {
            if (cause instanceof ActorDestroyStore.InjectedClosingCasFailure) return true;
        }
        return false;
    }

    private static String message(Throwable failure) {
        var cause = unwrap(failure);
        return cause.getCause() == null ? cause.toString() : cause + ": " + cause.getCause();
    }

    private record CommandRevision(
            Object fileKey, java.nio.file.attribute.FileTime modified, long size) {}

    private enum ProbePhase {
        ORDINARY,
        PENDING_CAS,
        AFTER_FAILURE,
        AFTER_COMMIT
    }

    public record Actor(ZLinkActorContext context) implements ZLinkActor {}

    public static final class Factory implements ZLinkActorFactory {
        private final ActorDestroyStore store;

        public Factory(ZLinkLocationStore store) {
            this.store = (ActorDestroyStore) store;
        }

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            store.factoryStarted();
            return CompletableFuture.completedFuture(new Actor(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Actor> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Actor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record ActorDestroyProbe(String label) {}

    public static final class ProbeHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Actor, ActorDestroyProbe, String>,
                    AutoCloseable {
        private final ActorDestroyStore store;
        private final ActorDestroyEvents events;
        private long generation;

        public ProbeHandler(ZLinkLocationStore store) {
            this.store = (ActorDestroyStore) store;
            events = this.store.events();
        }

        @Override
        public CompletionStage<String> handle(
                Entry spot, Actor actor, ZLinkMessageContext context, ActorDestroyProbe probe) {
            generation = actor.context().objectGeneration();
            events.emit("probeHandled", "label", probe.label(), "generation", generation);
            if (store.isRetryProbe(probe.label()))
                events.emit("retryProbeHandled", "label", probe.label());
            return CompletableFuture.completedFuture(Long.toString(generation));
        }

        @Override
        public void close() {
            if (generation != 0) events.emit("cleanup", "generation", generation);
        }
    }
}
