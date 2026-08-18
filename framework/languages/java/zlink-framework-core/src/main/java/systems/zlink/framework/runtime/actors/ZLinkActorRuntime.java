package systems.zlink.framework.runtime.actors;
import java.util.Arrays;
import java.util.HashMap;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.stream.Collectors;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkDirectJoinRelocationAuthority;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinRelocationPort;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import systems.zlink.framework.runtime.internal.backend.*;

import java.time.Duration;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorDirectory;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.actors.ZLinkActorCreateCall;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorGetOrCreateCall;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;

public final class ZLinkActorRuntime implements ZLinkActorManager, ZLinkActorDirectory {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkActorRuntime.class.getName());
    @FunctionalInterface
    public interface CreatedNotifier {
        CompletionStage<ZLinkActorCreateResponse> notify(
            RoutingId nodeRid,
            ZLinkActor actor,
            ZLinkMessage createRequest,
            Object createContext);
    }

    @FunctionalInterface
    public interface SourceActorLeaver {
        CompletionStage<Void> leave(ZLinkActor actor);

        /**
         * Cleans the source membership captured before a local join changes
         * the actor context. Existing leavers may keep the legacy behavior.
         */
        default CompletionStage<Void> leave(
            ZLinkActor actor,
            LocalMoveSource source) {
            return leave(actor);
        }
    }

    /** Source membership captured before a same-process join is committed. */
    public record LocalMoveSource(Object spotSurface, String spotId) {
    }

    /**
     * Framework-internal view of one captured Actor packet during a failed
     * remote move. The host uses it to put the packet back through the normal
     * local serial dispatch path.
     */
    public static final class TransferBacklogPacket implements AutoCloseable {
        private final ZLinkStreamHeader header;
        private final Message payload;
        private final byte[] acceptedJournalRecord;

        private TransferBacklogPacket(ZLinkActorHandoffPacket packet) {
            this.header = packet.header();
            this.payload = Message.from(packet.payload());
            this.acceptedJournalRecord = packet.acceptedJournalRecord();
        }

        public ZLinkStreamHeader header() {
            return header;
        }

        public Message payload() {
            return payload;
        }

        public byte[] acceptedJournalRecord() {
            return acceptedJournalRecord.clone();
        }

        @Override
        public void close() {
            payload.close();
        }
    }

    @FunctionalInterface
    public interface TransferBacklogRestorer {
        CompletionStage<Optional<Message>> restore(
            ZLinkActor actor,
            TransferBacklogPacket packet);
    }

    public interface LocalJoinCompleter {
        CompletionStage<Void> complete(ZLinkActor actor);

        void cancel(ZLinkActor actor);
    }

    @FunctionalInterface
    public interface CreationSubmitter {
        CompletionStage<ZLinkActorCreateResult> submit(
            String actorId,
            String actorType,
            ZLinkMessage createRequest,
            boolean getOrCreate,
            Duration timeout);
    }

    @FunctionalInterface
    public interface EntrySpotTargetSelector {
        CompletionStage<EntrySpotTarget> select(
            String actorType,
            Duration timeout);
    }

    @FunctionalInterface
    public interface MessageFollowNoticeSender {
        CompletionStage<Void> send(
            RoutingId sourceNodeRid,
            ZLinkServiceMessageFollowWireCodec.Notice notice);
    }

    public record EntrySpotTarget(
        RoutingId nodeRid,
        String spotId) {
        public EntrySpotTarget {
            Objects.requireNonNull(nodeRid, "nodeRid");
            systems.zlink.framework.runtime.internal.spots
                .ZLinkSpotIdValidator.requireValid(spotId);
        }
    }

    private final ZLinkInternalSpotNode spotNode;
    private volatile String meshName;
    private final Map<String, Class<? extends ZLinkActorFactory>> factories;
    private final ZLinkActorTransferRegistry actorTransfers;
    private final Duration defaultRequestTimeout;
    private final Duration messageFollowDuration;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkHandlerActivator handlerFactory;
    private final ZLinkStreamCodec defaultStreamCodec;
    private final ActorRegistry actorRegistry = new ActorRegistry();
    private final ZLinkActorDispatchSerials dispatches;
    private final ZLinkActorTransferHandoff handoff = new ZLinkActorTransferHandoff();
    private final ConcurrentMap<String, Long> transferStarts =
        new ConcurrentHashMap<>();
    private final Object actorCreationGate = new Object();
    private final Map<String, CompletableFuture<Void>>
        actorCreationTails = new HashMap<>();
    private CreatedNotifier createdNotifier =
        (ignoredNode, ignoredActor, ignoredRequest, ignoredContext) ->
            CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
    private Supplier<Object> actorCreateContextSupplier = () -> null;
    private Function<ZLinkActor, CompletionStage<Void>> disconnectedNotifier =
        ignored -> CompletableFuture.completedFuture(null);
    private SourceActorLeaver sourceActorLeaver =
        ignored -> CompletableFuture.completedFuture(null);
    private volatile TransferBacklogRestorer transferBacklogRestorer =
        (ignoredActor, ignoredPacket) -> CompletableFuture.failedFuture(
            new ZLinkConfigurationException(
                "remote Actor move backlog restorer is not configured"));
    private LocalJoinCompleter localJoinCompleter =
        unavailableLocalJoinCompleter();
    private Function<String, ZLinkSpot<?>> spotResolver = ignored -> null;
    private SpotTransportAddressResolver remoteAddressResolver;
    private final ZLinkActorLocationCoordinator locations =
        new ZLinkActorLocationCoordinator(actorRegistry::actorType);
    private ZLinkChannelRuntime routedTransport;
    private ZLinkActorSpotJoinCall.TransferTransport actorJoinTransferTransport;
    private Supplier<String> sourceEntrySpotId = () -> "";
    // Shared flow tracer (installed by the host); null = no tracing wired.
    private ZLinkMessageFlowTracer flow;
    private volatile boolean draining;
    private volatile boolean relocating;
    private volatile CreationSubmitter creationSubmitter;
    private final ConcurrentMap<String,
        Set<AcceptedHandoffOperation>> acceptedHandoffOperations =
            new ConcurrentHashMap<>();
    private volatile EntrySpotTargetSelector entrySpotTargetSelector =
        (ignoredType, ignoredTimeout) ->
            CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "eligible Entry Spot selection is unavailable"));
    private ZLinkRelayMetadataPolicy metadataPolicy = ZLinkRelayMetadataPolicy.EMPTY;
    //  Command 42 loss policy is `retransmit-until-sealed-or-deadline`
    //  (service-wire-v1.schema.json, relocationStateMachine.commandRules).
    //  The Actor lane is already sealed when this runs. If the Session owner
    //  cannot return exact command-43 evidence before this deadline, the move
    //  fails closed; it must never infer a high-water or continue unsealed.
    private final Duration sessionRelocationSealTimeout;
    private volatile ZLinkSessionRelocationPeerClient sessionRelocationSealer;
    private final ConcurrentMap<
        ZLinkServiceM6BWireCodec.RelocationIdentity,
        DirectJoinSessionAbortFlight> directJoinSessionAborts =
            new ConcurrentHashMap<>();
    private volatile ZLinkDirectJoinRelocation directJoinRelocation;
    private volatile ZLinkActorJoinRelocationPort actorJoinRelocationPort;
    private volatile MessageFollowNoticeSender messageFollowNoticeSender;

    public void beginDrain() {
        draining = true;
        stopDirectJoinSessionRecovery(
            "Actor runtime began drain with a pending direct-Join Session abort");
    }

    boolean relocationRecoveryStopped() {
        return draining;
    }

    public void beginRelocation() {
        relocating = true;
    }

    public void cancelRelocation() {
        relocating = false;
    }

    public void setDirectJoinRelocationStores(
        ZLinkLocationRepository authority) {
        directJoinRelocation = authority == null
            ? null
            : new ZLinkDirectJoinRelocation(
                authority,
                serializer);
    }

    public void setActorJoinRelocationPort(
        ZLinkActorJoinRelocationPort port) {
        actorJoinRelocationPort = Objects.requireNonNull(port, "port");
    }

    CompletionStage<ZLinkActorJoinRelocationPort.Submission>
        relocateActorJoin(
            ZLinkActorJoinRelocationPort.Goal goal,
            Duration timeout) {
        ZLinkActorJoinRelocationPort port = actorJoinRelocationPort;
        if (port == null) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "canonical Actor Join relocation is not installed"));
        }
        return port.relocate(goal, timeout);
    }

    public void admitActorJoin(
        ZLinkActorJoinRelocationPort.Admission admission) {
        ZLinkActorJoinRelocationPort port = actorJoinRelocationPort;
        if (port == null) {
            throw new ZLinkConfigurationException(
                "canonical Actor Join relocation is not installed");
        }
        port.admit(admission);
    }

    CompletionStage<ZLinkDirectJoinRelocation.Manifest>
        prepareDeferredJoinRelocation(
            UUID relocationId,
            ZLinkActorJoinOperationId operationId,
            ZLinkBackendActorRef actor,
            String actorType,
            String targetSpotId,
            RoutingId targetNodeRid,
            boolean restoreSnapshot,
            byte[] applicationState,
            List<
                ZLinkAsyncSerialQueue.QueuedRecord>
                acceptedJournal,
            byte[] rawReply) {
        ZLinkDirectJoinRelocation relocation =
            directJoinRelocation;
        if (relocation == null) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "cross-node deferred Actor Join requires Location and Relocation Stores"));
        }
        return relocation.prepareRelocation(
            relocationId,
            operationId,
            actor,
            actorType,
            targetSpotId,
            targetNodeRid,
            restoreSnapshot,
            applicationState,
            acceptedJournal,
            rawReply);
    }

    public CompletionStage<ZLinkDirectJoinRelocationAuthority.CommittedActorTenure>
        commitDeferredJoinRelocation(
        ZLinkActorSpotRoutePackets.TransferRequest request) {
        ZLinkDirectJoinRelocation relocation =
            directJoinRelocation;
        if (request.relocationManifest() == null) {
            return CompletableFuture.completedFuture(null);
        }
        if (relocation == null) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "direct Actor Join relocation authority is unavailable"));
        }
        return relocation.commitPrepared(
            request.relocationManifest(), request.actorRef());
    }

    public CompletionStage<DeferredJoinRelocationRoot>
        loadDeferredJoinRelocation(
            ZLinkActorSpotRoutePackets.TransferRequest request) {
        ZLinkDirectJoinRelocation relocation =
            directJoinRelocation;
        if (request.relocationManifest() == null || relocation == null) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "direct Actor Join relocation authority is unavailable"));
        }
        return relocation.loadPrepared(
                request.relocationManifest(),
                request.actorRef(),
                request.adapterKey() != null)
            .thenApply(root -> new DeferredJoinRelocationRoot(
                root.applicationState(),
                root.acceptedJournal()));
    }

    public CompletionStage<Void> abortDeferredJoinRelocation(
        ZLinkActorSpotRoutePackets.TransferRequest request) {
        ZLinkDirectJoinRelocation relocation =
            directJoinRelocation;
        if (request.relocationManifest() == null || relocation == null) {
            return CompletableFuture.completedFuture(null);
        }
        return relocation.abortPrepared(request.relocationManifest());
    }

    CompletionStage<Void> abortDeferredJoinRelocation(
            ZLinkDirectJoinRelocation.Manifest manifest) {
        ZLinkDirectJoinRelocation relocation =
            directJoinRelocation;
        return manifest == null || relocation == null
            ? CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "direct-Join abort has no relocation authority"))
            : relocation.abortPrepared(manifest);
    }

    public record DeferredJoinRelocationRoot(
        byte[] applicationState,
        List<ZLinkAsyncSerialQueue.QueuedRecord> acceptedJournal) {
        public DeferredJoinRelocationRoot {
            applicationState = applicationState.clone();
            acceptedJournal = List.copyOf(acceptedJournal);
        }
        @Override public byte[] applicationState() {
            return applicationState.clone();
        }
    }

    public CompletionStage<Void> deliverDeferredJoinAccepted(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkBackendActorRef actor) {
        if (request.relocationManifest() == null) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkDirectJoinRelocation relocation = directJoinRelocation;
        if (relocation == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "direct Actor Join relocation authority is unavailable"));
        }
        return deliverDeferredJoinAcceptedWithRetry(
                relocation,
                request.relocationManifest(),
                actor,
                0);
    }

    public CompletionStage<Void> publishDeferredJoinTargetReady(
        ZLinkActorSpotRoutePackets.TransferRequest request,
        ZLinkBackendActorRef actor) {
        if (request.relocationManifest() == null) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkDirectJoinRelocation relocation = directJoinRelocation;
        if (relocation == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "deferred Actor Join relocation authority is unavailable"));
        }
        ZLinkActor target = actorRegistry.actor(request.actorId());
        return target == null || isMoving(target)
            ? CompletableFuture.completedFuture(null)
            : relocation.publishTargetReady(
                request.relocationManifest(), actor);
    }

    private CompletionStage<Void> deliverDeferredJoinAcceptedWithRetry(
        ZLinkDirectJoinRelocation relocation,
        ZLinkDirectJoinRelocation.Manifest manifest,
        ZLinkBackendActorRef actor,
        int attempt) {
        return relocation.deliver(manifest, actor, this)
            .exceptionallyCompose(error -> {
                if (attempt >= 2 || draining) {
                    return CompletableFuture.failedFuture(error);
                }
                CompletableFuture<Void> delay = new CompletableFuture<>();
                CompletableFuture.delayedExecutor(
                        10L << attempt,
                        TimeUnit.MILLISECONDS)
                    .execute(() -> delay.complete(null));
                return delay.thenCompose(ignored ->
                    deliverDeferredJoinAcceptedWithRetry(
                        relocation,
                        manifest,
                        actor,
                        attempt + 1));
            });
    }

    /**
     * Completes after every admitted Actor turn, including a yielded terminal
     * continuation, has reached its terminal boundary.
     */
    public CompletionStage<Void> awaitDrainBarrier() {
        return dispatches.awaitQuiescence();
    }

    public boolean drainComplete() {
        for (ActorEntry entry : actorRegistry.entries()) {
            if (entry.context().moving()
                || handoff.messageFollowSource(entry.actor().context().actorId()).isEmpty()) {
                return false;
            }
        }
        return true;
    }

    public Set<String> activeActorTypes() {
        return actorRegistry.entries().stream()
            .map(ActorEntry::actorType)
            .collect(Collectors.toUnmodifiableSet());
    }

    public List<String> activeActorIds() {
        synchronized (this) {
            return actorRegistry.entries().stream()
                .map(entry -> entry.actor().context().actorId())
                .sorted()
                .toList();
        }
    }

    public CompletionStage<Integer> handoffActorsToEntrySpot(
        String actorType,
        String routeChannelName,
        RoutingId targetNodeRid) {
        if (actorType == null || actorType.isBlank()
            || routeChannelName == null || routeChannelName.isBlank() || targetNodeRid == null) {
            return CompletableFuture.completedFuture(0);
        }
        CompletionStage<Integer> transferred = CompletableFuture.completedFuture(0);
        for (ActorEntry entry : actorRegistry.entries()) {
            if (!actorType.equals(entry.actorType())) {
                continue;
            }
            transferred = transferred.thenCompose(count -> handoffActorToEntrySpot(
                    entry, routeChannelName, targetNodeRid)
                .thenApply(moved -> moved ? count + 1 : count));
        }
        return transferred;
    }

    private CompletionStage<Boolean> handoffActorToEntrySpot(
        ActorEntry entry,
        String routeChannelName,
        RoutingId targetNodeRid) {
        DefaultActorContext context = entry.context();
        CompletionStage<Void> readyToMove = context.moving()
            ? context.moveCompletion()
            : CompletableFuture.completedFuture(null);
        return readyToMove
            .thenCompose(ignored -> awaitActorDispatch(entry.actor()))
            .thenCompose(ignored -> {
            ActorEntry current = actorRegistry.byId.get(entry.actor().context().actorId());
            if (current != entry
                || context.actorRef() == null
                || !context.actorRef().nodeRid().equals(spotNode.routingId())) {
                return CompletableFuture.completedFuture(false);
            }
            return submitDrainHandoff(
                context,
                routeChannelName,
                targetNodeRid,
                ZLinkActorSpotJoinCall.newOperationId(),
                System.nanoTime() + defaultRequestTimeout.toNanos());
        });
    }

    private CompletionStage<Void> awaitActorDispatch(ZLinkActor actor) {
        return submitActorDispatch(
            actor.context().actorId(),
            () -> CompletableFuture.completedFuture(null));
    }

    private CompletionStage<Boolean> submitDrainHandoff(
        DefaultActorContext context,
        String routeChannelName,
        RoutingId targetNodeRid,
        ZLinkActorJoinOperationId operationId,
        long deadlineNanos) {
        Message request = Message.from(new byte[0]);
        CompletionStage<Boolean> attempt = new ZLinkActorSpotJoinCall(
                context,
                routeChannelName,
                targetNodeRid,
                request,
                defaultRequestTimeout,
                context.spotJoinServices())
            .execute(operationId, deadlineNanos)
            .thenApply(result -> result instanceof ZLinkActorJoinOutcome.Accepted);
        return attempt.whenComplete((moved, failure) -> request.close())
            .exceptionallyCompose(failure -> {
                Throwable cause = failure instanceof CompletionException && failure.getCause() != null
                    ? failure.getCause()
                    : failure;
                if (!(cause instanceof ZlinkSubmitException)
                    || System.nanoTime() >= deadlineNanos) {
                    return CompletableFuture.failedFuture(cause);
                }
                CompletableFuture<Boolean> retry = new CompletableFuture<>();
                CompletableFuture.delayedExecutor(25L, TimeUnit.MILLISECONDS)
                    .execute(() -> submitDrainHandoff(
                            context, routeChannelName, targetNodeRid,
                            operationId, deadlineNanos)
                        .whenComplete((moved, retryFailure) -> {
                            if (retryFailure != null) {
                                retry.completeExceptionally(retryFailure);
                            } else {
                                retry.complete(moved);
                            }
                        }));
                return retry;
            });
    }

    public void setMessageFlowTracer(
        ZLinkMessageFlowTracer flow) {
        this.flow = flow;
    }

    ZLinkFlowContext.Scope enterApplicationFlow() {
        return ZLinkFlowContext.enterCurrentOrCreate(
            ZLinkFlowOrigin.APPLICATION,
            flow != null && flow.captureEnabled());
    }

    public void setMetadataPolicy(
        Set<String> sessionToActorKeys,
        Set<String> actorToSessionKeys) {
        metadataPolicy =
            new ZLinkRelayMetadataPolicy(sessionToActorKeys, actorToSessionKeys);
    }

    public ZLinkActorRuntime(
        ZLinkInternalSpotNode spotNode,
        Map<String, Class<? extends ZLinkActorFactory>> factories,
        Duration defaultRequestTimeout,
        ZLinkMessageSerializer serializer) {
        this(
            spotNode,
            factories,
            Map.of(),
            defaultRequestTimeout,
            Duration.ofSeconds(5),
            serializer,
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.JSON);
    }

    public ZLinkActorRuntime(
        ZLinkInternalSpotNode spotNode,
        Map<String, Class<? extends ZLinkActorFactory>> factories,
        Duration defaultRequestTimeout,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory) {
        this(
            spotNode,
            factories,
            Map.of(),
            defaultRequestTimeout,
            Duration.ofSeconds(5),
            serializer,
            handlerFactory,
            ZLinkStreamCodec.JSON);
    }

    public ZLinkActorRuntime(
        ZLinkInternalSpotNode spotNode,
        Map<String, Class<? extends ZLinkActorFactory>> factories,
        Map<String, Class<? extends ZLinkActorRelocationAdapter<?>>> transferAdapters,
        Duration defaultRequestTimeout,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkStreamCodec defaultStreamCodec) {
        this(
            spotNode,
            factories,
            transferAdapters,
            defaultRequestTimeout,
            Duration.ofSeconds(5),
            serializer,
            handlerFactory,
            defaultStreamCodec);
    }

    public ZLinkActorRuntime(
        ZLinkInternalSpotNode spotNode,
        Map<String, Class<? extends ZLinkActorFactory>> factories,
        Map<String, Class<? extends ZLinkActorRelocationAdapter<?>>> transferAdapters,
        Duration defaultRequestTimeout,
        Duration messageFollowDuration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkStreamCodec defaultStreamCodec,
        BiFunction<
            ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            BiFunction<
                Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission) {
        this(
            spotNode,
            factories,
            transferAdapters,
            defaultRequestTimeout,
            messageFollowDuration,
            new ZLinkLocationOptions(),
            serializer,
            handlerFactory,
            defaultStreamCodec,
            admission,
            null);
    }

    public ZLinkActorRuntime(
        ZLinkInternalSpotNode spotNode,
        Map<String, Class<? extends ZLinkActorFactory>> factories,
        Map<String, Class<? extends ZLinkActorRelocationAdapter<?>>> transferAdapters,
        Duration defaultRequestTimeout,
        Duration messageFollowDuration,
        ZLinkLocationOptions locationOptions,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkStreamCodec defaultStreamCodec,
        BiFunction<
            ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            BiFunction<
                Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission,
        Executor serialExecutor) {
        if (serializer == null) {
            throw new ZLinkConfigurationException("serializer is required");
        }
        if (handlerFactory == null) {
            throw new ZLinkConfigurationException("handlerFactory is required");
        }
        this.spotNode = spotNode;
        this.dispatches = new ZLinkActorDispatchSerials(
            this,
            this::deferredJoinIncarnation,
            serialExecutor);
        this.meshName = spotNode.routingId().toString();
        this.factories = Map.copyOf(factories);
        this.defaultRequestTimeout = defaultRequestTimeout;
        this.messageFollowDuration = messageFollowDuration == null
            ? Duration.ofSeconds(5)
            : messageFollowDuration;
        this.sessionRelocationSealTimeout = Objects.requireNonNull(
            locationOptions,
            "locationOptions").sessionRelocationSealTimeout();
        this.serializer = serializer;
        this.handlerFactory = handlerFactory;
        this.actorTransfers = new ZLinkActorTransferRegistry(
            transferAdapters,
            handlerFactory);
        this.defaultStreamCodec =
            defaultStreamCodec == null ? ZLinkStreamCodec.JSON : defaultStreamCodec;
    }

    public ZLinkActorRuntime(
        ZLinkInternalSpotNode spotNode,
        Map<String, Class<? extends ZLinkActorFactory>> factories,
        Map<String, Class<? extends ZLinkActorRelocationAdapter<?>>> transferAdapters,
        Duration defaultRequestTimeout,
        Duration messageFollowDuration,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator handlerFactory,
        ZLinkStreamCodec defaultStreamCodec) {
        this(
            spotNode,
            factories,
            transferAdapters,
            defaultRequestTimeout,
            messageFollowDuration,
            serializer,
            handlerFactory,
            defaultStreamCodec,
            (ignoredBackend, ignoredKey) -> (ignoredSubmission, ignoredCleanup) ->
                CompletableFuture.failedFuture(new IllegalStateException(
                    "one-way admission factory is required")));
    }

    public void setMeshName(String meshName) {
        if (meshName == null || meshName.isBlank()) {
            throw new ZLinkConfigurationException("meshName is required");
        }
        this.meshName = meshName;
    }

    public String meshName() {
        return meshName;
    }

    @Override
    public ZLinkActorCreateCall create(String actorId, String actorType) {
        rejectAfterRelocationReady("Actor create");
        return new ActorCreateCall(actorId, actorType);
    }

    private CompletionStage<ZLinkActorCreateResult> submitCreate(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        boolean getOrCreate,
        String selectedMesh,
        Duration timeout) {
        streamTrace(STREAM_TRACE ? "actor-create submit actor=" + actorId
            + " type=" + actorType
            + " getOrCreate=" + getOrCreate
            + " mesh=" + selectedMesh
            + " timeout=" + timeout : null);
        requireActorId(actorId);
        if (draining || relocating) {
            return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.REJECTED,
                "Actor creation is rejected while relocation or shutdown admission is sealed"));
        }
        if (selectedMesh != null && !selectedMesh.equals(meshName)) {
            return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                "Actor Mesh was not found: " + selectedMesh));
        }
        CreationSubmitter submitter = creationSubmitter;
        if (submitter != null) {
            streamTrace(STREAM_TRACE ? "actor-create submit-remote actor=" + actorId : null);
            return submitter.submit(
                actorId, actorType, createRequest, getOrCreate, timeout);
        }
        streamTrace(STREAM_TRACE ? "actor-create submit-local actor=" + actorId : null);
        CompletionStage<ZLinkActorCreateResult> operation = serializeActorCreation(
            actorId,
            () -> getOrCreate
                ? getOrCreateActorOperation(actorId, actorType, createRequest)
                : createActorOperation(actorId, actorType, createRequest));
        return operation.toCompletableFuture().orTimeout(
            timeout.toNanos(), TimeUnit.NANOSECONDS);
    }

    private CompletionStage<ZLinkActorCreateResult> createActorOperation(
        String actorId,
        String actorType,
        ZLinkMessage createRequest) {
        AtomicReference<ZLinkActorCreateResponse> response =
            new AtomicReference<>();
        return createLocalActor(actorId, actorType, createRequest, true, response::set)
            .thenApply(actor -> {
                ZLinkActorCreateResponse admission = response.get();
                return (ZLinkActorCreateResult) new ZLinkActorCreateResult.Created(
                    publicRefFor(actor),
                    admission == null ? null : admission.reply());
            })
            .exceptionally(error -> {
                Throwable cause = unwrap(error);
                if (cause instanceof ActorCreateRejected rejected) {
                    return new ZLinkActorCreateResult.Rejected(rejected.reply());
                }
                throw new CompletionException(cause);
            });
    }

    private CompletionStage<ZLinkActor> createLocalActor(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        boolean failIfExists) {
        return createLocalActor(actorId, actorType, createRequest, failIfExists, true);
    }

    private CompletionStage<ZLinkActor> createLocalActor(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        boolean failIfExists,
        Consumer<ZLinkActorCreateResponse> responseSink) {
        return createLocalActor(
            actorId,
            actorType,
            createRequest,
            failIfExists,
            true,
            ZLinkLocationWriteIntent.NEW_CLAIM,
            responseSink);
    }

    private CompletionStage<ZLinkActor> createLocalActor(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        boolean failIfExists,
        boolean notifyCreated) {
        return createLocalActor(
            actorId,
            actorType,
            createRequest,
            failIfExists,
            notifyCreated,
            ZLinkLocationWriteIntent.NEW_CLAIM,
            ignored -> {
            });
    }

    private CompletionStage<ZLinkActor> createLocalActor(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        boolean failIfExists,
        boolean notifyCreated,
        ZLinkLocationWriteIntent intent) {
        return createLocalActor(
            actorId,
            actorType,
            createRequest,
            failIfExists,
            notifyCreated,
            intent,
            ignored -> {
            });
    }

    private CompletionStage<ZLinkActor> createLocalActor(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        boolean failIfExists,
        boolean notifyCreated,
        ZLinkLocationWriteIntent intent,
        Consumer<ZLinkActorCreateResponse> responseSink) {
        streamTrace(STREAM_TRACE ? "actor-create local-start actor=" + actorId
            + " type=" + actorType
            + " failIfExists=" + failIfExists
            + " notifyCreated=" + notifyCreated
            + " intent=" + intent : null);
        requireActorId(actorId);
        if ((draining || relocating)
            && intent != ZLinkLocationWriteIntent.TAKEOVER) {
            return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.REJECTED,
                "actor creation is rejected while the node is draining"));
        }
        if (createRequest == null) {
            throw new ZLinkConfigurationException("createRequest is required");
        }
        Class<? extends ZLinkActorFactory> factoryType = requireFactory(actorType);
        ZLinkActor existing = actorRegistry.actor(actorId);
        if (existing != null) {
            if (failIfExists) {
                throw new ZLinkConfigurationException("duplicate actor id: " + actorId);
            }
            streamTrace(STREAM_TRACE ? "actor-create local-existing actor=" + actorId : null);
            return CompletableFuture.completedFuture(existing);
        }
        Object createContext = actorCreateContextSupplier.get();
        if (locations.claimsActors(intent)) {
            streamTrace(STREAM_TRACE ? "actor-create location-claim-start actor=" + actorId : null);
            return locations
                .claimActor(
                    actorType,
                    actorId,
                    spotNode.routingId(),
                    intent,
                    () -> deactivateActorOnOwnershipLoss(actorId))
                .thenCompose(ignored -> {
                    streamTrace(STREAM_TRACE ? "actor-create location-claim-complete actor=" + actorId : null);
                    return activateLocalActor(
                        actorId,
                        actorType,
                        createRequest,
                        factoryType,
                        notifyCreated,
                        createContext,
                        responseSink)
                    .thenCompose(actor -> locations.setActorRef(
                            actorType,
                            actorId,
                            new ActorRef(
                                refFor(actor).actorId(),
                                refFor(actor).generation(),
                                meshName,
                                refFor(actor).nodeRid()))
                        .thenApply(ignoredSet -> actor))
                    ;
                })
                    .whenComplete((actor, error) -> {
                        streamTrace(STREAM_TRACE ? "actor-create local-complete actor=" + actorId
                            + " error=" + (error == null ? "none" : error) : null);
                        if (error != null) {
                            locations.releaseActor(actorType, actorId);
                        }
                    });
        }
        return activateLocalActor(
            actorId,
            actorType,
            createRequest,
            factoryType,
            notifyCreated,
            createContext,
            responseSink);
    }

    private CompletionStage<ZLinkActor> activateLocalActor(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        Class<? extends ZLinkActorFactory> factoryType,
        boolean notifyCreated,
        Object createContext,
        Consumer<ZLinkActorCreateResponse> responseSink) {
        return activateLocalActor(
            actorId,
            actorType,
            createRequest,
            factoryType,
            notifyCreated,
            createContext,
            responseSink,
            true,
            0);
    }

    private CompletionStage<ZLinkActor> activateLocalActor(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        Class<? extends ZLinkActorFactory> factoryType,
        boolean notifyCreated,
        Object createContext,
        Consumer<ZLinkActorCreateResponse> responseSink,
        boolean releaseLocationOnReject,
        long reservedObjectGeneration) {
        streamTrace(STREAM_TRACE ? "actor-create activate-start actor=" + actorId
            + " type=" + actorType
            + " reservedGeneration=" + reservedObjectGeneration : null);
        Message nativeCreateRequest = messageFromRequest(createRequest);
        ZLinkBackendActorRef actorRef;
        try {
            actorRef = reservedObjectGeneration > 0
                ? spotNode.createActor(
                    actorId,
                    reservedObjectGeneration,
                    nativeCreateRequest)
                : spotNode.createActor(actorId, nativeCreateRequest);
        } catch (RuntimeException ex) {
            nativeCreateRequest.close();
            throw ex;
        }
        if (actorRegistry.contains(actorId)) {
            throw new ZLinkConfigurationException("duplicate actor id: " + actorId);
        }
        DefaultActorContext context = new DefaultActorContext(actorRef);
        return ZLinkHandlerStages
            .fromSupplier(() -> createFactory(factoryType).create(context))
            .thenCompose(stage -> stage)
            .thenApply(actor -> {
                context.setActor(actor);
                actorRegistry.register(actorId, actorType, actor, context);
                streamTrace(STREAM_TRACE ? "actor-create factory-complete actor=" + actorId : null);
                return actor;
            })
            .thenCompose(actor -> {
                if (!notifyCreated) {
                    streamTrace(STREAM_TRACE ? "actor-create notify-skipped actor=" + actorId : null);
                    return CompletableFuture.completedFuture(actor);
                }
                streamTrace(STREAM_TRACE ? "actor-create notify-start actor=" + actorId : null);
                CompletableFuture<ZLinkActorCreateResponse> responseStage =
                    new CompletableFuture<>();
                return submitActorDispatch(
                    actor.context().actorId(),
                    () -> createdNotifier.notify(
                            actorRef.nodeRid(),
                            actor,
                            createRequest,
                            createContext)
                        .thenAccept(responseStage::complete))
                    .thenCompose(ignored -> responseStage)
                    .thenApply(response -> {
                        ZLinkActorCreateResponse effective =
                            response == null ? ZLinkActorCreateResponse.reject() : response;
                        streamTrace(STREAM_TRACE ? "actor-create notify-complete actor=" + actorId
                            + " accepted=" + effective.accepted() : null);
                        responseSink.accept(effective);
                        if (!effective.accepted()) {
                            discardLocalActor(
                                actor.context().actorId(),
                                releaseLocationOnReject);
                            throw new ActorCreateRejected(effective.reply());
                        }
                        return actor;
                    });
            })
            .whenComplete((actor, error) -> {
                streamTrace(STREAM_TRACE ? "actor-create activate-complete actor=" + actorId
                    + " error=" + (error == null ? "none" : error) : null);
                if (error != null) {
                    context.closeHandlerInstances();
                }
            });
    }

    CompletionStage<ZLinkActorCreateResult> createReservedActor(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        long objectGeneration,
        long authorityOwnerGeneration) {
        requireActorId(actorId);
        if (objectGeneration <= 0 || authorityOwnerGeneration <= 0) {
            throw new IllegalArgumentException(
                "reserved Actor generations must be positive");
        }
        return serializeActorCreation(actorId, () -> {
            ZLinkActor existing = actorRegistry.actor(actorId);
            if (existing != null) {
                return CompletableFuture.completedFuture(
                    new ZLinkActorCreateResult.Existing(
                        publicRefFor(existing)));
            }
            Class<? extends ZLinkActorFactory> factoryType =
                requireFactory(actorType);
            Object createContext = actorCreateContextSupplier.get();
            AtomicReference<
                ZLinkActorCreateResponse> response =
                    new AtomicReference<>();
            return activateLocalActor(
                    actorId,
                    actorType,
                    createRequest,
                    factoryType,
                    true,
                    createContext,
                    response::set,
                    false,
                    objectGeneration)
                .thenApply(actor -> {
                    spotNode.rememberActorAuthority(
                        refFor(actor),
                        authorityOwnerGeneration,
                        spotNode.localAuthorityLeaseGeneration());
                    ZLinkActorCreateResponse admission = response.get();
                    return (ZLinkActorCreateResult)
                        new ZLinkActorCreateResult.Created(
                            publicRefFor(actor),
                            admission == null
                                ? null : admission.reply());
                })
                .exceptionally(error -> {
                    Throwable cause = unwrap(error);
                    if (cause instanceof ActorCreateRejected rejected) {
                        return new ZLinkActorCreateResult.Rejected(
                            rejected.reply());
                    }
                    throw new CompletionException(cause);
                });
        });
    }

    CompletionStage<Void> discardReservedActor(String actorId) {
        return discardLocalActor(actorId, false);
    }

    public CompletionStage<ZLinkActor> materializeTransferredActor(
        String actorId,
        String actorType,
        String adapterKey,
        ZLinkMessage transferState) {
        return materializeTransferredActor(
            actorId, actorType, adapterKey, transferState, null);
    }

    public CompletionStage<ZLinkActor> materializeTransferredActor(
        String actorId,
        String actorType,
        String adapterKey,
        ZLinkMessage transferState,
        ZLinkBackendActorRef preparedActorRef) {
        requireActorId(actorId);
        Class<? extends ZLinkActorFactory> factoryType = requireFactory(actorType);
        ZLinkBackendActorRef reentryActorRef = preparedActorRef == null
            ? detachMessageFollowProxyForReentry(actorId)
            : preparedActorRef;
        if (actorRegistry.contains(actorId)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "target runtime already owns actor: " + actorId));
        }
        if (adapterKey != null && !adapterKey.equals(actorType)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor transfer adapter key does not match actor type: " + adapterKey));
        }
        return prepareTransferredActor(
                actorId,
                actorType,
                adapterKey,
                transferState,
                factoryType,
                reentryActorRef)
            .thenApply(prepared -> publishPreparedTransferredActor(prepared));
    }

    public CompletionStage<PreparedTransferredActor>
        prepareDeferredJoinTarget(
            ZLinkActorSpotRoutePackets.TransferRequest request,
            ZLinkMessage incomingState,
            DeferredJoinRelocationRoot root) {
        byte[] incomingEncoded =
            incomingState.toEncodedPayload(serializer).bytes();
        if (!Arrays.equals(
                incomingEncoded,
                root.applicationState())) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DATA_LOST,
                    "direct Actor Join payload differs from canonical relocation root"));
        }
        String actorId = request.actorId();
        String actorType = request.actorType();
        String adapterKey = request.adapterKey();
        requireActorId(actorId);
        Class<? extends ZLinkActorFactory> factoryType =
            requireFactory(actorType);
        //  An Actor that relocates straight back (A→B→A) can reach its old
        //  runtime again before that runtime finished releasing the Actor it
        //  just handed off. Internals 12 §"Ready 시점" makes the source's own
        //  resource cleanup follow-up work that runs after commit and does not
        //  gate admission, so a lagging local release is not an authority
        //  conflict and must not turn the new relocation into a terminal
        //  failure. Wait for the retiring registration inside a bounded local
        //  window; if it never clears, the same rejection stands.
        return ZLinkActorRetryScheduler.waitUntilRelayOrContinue(
                retiringSourceReleaseWindow(request),
                () -> localActorSlotFree(actorId))
            .thenCompose(ignored -> {
                ZLinkBackendActorRef reentryActorRef =
                    detachMessageFollowProxyForReentry(actorId);
                if (actorRegistry.contains(actorId)) {
                    return CompletableFuture
                        .<PreparedTransferredActor>failedFuture(
                            new ZLinkConfigurationException(
                                "target runtime already owns actor: "
                                    + actorId));
                }
                if (adapterKey != null && !adapterKey.equals(actorType)) {
                    return CompletableFuture
                        .<PreparedTransferredActor>failedFuture(
                            new ZLinkConfigurationException(
                                "actor transfer adapter key does not match "
                                    + "actor type: " + adapterKey));
                }
                ZLinkMessage transferState = ZLinkMessage.fromEncoded(
                    ZLinkEncodedPayload.from(root.applicationState()),
                    serializer);
                return prepareTransferredActor(
                    actorId,
                    actorType,
                    adapterKey,
                    transferState,
                    factoryType,
                    reentryActorRef == null
                        ? new ZLinkBackendActorRef(
                            spotNode.routingId(),
                            actorId,
                            request.actorGeneration())
                        : reentryActorRef);
            });
    }

    private static final Duration RETIRING_SOURCE_RELEASE_WINDOW =
        Duration.ofSeconds(2);

    private static Duration retiringSourceReleaseWindow(
        ZLinkActorSpotRoutePackets.TransferRequest request) {
        Duration deadline = Duration.ofMillis(
            Math.max(1L, request.timeoutMillis()));
        return deadline.compareTo(RETIRING_SOURCE_RELEASE_WINDOW) < 0
            ? deadline
            : RETIRING_SOURCE_RELEASE_WINDOW;
    }

    /**
     * Reports whether this runtime can take the stable Actor identity: either
     * nothing is registered, or the only registration is the Message Follow
     * proxy that {@code detachMessageFollowProxyForReentry} releases.
     */
    private synchronized boolean localActorSlotFree(String actorId) {
        ZLinkActor current = actorRegistry.actor(actorId);
        if (current == null) {
            return !actorRegistry.contains(actorId);
        }
        ZLinkActorTransferHandoff.MessageFollowSource followSource =
            handoff.messageFollowSource(actorId).orElse(null);
        if (followSource == null) {
            return false;
        }
        DefaultActorContext context = actorRegistry.context(current);
        return context != null
            && followSource.targetActorRef().equals(context.actorRef());
    }

    /**
     * Runs the target factory and byte[] Restore without making the Actor
     * discoverable through the local registry.
     */
    public CompletionStage<PreparedTransferredActor> prepareRelocatedActor(
        String actorId,
        String actorType,
        byte[] applicationState,
        boolean restoreSnapshot,
        systems.zlink.framework.runtime.internal.relocation
            .ZLinkRelocationAdapterRegistry adapters,
        ZLinkRelocationCancellation cancellation,
        ZLinkBackendActorRef preparedActorRef) {
        requireActorId(actorId);
        Objects.requireNonNull(applicationState, "applicationState");
        if (restoreSnapshot) {
            Objects.requireNonNull(adapters, "adapters");
        }
        Objects.requireNonNull(cancellation, "cancellation");
        Class<? extends ZLinkActorFactory> factoryType = requireFactory(actorType);
        if (actorRegistry.contains(actorId)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "target runtime already owns actor: " + actorId));
        }
        ZLinkBackendActorRef actorRef = preparedActorRef;
        if (actorRef == null) {
            Message createRequest = Message.from(new byte[0]);
            try {
                actorRef = spotNode.createActor(actorId, createRequest);
            } catch (RuntimeException failure) {
                createRequest.close();
                throw failure;
            }
        }
        DefaultActorContext context = new DefaultActorContext(actorRef);
        ZLinkBackendActorRef finalActorRef = actorRef;
        context.beginMove();
        CompletionStage<PreparedTransferredActor> activation = ZLinkHandlerStages
            .fromSupplier(() -> createFactory(factoryType).create(context))
            .thenCompose(stage -> stage)
            .thenCompose(actor -> {
                if (actor == null) {
                    return CompletableFuture.failedFuture(
                        new ZLinkConfigurationException(
                            "actor factory returned null: " + actorId));
                }
                context.setActor(actor);
                CompletionStage<Void> restore = restoreSnapshot
                    ? adapters.restoreActor(
                        actorType,
                        actor,
                        applicationState.clone(),
                        cancellation)
                    : CompletableFuture.completedFuture(null);
                return restore.thenApply(ignored ->
                    new PreparedTransferredActor(
                        actorId,
                        actorType,
                        actor,
                        context,
                        finalActorRef));
            });
        return activation.exceptionallyCompose(failure ->
            discardPreparedBackend(finalActorRef, context)
                .thenCompose(ignored -> CompletableFuture.failedFuture(
                    unwrap(failure))));
    }

    public synchronized ZLinkActor publishPreparedTransferredActor(
        PreparedTransferredActor prepared) {
        return publishPreparedTransferredActor(prepared, null, 0);
    }

    public synchronized ZLinkActor publishPreparedTransferredActor(
        PreparedTransferredActor prepared,
        String targetSpotId,
        long authorityOwnerGeneration) {
        Objects.requireNonNull(prepared, "prepared");
        prepared.requireOwner(this);
        if (prepared.terminal) {
            throw new IllegalStateException(
                "prepared Actor activation is already terminal");
        }
        if (actorRegistry.contains(prepared.actorId)) {
            throw new ZLinkConfigurationException(
                "target runtime already owns actor: " + prepared.actorId);
        }
        if (targetSpotId != null) {
            spotNode.registerTransferredActor(
                prepared.actorRef,
                targetSpotId,
                1L);
            spotNode.rememberActorAuthority(
                prepared.actorRef,
                authorityOwnerGeneration,
                spotNode.localAuthorityLeaseGeneration());
        }
        actorRegistry.register(
            prepared.actorId,
            prepared.actorType,
            prepared.actor,
            prepared.context);
        actorRegistry.markTransferred(prepared.actorId);
        prepared.published = true;
        return prepared.actor;
    }

    public synchronized void completePreparedTransferredActor(
        PreparedTransferredActor prepared) {
        Objects.requireNonNull(prepared, "prepared");
        prepared.requireOwner(this);
        if (!prepared.published || prepared.terminal) {
            throw new IllegalStateException(
                "prepared Actor must be published exactly once before completion");
        }
        prepared.context.endMove();
        actorRegistry.clearPendingTransfer(prepared.actorId);
        prepared.terminal = true;
    }

    public CompletionStage<Void> discardPreparedTransferredActor(
        PreparedTransferredActor prepared) {
        Objects.requireNonNull(prepared, "prepared");
        prepared.requireOwner(this);
        synchronized (this) {
            if (prepared.terminal) {
                return CompletableFuture.completedFuture(null);
            }
            if (prepared.published) {
                actorRegistry.remove(prepared.actorId, prepared.actor);
                dispatches.remove(prepared.actorId);
            }
            prepared.terminal = true;
        }
        return discardPreparedBackend(prepared.actorRef, prepared.context);
    }

    private synchronized ZLinkBackendActorRef detachMessageFollowProxyForReentry(String actorId) {
        ZLinkActorTransferHandoff.MessageFollowSource followSource =
            handoff.messageFollowSource(actorId).orElse(null);
        ZLinkActor oldActor = actorRegistry.actor(actorId);
        if (followSource == null || oldActor == null) {
            return null;
        }
        DefaultActorContext oldContext = actorRegistry.context(oldActor);
        if (oldContext == null || !followSource.targetActorRef().equals(oldContext.actorRef())) {
            return null;
        }
        // The Message Follow proxy retains the original native Actor binding so
        // late packets can still be forwarded. When the Actor returns to this
        // runtime, release that proxy binding before the transferred Actor
        // installs a new binding for the same stable identity.
        spotNode.closeActorBoundSession(
            followSource.sourceActorRef(),
            defaultRequestTimeout);
        actorRegistry.remove(actorId, oldActor);
        dispatches.remove(actorId);
        oldContext.closeHandlerInstances();
        return handoff.takeMessageFollowSource(actorId)
            .map(ZLinkActorTransferHandoff.MessageFollowSource::sourceActorRef)
            .orElse(null);
    }

    public CompletionStage<Void> rollbackTransferredActor(ZLinkActor actor) {
        DefaultActorContext context = requireContext(actor);
        ZLinkBackendActorRef actorRef = context.actorRef();
        String actorType = actorRegistry.actorTypeOrDefault(actor.context().actorId(), "");
        context.markLeft();
        return spotNode.destroyActor(actorRef, defaultRequestTimeout)
            .exceptionally(error -> null)
            .thenCompose(ignored -> locations.releaseActor(actorType, actor.context().actorId()))
            .thenRun(() -> {
                removeActorSessionRouteForContext(context);
                context.clearAfterDestroy();
                actorRegistry.remove(actor.context().actorId(), actor);
                dispatches.remove(actor.context().actorId());
            });
    }

    private CompletionStage<PreparedTransferredActor> prepareTransferredActor(
        String actorId,
        String actorType,
        String adapterKey,
        ZLinkMessage transferState,
        Class<? extends ZLinkActorFactory> factoryType,
        ZLinkBackendActorRef reentryActorRef) {
        ZLinkBackendActorRef actorRef = reentryActorRef;
        if (actorRef == null) {
            Message nativeCreateRequest = Message.from(new byte[0]);
            try {
                actorRef = spotNode.createActor(actorId, nativeCreateRequest);
            } catch (RuntimeException error) {
                nativeCreateRequest.close();
                throw error;
            }
        }
        DefaultActorContext context = new DefaultActorContext(actorRef);
        ZLinkBackendActorRef finalActorRef = actorRef;
        context.beginMove();
        CompletionStage<PreparedTransferredActor> activation =
            ZLinkHandlerStages.fromStageSupplier(() -> adapterKey == null
                ? createFactory(factoryType).create(context)
                : actorTransfers.transferIn(
                    actorType,
                    actorId,
                    context,
                    transferState,
                    factoryType))
            .thenApply(actor -> {
                if (actor == null) {
                    throw new ZLinkConfigurationException(
                        "actor transfer did not materialize an actor: " + actorId);
                }
                context.setActor(actor);
                return new PreparedTransferredActor(
                    actorId,
                    actorType,
                    actor,
                    context,
                    finalActorRef);
            });
        return activation.exceptionallyCompose(failure ->
            discardPreparedBackend(finalActorRef, context)
                .thenCompose(ignored -> CompletableFuture.failedFuture(
                    unwrap(failure))));
    }

    private CompletionStage<Void> discardPreparedBackend(
        ZLinkBackendActorRef actorRef,
        DefaultActorContext context) {
        context.clearAfterDestroy();
        return spotNode.destroyActor(actorRef, defaultRequestTimeout)
            .exceptionally(error -> null);
    }

    public final class PreparedTransferredActor {
        private final String actorId;
        private final String actorType;
        private final ZLinkActor actor;
        private final DefaultActorContext context;
        private final ZLinkBackendActorRef actorRef;
        private boolean published;
        private boolean terminal;

        private PreparedTransferredActor(
            String actorId,
            String actorType,
            ZLinkActor actor,
            DefaultActorContext context,
            ZLinkBackendActorRef actorRef) {
            this.actorId = actorId;
            this.actorType = actorType;
            this.actor = actor;
            this.context = context;
            this.actorRef = actorRef;
        }

        public ZLinkActor actor() {
            return actor;
        }

        public ZLinkBackendActorRef actorRef() {
            return actorRef;
        }

        public String actorId() {
            return actorId;
        }

        private void requireOwner(ZLinkActorRuntime owner) {
            if (ZLinkActorRuntime.this != owner) {
                throw new IllegalArgumentException(
                    "prepared Actor belongs to another runtime");
            }
        }
    }

    CompletionStage<ZLinkActorTransferRegistry.TransferState> transferOut(ZLinkActor actor) {
        String actorType = actorRegistry.actorType(actor.context().actorId());
        if (actorType == null) {
            throw new ZLinkConfigurationException(
                "actor type is not registered for transfer: " + actor.context().actorId());
        }
        return actorTransfers.transferOut(actorType, actor);
    }

    CompletionStage<Void> beginRemoteMove(ZLinkActor actor) {
        DefaultActorContext context = requireContext(actor);
        context.beginMove();
        handoff.begin(actor.context().actorId());
        if (ZLinkRuntimeMetrics.enabled()) {
            transferStarts.put(actor.context().actorId(), System.nanoTime());
            ZLinkRuntimeMetrics.record("zlink.actor.transfer.pending_requests.count",
                handoff.pendingCount(actor.context().actorId()), Map.of());
        }
        return CompletableFuture.completedFuture(null);
    }

    public CompletionStage<Optional<Message>> captureMovingPacket(
        ZLinkActor actor,
        ZLinkStreamHeader header,
        Message payload) {
        return captureMovingPacket(actor, header, payload, null);
    }

    public CompletionStage<Optional<Message>> captureMovingPacket(
        ZLinkActor actor,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute) {
        return captureMovingPacket(actor, header, payload, replyRoute, 0);
    }

    public CompletionStage<Optional<Message>> captureMovingPacket(
        ZLinkActor actor,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute,
        long acceptedSessionSequence) {
        if (header.requestSequence().isPresent()) {
            return null;
        }
        byte[] acceptedJournalRecord = encodeLocalSessionActorAccepted(
            actor, header, payload, acceptedSessionSequence);
        if (acceptedJournalRecord.length == 0) {
            // Pre-Captured boundary: the frame was never accepted, so the
            // session owner keeps redelivery ownership. Surface the retryable
            // moving/unavailable terminal instead of a configuration failure.
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "Actor '" + actor.context().actorId() + "' is moving:"
                    + " bound-session Actor send cannot cross relocation without"
                    + " an exact accepted journal fence");
        }
        if (replyRoute == null && header.requestSequence().isPresent()) {
            DefaultActorContext context = requireContext(actor);
            ZLinkActorContextState.BoundSessionSource source =
                context.boundSessionSourceSnapshot();
            if (source != null) {
                replyRoute = new ZLinkActorReplyRoute(
                    context.actorRef(),
                    source.sourceNodeRid(),
                    source.sourceSessionRid(),
                    header.requestSequence().orElseThrow(),
                    0);
            }
        }
        ZLinkActorHandoffPacket packet =
            handoff.capture(
                actor.context().actorId(), header, payload, replyRoute,
                acceptedJournalRecord);
        if (packet == null) {
            return null;
        }
        traceActorTransferMarker("handoff_backlog", actor.context().actorId(),
            Long.toString(packet.arrivalIndex()));
        if (replyRoute != null) {
            traceActorTransferMarker(
                "handoff_request_frame",
                actor.context().actorId(),
                Long.toUnsignedString(replyRoute.requestId())
                    + ":" + Integer.toUnsignedString(replyRoute.flags()));
        }
        return packet.reply();
    }

    List<ZLinkActorHandoffPacket> takeRemoteMoveBacklog(ZLinkActor actor) {
        return handoff.take(actor.context().actorId());
    }

    List<ZLinkActorHandoffPacket> finishRemoteMoveBacklog(ZLinkActor actor) {
        return handoff.finish(actor.context().actorId());
    }

    CompletionStage<Void> restoreRemoteMoveBacklog(
        ZLinkActor actor,
        List<ZLinkActorHandoffPacket> committedBacklog) {
        DefaultActorContext context = requireContext(actor);
        List<ZLinkActorHandoffPacket> packets = handoff.takeForRestore(
            actor.context().actorId(), committedBacklog);
        CompletionStage<Void> replay = CompletableFuture.completedFuture(null);
        for (int index = 0; index < packets.size(); index++) {
            ZLinkActorHandoffPacket packet = packets.get(index);
            int remainingStart = index + 1;
            replay = replay
                .thenCompose(ignored -> restoreRemoteMovePacket(actor, packet))
                .whenComplete((ignored, error) -> {
                    if (error != null && remainingStart < packets.size()) {
                        failHandoffPackets(
                            packets.subList(remainingStart, packets.size()),
                            unwrapTransferFailure(error));
                    }
                });
        }
        return replay.whenComplete((ignored, error) -> {
            if (error == null) {
                context.endMove();
            } else {
                context.failMove(unwrapTransferFailure(error));
            }
        });
    }

    private CompletionStage<Void> restoreRemoteMovePacket(
        ZLinkActor actor,
        ZLinkActorHandoffPacket packet) {
        TransferBacklogPacket view = new TransferBacklogPacket(packet);
        CompletionStage<Optional<Message>> restored;
        try {
            restored = Objects.requireNonNull(
                transferBacklogRestorer.restore(actor, view),
                "remote Actor move backlog restorer returned null");
        } catch (Throwable failure) {
            view.close();
            if (packet.fail(failure)) {
                packet.close();
            }
            return CompletableFuture.failedFuture(failure);
        }
        return restored.handle((reply, error) -> {
            try {
                if (error != null) {
                    if (packet.fail(unwrapTransferFailure(error))) {
                        packet.close();
                    }
                    throw new CompletionException(unwrapTransferFailure(error));
                }
                packet.complete(reply == null ? Optional.empty() : reply);
                return null;
            } finally {
                view.close();
            }
        });
    }

    private static void failHandoffPackets(
        List<ZLinkActorHandoffPacket> packets,
        Throwable error) {
        packets.forEach(packet -> {
            if (packet.fail(error)) {
                packet.close();
            }
        });
    }

    private static Throwable unwrapTransferFailure(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException
                || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    public boolean claimAcceptedHandoffOperation(
        ZLinkActor actor,
        long operationHigh,
        long operationLow) {
        DefaultActorContext context = requireContext(actor);
        if (operationHigh == 0 && operationLow == 0) {
            throw new ZLinkConfigurationException(
                "accepted Actor handoff operation id must not be zero");
        }
        AcceptedHandoffOperation operation = new AcceptedHandoffOperation(
            context.objectGeneration(), operationHigh, operationLow);
        Set<AcceptedHandoffOperation> operations =
            acceptedHandoffOperations.computeIfAbsent(
                actor.context().actorId(),
                ignored -> ConcurrentHashMap.newKeySet());
        if (operations.size() >= 4096 && !operations.contains(operation)) {
            throw new ZLinkConfigurationException(
                "accepted Actor handoff operation retention is full");
        }
        return operations.add(operation);
    }

    private record AcceptedHandoffOperation(
        long objectGeneration,
        long operationHigh,
        long operationLow) {
    }

    public CompletionStage<Void> leaveSourceForLocalMove(ZLinkActor actor) {
        LocalMoveSource source = beginLocalMove(actor);
        return cleanupSourceForLocalMove(actor, source)
            .thenRun(() -> requireContext(actor).markLeft());
    }

    public LocalMoveSource beginLocalMove(ZLinkActor actor) {
        DefaultActorContext context = requireContext(actor);
        context.beginMove();
        return new LocalMoveSource(context.currentSpot(), context.joinedSpotId());
    }

    public CompletionStage<Void> leaveSourceForLocalMove(
        ZLinkActor actor,
        LocalMoveSource source) {
        CompletionStage<Void> cleanup = cleanupSourceForLocalMove(actor, source);
        return cleanup.thenRun(() -> requireContext(actor).markLeft());
    }

    public CompletionStage<Void> cleanupSourceForLocalMove(
        ZLinkActor actor,
        LocalMoveSource source) {
        requireContext(actor);
        if (source == null || source.spotId() == null) {
            return CompletableFuture.completedFuture(null);
        }
        return sourceActorLeaver.leave(actor, source);
    }

    /**
     * Sends the source lifecycle notification for a local Join without making
     * the target Join completion wait for the notification to finish.
     */
    public void notifySourceForLocalMove(
        ZLinkActor actor,
        LocalMoveSource source) {
        try {
            CompletionStage<Void> cleanup = cleanupSourceForLocalMove(actor, source);
            if (cleanup != null) {
                cleanup.exceptionally(ignored -> null);
            }
        } catch (Throwable ignored) {
            // Source OnLeaveActor is a one-way notification. Its failure must
            // not turn an already committed target Join into a failed Join.
        }
    }

    CompletionStage<Void> leaveSourceForRemoteMove(ZLinkActor actor) {
        DefaultActorContext context = requireContext(actor);
        String currentSpotId = context.joinedSpotId();
        return sourceActorLeaver.leave(actor)
            .thenCompose(ignored -> {
                return currentSpotId == null || actorRegistry.isRoutedTransfer(actor.context().actorId())
                ? CompletableFuture.completedFuture(null)
                : spotNode.leaveActor(
                        context.actorRef(),
                        currentSpotId,
                        defaultRequestTimeout)
                    .thenAccept(parts -> parts.forEach(Message::close));
            })
            .thenRun(context::markLeft);
    }

    CompletionStage<Void> leaveSourceForCoreRemoteMove(ZLinkActor actor) {
        DefaultActorContext context = requireContext(actor);
        return sourceActorLeaver.leave(actor).thenRun(context::markLeft);
    }

    /**
     * Dispatches the source lifecycle notification after the remote location
     * commit. The notification is deliberately one-way: the source context
     * must stop advertising its old membership without making the committed
     * target wait for the callback result.
     */
    void notifySourceForCoreRemoteMove(ZLinkActor actor) {
        DefaultActorContext context = requireContext(actor);
        try {
            CompletionStage<Void> notification = sourceActorLeaver.leave(actor);
            if (notification != null) {
                notification.exceptionally(ignored -> null);
            }
        } catch (Throwable ignored) {
            // Source OnLeaveActor is a one-way notification after commit.
        }
        context.markLeft();
    }

    void cancelRemoteMove(ZLinkActor actor) {
        failTransferBacklog(actor, new ZLinkConfigurationException("actor transfer was cancelled"));
        requireContext(actor).endMove();
    }

    public void completeRemoteMove(ZLinkActor actor) {
        requireContext(actor).endMove();
        Long started = transferStarts.remove(actor.context().actorId());
        ZLinkRuntimeMetrics.increment("zlink.actor.transfers", Map.of());
        if (started != null) {
            ZLinkRuntimeMetrics.record("zlink.actor.transfer.duration",
                Duration.ofNanos(System.nanoTime() - started), Map.of());
        }
    }

    void abandonSourceLocationOwnership(ZLinkActor actor) {
        locations.abandonActor(actor.context().actorId());
    }

    void retainMessageFollowSource(
        ZLinkActor actor,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef,
        SpotTransportAddress targetAddress,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute) {
        Objects.requireNonNull(targetAddress, "targetAddress");
        Objects.requireNonNull(targetRoute, "targetRoute");
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute =
            localMessageFollowSourceRoute(sourceActorRef);
        handoff.retain(
            actor.context().actorId(), sourceActorRef, targetActorRef,
            targetAddress, sourceRoute, targetRoute, messageFollowDuration,
            this::removeMessageFollowSource);
        traceRetainedMessageFollowSource(actor, sourceActorRef);
    }

    CompletionStage<Optional<ZLinkStoreLocationResolvers.ActorRoute>>
        resolveMessageFollowTargetRoute(
            ZLinkBackendActorRef targetActorRef,
            SpotTransportAddress targetAddress) {
        try {
            return locations.resolveStoredActorRoute(targetActorRef.actorId())
                .thenApply(route -> {
                    if (!messageFollowTargetRouteMatches(
                        route, targetActorRef, targetAddress)) {
                        return Optional.<ZLinkStoreLocationResolvers.ActorRoute>empty();
                    }
                    return Optional.of(route);
                })
                .exceptionally(failure -> {
                    LOGGER.warning("Message Follow target route was not retained for actor "
                        + targetActorRef.actorId() + ": "
                        + unwrapCompletionFailure(failure));
                    return Optional.<ZLinkStoreLocationResolvers.ActorRoute>empty();
                });
        } catch (RuntimeException failure) {
            LOGGER.warning("Message Follow target route lookup failed for actor "
                + targetActorRef.actorId() + ": " + failure);
            return CompletableFuture.completedFuture(Optional.empty());
        }
    }

    CompletionStage<ZLinkStoreLocationResolvers.ActorRoute>
        resolveCommittedTargetActorRoute(String actorId) {
        return locations.resolveStoredActorRoute(actorId);
    }

    static boolean messageFollowTargetRouteMatches(
        ZLinkStoreLocationResolvers.ActorRoute route,
        ZLinkBackendActorRef targetActorRef,
        SpotTransportAddress targetAddress) {
        return route != null
            && targetActorRef != null
            && targetAddress != null
            && route.actorRef() != null
            && route.actorRef().actorId().equals(targetActorRef.actorId())
            && route.actorRef().objectGeneration() == targetActorRef.generation()
            && route.actorRef().nodeRid().equals(targetActorRef.nodeRid())
            && route.nodeRid().equals(targetActorRef.nodeRid())
            && route.targetNodeGeneration()
                == targetAddress.targetNodeGeneration();
    }

    private void traceRetainedMessageFollowSource(
        ZLinkActor actor,
        ZLinkBackendActorRef sourceActorRef) {
        // Keep the actor context as a Message Follow proxy while the old native
        // actor reference can still receive packets. EntrySpot dispatch uses its
        // rebound target reference to relay those packets to the new owner.
        traceActorTransferMarker(
            "message_follow_registered",
            actor.context().actorId(),
            Long.toUnsignedString(sourceActorRef.generation()));
    }

    int messageFollowSourceCount() {
        return handoff.messageFollowSourceCount();
    }

    public Optional<ZLinkBackendActorRef> messageFollowTargetActorRef(
        ZLinkActor actor) {
        Objects.requireNonNull(actor, "actor");
        return handoff.messageFollowSource(actor.context().actorId())
            .map(ZLinkActorTransferHandoff.MessageFollowSource::targetActorRef);
    }

    private void removeMessageFollowSource(
        ZLinkActorTransferHandoff.MessageFollowSource source) {
        cleanupMessageFollowNativeSource(source.sourceActorRef());
    }

    private void cleanupMessageFollowNativeSource(ZLinkBackendActorRef sourceActorRef) {
        spotNode.destroyActor(sourceActorRef, defaultRequestTimeout)
            .whenComplete((ignored, error) -> {
                if (error != null && !ZLinkActorSubmitFaults.requestNotFound(error)) {
                    ZLinkActorRetryScheduler.scheduleRoute(
                        () -> cleanupMessageFollowNativeSource(sourceActorRef));
                }
            });
    }

    public void traceActorTransferMarker(String marker, String actorId, String correlationId) {
        if (STREAM_TRACE) {
            LOGGER.warning(
                "[zlink-java-stream-trace] actor-transfer marker=" + marker
                    + " actor=" + actorId
                    + " correlation=" + correlationId);
        }
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow == null
            ? null : flow.begin(ZLinkMessageFlowOutcome.DISPATCHED);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(new ZLinkMessageFlowEvent(
            ZLinkMessageFlowOutcome.DISPATCHED,
            ZLinkDispatchErrorSurface.SPOT_ACTOR,
            ZLinkDispatchMessageKind.ACTOR_SEND,
            marker,
            null,
            null,
            correlationId,
            null,
            null,
            actorId,
            null));
    }

    public void failRemoteMove(ZLinkActor actor, Throwable error) {
        failTransferBacklog(actor, error);
        requireContext(actor).failMove(error);
    }

    private void failTransferBacklog(ZLinkActor actor, Throwable error) {
        handoff.fail(actor.context().actorId(), error);
    }

    public boolean isMoving(ZLinkActor actor) {
        return requireContext(actor).moving();
    }

    public CompletionStage<Void> awaitMoveCompletion(ZLinkActor actor) {
        return requireContext(actor).moveCompletion();
    }

    public CompletionStage<Void> commitJoinedLocation(
        ZLinkActor actor,
        String spotId) {
        return commitJoinedLocation(actor, requireContext(actor).actorRef(), spotId);
    }

    public CompletionStage<Void> commitJoinedLocation(
        ZLinkActor actor,
        ZLinkBackendActorRef targetActorRef,
        String spotId) {
        Objects.requireNonNull(targetActorRef, "targetActorRef");
        if (!actorRegistry.clearPendingTransfer(actor.context().actorId())) {
            return locations.setActorRef(
                    actorRegistry.actorType(actor.context().actorId()),
                    actor.context().actorId(),
                    toPublicActorRef(targetActorRef, meshName))
                .thenCompose(ignored -> renewActorJoinedLocation(actor, spotId));
        }
        String actorType = actorRegistry.actorType(actor.context().actorId());
        return locations.claimActor(
                actorType,
                actor.context().actorId(),
                spotNode.routingId(),
                ZLinkLocationWriteIntent.TAKEOVER,
                () -> deactivateActorOnOwnershipLoss(actor.context().actorId()))
            .thenCompose(ignored -> locations.setActorRef(
                actorType,
                actor.context().actorId(),
                toPublicActorRef(targetActorRef, meshName)))
            .thenCompose(ignored -> renewActorJoinedLocation(actor, spotId));
    }

    public CompletionStage<Void> commitEntryLocation(
        ZLinkActor actor,
        RoutingId entryNodeRid) {
        DefaultActorContext context = requireContext(actor);
        context.setEntrySpotNodeRid(entryNodeRid);
        context.markMovedToEntrySpot(
            context.actorRef(),
            new EntrySpotTarget(entryNodeRid, context.entrySpotId()));
        String actorType = actorRegistry.actorType(actor.context().actorId());
        CompletionStage<Void> ownership = actorRegistry.clearPendingTransfer(actor.context().actorId())
            ? locations.claimActor(
                actorType,
                actor.context().actorId(),
                spotNode.routingId(),
                ZLinkLocationWriteIntent.TAKEOVER,
                () -> deactivateActorOnOwnershipLoss(actor.context().actorId()))
            : CompletableFuture.completedFuture(null);
        return ownership
            .thenCompose(ignored -> locations.setActorRef(
                actorType, actor.context().actorId(), publicRefFor(actor)))
            .thenCompose(ignored -> renewActorMovedToEntrySpotLocation(actor, entryNodeRid));
    }

    public void markJoinedEntrySpot(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        RoutingId entryNodeRid) {
        DefaultActorContext context = requireContext(actor);
        context.setEntrySpotNodeRid(entryNodeRid);
        context.markMovedToEntrySpot(
            actorRef,
            new EntrySpotTarget(entryNodeRid, context.entrySpotId()));
    }

    private DefaultActorContext requireContext(ZLinkActor actor) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        return context;
    }

    static ActorRef toPublicActorRef(
        ZLinkBackendActorRef actorRef,
        String meshName) {
        return new ActorRef(
            actorRef.actorId(),
            actorRef.generation(),
            meshName,
            actorRef.nodeRid());
    }

    private CompletionStage<Void> renewActorJoinedLocation(
        ZLinkActor actor,
        String spotId) {
        return locations.actorJoinedSpot(actor, spotId);
    }

    private CompletionStage<Void> renewActorLeftLocation(ZLinkActor actor) {
        return locations.actorLeftSpot(actor);
    }

    private CompletionStage<Void> renewActorMovedToEntrySpotLocation(
        ZLinkActor actor,
        RoutingId nodeRid) {
        return locations.actorMovedToEntrySpot(actor, nodeRid);
    }

    private void removeActorSessionRouteForContext(DefaultActorContext context) {
        locations.removeSessionRoute(context.boundSessionSourceSessionRid());
    }

    CompletionStage<Void> deactivateActorOnOwnershipLoss(String actorId) {
        return discardLocalActor(actorId, true);
    }

    private CompletionStage<Void> discardLocalActor(
        String actorId,
        boolean releaseLocation) {
        ZLinkActor actor;
        DefaultActorContext context;
        String actorType;
        synchronized (this) {
            actor = actorRegistry.actor(actorId);
            context = actor == null ? null : actorRegistry.context(actor);
            actorType = actorRegistry.actorType(actorId);
        }
        if (actor == null || context == null || context.actorRef() == null) {
            return CompletableFuture.completedFuture(null);
        }
        return dispatches.beginTeardown(actorId, () -> {
            CompletionStage<Void> discarded =
                context.disconnectBoundSessionForDestroy()
                .exceptionally(error -> null)
                .thenCompose(ignored -> spotNode.destroyActor(
                    context.actorRef(),
                    defaultRequestTimeout)
                    .exceptionally(error -> null));
            if (releaseLocation) {
                discarded = discarded.thenCompose(
                    ignored -> locations.releaseActor(actorType, actorId)
                        .exceptionally(error -> null));
            }
            return discarded.thenRun(() -> {
                synchronized (this) {
                    actorRegistry.remove(actorId, actor);
                }
                removeActorSessionRouteForContext(context);
                context.clearAfterDestroy();
            });
        });
    }

    @Override
    public CompletionStage<Optional<ActorRef>> find(String actorId) {
        rejectAfterRelocationReady("Actor find");
        requireActorId(actorId);
        ZLinkActor local = actorRegistry.actor(actorId);
        if (local != null
            && !actorRegistry.isPendingTransfer(actorId)
            && !requireContext(local).moving()) {
            return CompletableFuture.completedFuture(Optional.of(publicRefFor(local)));
        }
        if (local != null && requireContext(local).moving()
            || actorRegistry.isPendingTransfer(actorId)) {
            return locations.findStoredActorRef(actorId);
        }
        try {
            ZLinkBackendActorRef nativeActor = spotNode.actorLookup(actorId);
            if (nativeActor != null) {
                return CompletableFuture.completedFuture(Optional.of(
                    toPublicActorRef(nativeActor, meshName)));
            }
        } catch (ZlinkConfigException ex) {
            if (ex.getResult() != ConfigResult.NOT_FOUND) {
                throw ex;
            }
        }
        return locations.findStoredActorRef(actorId);
    }

    @Override
    public CompletionStage<Optional<SpotRef>> findSpot(
        String actorId) {
        rejectAfterRelocationReady("Actor findSpot");
        requireActorId(actorId);
        return locations.findStoredSpotRef(actorId);
    }

    @Override
    public CompletionStage<Boolean> destroy(ActorRef actor) {
        rejectAfterRelocationReady("Actor destroy");
        if (actor == null) {
            throw new ZLinkConfigurationException("actor is required");
        }

        ZLinkActor current;
        DefaultActorContext context;
        synchronized (this) {
            current = actorRegistry.actor(actor.actorId());
            if (current == null) {
                return CompletableFuture.completedFuture(false);
            }
            context = actorRegistry.context(current);
            if (context == null || context.actorRef() == null) {
                return CompletableFuture.completedFuture(false);
            }

            ZLinkBackendActorRef currentRef = context.actorRef();
            if (!currentRef.actorId().equals(actor.actorId())
                || !currentRef.nodeRid().equals(actor.nodeRid())
                || currentRef.generation() != actor.objectGeneration()
                || !meshName.equals(actor.meshName())) {
                return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "actor ref is not current for this runtime: " + actor.actorId()));
            }
            if (context.moving() || actorRegistry.isPendingTransfer(actor.actorId())) {
                return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "actor is sealed for relocation: " + actor.actorId()));
            }
            if (context.currentSpot() != null) {
                return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    "actor must leave its current Spot before destroy: "
                        + actor.actorId()));
            }
        }

        return destroyFromEntrySpot(actor.nodeRid(), current)
            .thenApply(ignored -> true);
    }

    CompletionStage<Optional<ActorRef>> findCommittedRemoteActor(
        String actorId,
        RoutingId targetNodeRid,
        long objectGeneration) {
        return locations.findStoredActorRefExact(actorId)
            .thenApply(found -> found.filter(actor ->
                actor.nodeRid().equals(targetNodeRid)
                    && actor.objectGeneration() == objectGeneration));
    }

    CompletionStage<Void> prepareRemoteSessionBinding(
        ZLinkBackendActorRef actor) {
        return locations.resolveStoredActorRoute(actor.actorId())
            .thenAccept(route -> {
                if (route == null
                    || route.actorRef() == null
                    || !route.actorRef().nodeRid().equals(actor.nodeRid())
                    || route.actorRef().objectGeneration()
                        != actor.generation()
                    || route.authorityOwnerGeneration() <= 0) {
                    throw new IllegalStateException(
                        "Actor authority changed before Session binding");
                }
                spotNode.rememberActorAuthority(
                    actor,
                    route.authorityOwnerGeneration(),
                    route.ownerLeaseGeneration());
            });
    }

    @Override
    public CompletionStage<ActorRef> ensure(
        String actorId,
        ZLinkMessage createRequest) {
        requireActorId(actorId);
        if (createRequest == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "createRequest is required"));
        }
        CompletionStage<Optional<ActorRef>> existing = find(actorId);
        return existing.thenCompose(found -> {
            if (found.isPresent()) {
                return CompletableFuture.completedFuture(found.get());
            }
            return createLocalActor(actorId, resolveSingleActorType(), createRequest, false)
                .thenApply(this::publicRefFor)
                .exceptionallyCompose(error -> {
                    Throwable cause = unwrap(error);
                    if (cause instanceof ZLinkFrameworkException frameworkError
                        && frameworkError.kind() == ZLinkFrameworkErrorKind.INTERNAL_FAILURE) {
                        return find(actorId).thenCompose(raced -> raced
                            .<CompletionStage<ActorRef>>map(CompletableFuture::completedFuture)
                            .orElseGet(() -> CompletableFuture.failedFuture(
                                new ZLinkFrameworkException(
                                    ZLinkFrameworkErrorKind.REJECTED,
                                    frameworkError.getMessage(),
                                    frameworkError))));
                    }
                    return CompletableFuture.failedFuture(cause);
                });
        });
    }

    private static Throwable unwrap(Throwable error) {
        if (error instanceof CompletionException && error.getCause() != null) {
            return error.getCause();
        }
        return error;
    }

    private static void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] " + message);
        }
    }

    private String resolveSingleActorType() {
        if (factories.size() == 1) {
            return factories.keySet().iterator().next();
        }
        String message = factories.isEmpty()
            ? "actor directory requires one registered actor factory"
            : "actor directory cannot choose an actor type when multiple actor factories are registered";
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            message);
    }

    @Override
    public ZLinkActorGetOrCreateCall getOrCreate(
        String actorId,
        String actorType) {
        rejectAfterRelocationReady("Actor getOrCreate");
        return new ActorGetOrCreateCall(actorId, actorType);
    }

    private static void rejectAfterRelocationReady(String operation) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectAfterRelocationReady(
                operation);
    }

    private abstract class ActorCreationCall {
        private final String actorId;
        private final String actorType;
        private final boolean getOrCreate;
        private final AtomicBoolean submitted =
            new AtomicBoolean();
        private String selectedMesh;
        private ZLinkMessage request = ZLinkMessage.empty();
        private boolean requestSet;
        private Duration timeout = defaultRequestTimeout;
        private boolean timeoutSet;

        ActorCreationCall(
            String actorId,
            String actorType,
            boolean getOrCreate) {
            this.actorId = actorId;
            this.actorType = actorType;
            this.getOrCreate = getOrCreate;
        }

        final void setMesh(String value) {
            if (selectedMesh != null) {
                throw new IllegalStateException("Mesh was already set");
            }
            if (value == null || value.isBlank()) {
                throw new IllegalArgumentException("meshName is required");
            }
            selectedMesh = value;
        }

        final void setRequest(ZLinkMessage value) {
            if (requestSet) {
                throw new IllegalStateException("request was already set");
            }
            request = Objects.requireNonNull(value, "request");
            requestSet = true;
        }

        final void setTimeout(Duration value) {
            if (timeoutSet) {
                throw new IllegalStateException("timeout was already set");
            }
            if (value == null || value.isZero() || value.isNegative()) {
                throw new IllegalArgumentException("timeout must be positive");
            }
            timeout = value;
            timeoutSet = true;
        }

        final CompletionStage<ZLinkActorCreateResult> submitOperation() {
            if (!submitted.compareAndSet(false, true)) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Actor creation call was already submitted"));
            }
            return submitCreate(
                actorId,
                actorType,
                request,
                getOrCreate,
                selectedMesh,
                timeout);
        }

        final CompletionStage<ZLinkActorCreateResult> yieldOperation() {
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.requireYieldAllowed(
                    "Actor creation");
            return ZLinkAsyncSerialQueue
                .yieldCurrent(submitOperation());
        }
    }

    private final class ActorCreateCall extends ActorCreationCall
        implements ZLinkActorCreateCall {
        ActorCreateCall(String actorId, String actorType) {
            super(actorId, actorType, false);
        }

        @Override public ActorCreateCall inMesh(String value) {
            setMesh(value); return this;
        }
        @Override public ActorCreateCall request(Object value) {
            return request(ZLinkMessage.of(value));
        }
        @Override public ActorCreateCall request(ZLinkMessage value) {
            setRequest(value); return this;
        }
        @Override public ActorCreateCall timeout(Duration value) {
            setTimeout(value); return this;
        }
        @Override public CompletionStage<ZLinkActorCreateResult> submit() {
            return submitOperation();
        }
        @Override public CompletionStage<ZLinkActorCreateResult> yield() {
            return yieldOperation();
        }
    }

    private final class ActorGetOrCreateCall extends ActorCreationCall
        implements ZLinkActorGetOrCreateCall {
        ActorGetOrCreateCall(String actorId, String actorType) {
            super(actorId, actorType, true);
        }

        @Override public ActorGetOrCreateCall inMesh(String value) {
            setMesh(value); return this;
        }
        @Override public ActorGetOrCreateCall request(Object value) {
            return request(ZLinkMessage.of(value));
        }
        @Override public ActorGetOrCreateCall request(ZLinkMessage value) {
            setRequest(value); return this;
        }
        @Override public ActorGetOrCreateCall timeout(Duration value) {
            setTimeout(value); return this;
        }
        @Override public CompletionStage<ZLinkActorCreateResult> submit() {
            return submitOperation();
        }
        @Override public CompletionStage<ZLinkActorCreateResult> yield() {
            return yieldOperation();
        }
    }

    private CompletionStage<ZLinkActorCreateResult> getOrCreateActorOperation(
        String actorId,
        String actorType,
        ZLinkMessage createRequest) {
        streamTrace(STREAM_TRACE ? "actor-create get-or-create-start actor=" + actorId
            + " type=" + actorType : null);
        ZLinkActor actor = actorRegistry.actor(actorId);
        if (actor != null) {
            streamTrace(STREAM_TRACE ? "actor-create get-or-create-existing actor=" + actorId : null);
            return CompletableFuture.completedFuture(
                new ZLinkActorCreateResult.Existing(publicRefFor(actor)));
        }
        AtomicReference<ZLinkActorCreateResponse> response =
            new AtomicReference<>();
        return createLocalActor(actorId, actorType, createRequest, false, response::set)
            .thenApply(created -> {
                ZLinkActorCreateResponse admission = response.get();
                return (ZLinkActorCreateResult) new ZLinkActorCreateResult.Created(
                    publicRefFor(created),
                    admission == null ? null : admission.reply());
            })
            .exceptionally(error -> {
                Throwable cause = unwrap(error);
                if (cause instanceof ActorCreateRejected rejected) {
                    return new ZLinkActorCreateResult.Rejected(rejected.reply());
                }
                throw new CompletionException(cause);
            });
    }

    private <T> CompletionStage<T> serializeActorCreation(
        String actorId,
        Supplier<CompletionStage<T>> operation) {
        CompletableFuture<Void> previous;
        CompletableFuture<Void> reservation = new CompletableFuture<>();
        synchronized (actorCreationGate) {
            previous = actorCreationTails.put(actorId, reservation);
        }

        CompletionStage<T> result = (previous == null
            ? CompletableFuture.completedFuture(null)
            : previous.handle((ignored, error) -> null))
            .thenCompose(ignored -> {
                try {
                    return operation.get();
                } catch (RuntimeException error) {
                    return CompletableFuture.failedFuture(error);
                }
            });
        result.whenComplete((ignored, error) -> {
            reservation.complete(null);
            synchronized (actorCreationGate) {
                if (actorCreationTails.get(actorId) == reservation) {
                    actorCreationTails.remove(actorId);
                }
            }
        });
        return result;
    }

    /**
     * Resolves the registered factory type for {@code actorType} without
     * reserving the Actor's registry slot. Used to validate a cross-node
     * Actor Join target admission up front (spec 15 §4.2 "factory 실행
     * 준비") before the payload and application state exist to actually
     * instantiate the Actor.
     */
    public Class<? extends ZLinkActorFactory> resolveActorFactoryType(
        String actorType) {
        return requireFactory(actorType);
    }

    private Class<? extends ZLinkActorFactory> requireFactory(String actorType) {
        if (actorType == null || actorType.isBlank()) {
            throw new ZLinkConfigurationException("actorType is required");
        }
        Class<? extends ZLinkActorFactory> factory = factories.get(actorType);
        if (factory == null) {
            throw new ZLinkConfigurationException("actor type is not registered: " + actorType);
        }
        return factory;
    }

    private static void requireActorId(String actorId) {
        if (actorId == null || actorId.isBlank()) {
            throw new ZLinkConfigurationException("actorId is required");
        }
    }

    private ZLinkActorFactory createFactory(
        Class<? extends ZLinkActorFactory> factoryType) {
        try {
            return (ZLinkActorFactory) handlerFactory.create(factoryType);
        } catch (RuntimeException ex) {
            throw new ZLinkConfigurationException(
                "failed to create actor factory: " + factoryType.getName(),
                ex);
        }
    }

    private Message messageFromRequest(ZLinkMessage request) {
        if (request == null) {
            throw new ZLinkConfigurationException("request is required");
        }
        return Message.from(request.toEncodedPayload(serializer).bytes());
    }

    ZLinkBackendActorRef refFor(ZLinkActor actor) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        return context.actorRef();
    }

    public ZLinkBackendActorRef currentRef(ZLinkActor actor) {
        return refFor(actor);
    }

    public ZLinkSpot<?> currentSpot(ZLinkActor actor) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null || context.joinedSpotId() == null) {
            return null;
        }
        return context.currentSpot();
    }

    private ActorRef publicRefFor(ZLinkActor actor) {
        ZLinkBackendActorRef actorRef = refFor(actor);
        return new ActorRef(
            actorRef.actorId(),
            actorRef.generation(),
            meshName,
            actorRef.nodeRid());
    }

    private DefaultActorContext contextFor(ActorRef actor) {
        if (actor == null) {
            throw new ZLinkConfigurationException("actor is required");
        }
        ZLinkActor current = actorRegistry.actor(actor.actorId());
        if (current == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.actorId());
        }
        DefaultActorContext context = actorRegistry.context(current);
        if (context == null || context.actorRef() == null) {
            throw new ZLinkConfigurationException(
                "actor does not have a native Actor ref: " + actor.actorId());
        }
        ZLinkBackendActorRef currentRef = context.actorRef();
        if (!currentRef.actorId().equals(actor.actorId())
            || !currentRef.nodeRid().equals(actor.nodeRid())
            || currentRef.generation() != actor.objectGeneration()
            || !meshName.equals(actor.meshName())) {
            throw new ZLinkConfigurationException(
                "actor ref is not current for this runtime: " + actor.actorId());
        }
        return context;
    }

    public String actorTypeFor(ZLinkActor actor) {
        String actorType = actorRegistry.actorType(actor.context().actorId());
        if (actorType == null || actorType.isBlank()) {
            throw new ZLinkConfigurationException(
                "actor type is not available: " + actor.context().actorId());
        }
        return actorType;
    }

    long bindSession(ZLinkActor actor, ZLinkBoundSession boundSession) {
        return bindSession(actor, boundSession, null, null);
    }

    long bindSession(
        ZLinkActor actor,
        ZLinkBoundSession boundSession,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        return bindSession(
            actor,
            boundSession,
            sourceNodeRid,
            sourceSessionRid,
            0,
            0);
    }

    long bindSession(
        ZLinkActor actor,
        ZLinkBoundSession boundSession,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long bindingGeneration,
        long lastAcceptedSessionSequence) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        long bindingToken = context.bindSession(
            boundSession,
            sourceNodeRid,
            sourceSessionRid,
            bindingGeneration,
            lastAcceptedSessionSequence);
        locations.bindSessionRoute(sourceSessionRid, actor.context().actorId(), sourceNodeRid);
        return bindingToken;
    }

    public byte[] encodeLocalSessionActorAccepted(
        ZLinkActor actor,
        ZLinkStreamHeader header,
        Message payload) {
        return encodeLocalSessionActorAccepted(actor, header, payload, 0);
    }

    public byte[] encodeLocalSessionActorAccepted(
        ZLinkActor actor,
        ZLinkStreamHeader header,
        Message payload,
        long acceptedSessionSequence) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            return new byte[0];
        }
        ZLinkActorContextState.BoundSessionSource source =
            acceptedSessionSequence > 0
                ? context.nextBoundSessionSource(acceptedSessionSequence)
                : context.nextBoundSessionSource();
        if (source == null) {
            return new byte[0];
        }
        return spotNode.encodeLocalSessionActorAccepted(
            context.actorRef(),
            source.sourceNodeRid(),
            source.sourceSessionRid(),
            source.bindingGeneration(),
            source.sessionSequence(),
            header.requestSequence().orElse(0L),
            header.packetName(),
            header.metadata(),
            payload.toByteArray());
    }

    public boolean clearSessionBinding(ZLinkActor actor, long bindingToken) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        RoutingId routeSessionRid = context.boundSessionSourceSessionRid(bindingToken);
        boolean cleared = context.clearBoundSession(bindingToken);
        if (cleared) {
            locations.removeSessionRoute(routeSessionRid);
        }
        return cleared;
    }

    public CompletionStage<Message> handleEntrySpotRouteJoin(
        RoutingId sourceRoutingId,
        Message payload) {
        ZLinkActorEntrySpotRoutePackets.JoinRequest request =
            ZLinkActorEntrySpotRoutePackets.decodeJoinRequest(payload);
        return createLocalActor(
                request.actorId(),
                request.actorType(),
                ZLinkMessage.empty(),
                false)
            .thenApply(actor -> {
                ZLinkBackendActorRef actorRef = refFor(actor);
                return ZLinkActorEntrySpotRoutePackets.encodeJoinReply(
                    actor.context().actorId(),
                    actorRegistry.actorTypeOrDefault(actor.context().actorId(), request.actorType()),
                    actorRef.nodeRid(),
                    actorRef.generation());
            });
    }

    public Optional<ZLinkActor> localActor(String actorId) {
        return Optional.ofNullable(actorRegistry.actor(actorId));
    }

    public CompletionStage<ZLinkActor> getOrCreateManagedActor(
        String actorId,
        String actorType) {
        return getOrCreateManagedActor(actorId, actorType, true);
    }

    public CompletionStage<ZLinkActor> getOrCreateManagedActorWithoutLocationClaim(
        String actorId,
        String actorType) {
        ZLinkActor existing = actorRegistry.actor(actorId);
        if (existing != null) {
            return CompletableFuture.completedFuture(existing);
        }
        return createLocalActor(
            actorId,
            actorType,
            ZLinkMessage.empty(),
            false,
            false,
            null);
    }

    private CompletionStage<ZLinkActor> getOrCreateManagedActor(
        String actorId,
        String actorType,
        boolean notifyCreated) {
        ZLinkActor existing = actorRegistry.actor(actorId);
        if (existing != null) {
            return CompletableFuture.completedFuture(existing);
        }
        return createLocalActor(actorId, actorType, ZLinkMessage.empty(), false, notifyCreated);
    }

    public boolean hasBoundSession(ZLinkActor actor) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        return context.hasBoundSession();
    }

    public Optional<BoundSessionRouteSnapshot> boundSessionRoute(
        ZLinkActor actor) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: "
                    + actor.context().actorId());
        }
        ZLinkActorContextState.BoundSessionSource source =
            context.boundSessionSourceSnapshot();
        if (source != null) {
            return Optional.of(new BoundSessionRouteSnapshot(
                source.sourceNodeRid(),
                source.sourceSessionRid(),
                source.bindingGeneration(),
                source.sessionSequence()));
        }
        return spotNode.boundSessionRoute(currentRef(actor))
            .map(route -> new BoundSessionRouteSnapshot(
                route.sessionOwnerNodeRid(),
                route.sessionRid(),
                route.bindingGeneration(),
                0));
    }

    /** Installs the command 42 sender for exact bound-Session relocation. */
    public void setSessionRelocationSealer(
        ZLinkSessionRelocationPeerClient sealer) {
        this.sessionRelocationSealer = sealer;
    }

    CompletionStage<DirectJoinSessionRouteCommand> directJoinSessionRouteCommand(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        RoutingId targetNodeRid,
        UUID relocationId) {
        BoundSessionRouteSnapshot session = boundSessionRoute(actor).orElse(null);
        if (session == null) {
            return CompletableFuture.completedFuture(
                DirectJoinSessionRouteCommand.empty());
        }
        return locations.directJoinSessionFence(
                actor.context().actorId(),
                session.sessionOwnerNodeRid(),
                targetNodeRid)
            .thenCompose(authority -> {
                DirectJoinSessionRoute route =
                    new DirectJoinSessionRoute(authority, session);
                return sealBoundSessionRoute(actorRef, relocationId, route)
                    .thenApply(sealed -> new DirectJoinSessionRouteCommand(
                        directJoinSessionRouteCommand(
                            actorRef,
                            targetNodeRid,
                            relocationId,
                            route),
                        sealed.seal(),
                        sealed.peer()));
            });
    }

    /**
     * Runs the spec 20 §5 step 1 seal against the bound Session owner:
     * command 42 carries the exact source fence and command 43 echoes it
     * field-for-field. The Session owner keeps message ordering state locally;
     * command 44 does not carry a duplicated high-water value.
     */
    private CompletionStage<SealedDirectJoinSessionRoute> sealBoundSessionRoute(
        ZLinkBackendActorRef actorRef,
        UUID relocationId,
        DirectJoinSessionRoute route) {
        var session = route.session();
        ZLinkSessionRelocationPeerClient sealer = sessionRelocationSealer;
        if (sealer == null) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "StateIncompatible: bound-Session relocation peer does "
                        + "not support command 42/43 sealing"));
        }
        var authority = route.authority();
        var source = authority.sourceActorOwner();
        var sessionOwner = authority.sessionOwner();
        var seal = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.SessionRelocationSeal(
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RelocationIdentity(
                        relocationId.getMostSignificantBits(),
                        relocationId.getLeastSignificantBits()),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                        authority.sourceAuthorityOwnerId(),
                        authority.sourceAuthorityOwnerLeaseGeneration(),
                        source.rid(),
                        source.lifecycleGeneration(),
                        authority.sourceAuthorityStoreVersion()),
                systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.ActorRouteFence(
                        new ZLinkBackendActorRef(
                            source.rid(),
                            actorRef.actorId(),
                            actorRef.generation()),
                        source.lifecycleGeneration(),
                        authority.sourceAuthorityOwnerGeneration(),
                        authority.sourceAuthorityOwnerLeaseGeneration()),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        session.sessionOwnerNodeRid(),
                        sessionOwner.lifecycleGeneration(),
                        sessionOwner.ownerId(),
                        sessionOwner.leaseGeneration(),
                        session.sessionRid(),
                        session.bindingGeneration()));
        //  The command 43 ACK completes on the mesh transport dispatch thread.
        //  Everything the Join does after this point - begin remote move,
        //  transfer out, manifest prepare - must not run there or it stalls
        //  every other inbound record on this node, so the continuation is
        //  handed to the default async pool before the caller chains on it.
        return sealer.sealRouteUntilAck(seal, sessionRelocationSealTimeout)
            .thenApplyAsync(sealed -> new SealedDirectJoinSessionRoute(
                seal, sealer))
            .exceptionallyCompose(failure -> {
                //  Preserve the typed seal failure (deadline -> DeadlineExceeded,
                //  malformed ACK -> ProtocolError, route -> Unavailable, shutdown
                //  -> ShuttingDown) instead of collapsing every command 42/43
                //  seal failure to NotConfigured.
                Throwable cause = failure instanceof CompletionException
                    && failure.getCause() != null ? failure.getCause() : failure;
                ZLinkFrameworkException typed = cause instanceof ZLinkFrameworkException framework
                    ? framework
                    : new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                        "RelocationFailed: command 42/43 did not seal the "
                            + "bound Session for " + actorRef.actorId(), cause);
                return CompletableFuture.failedFuture(typed);
            });
    }

    CompletionStage<Void> abortDirectJoinSessionRoute(
        DirectJoinSessionRouteCommand context) {
        Objects.requireNonNull(context, "context");
        if (context.seal() == null) {
            return CompletableFuture.completedFuture(null);
        }
        if (draining) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "Actor runtime is draining before direct-Join Session abort"));
        }
        var key = context.seal().relocation();
        DirectJoinSessionAbortFlight candidate =
            new DirectJoinSessionAbortFlight(
                context,
                directJoinSessionAbortCommand(context),
                new CompletableFuture<>());
        DirectJoinSessionAbortFlight existing =
            directJoinSessionAborts.putIfAbsent(key, candidate);
        if (existing != null) {
            if (!sameDirectJoinSessionRouteContext(
                    existing.context(), context)) {
                return CompletableFuture.failedFuture(
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                        "direct-Join Session abort conflicts with the retained "
                            + "relocation context"));
            }
            return existing.completion();
        }
        if (draining && directJoinSessionAborts.remove(key, candidate)) {
            candidate.completion().completeExceptionally(
                new ZLinkConfigurationException(
                    "Actor runtime began drain before direct-Join Session abort"));
            return candidate.completion();
        }
        attemptDirectJoinSessionAbort(candidate);
        return candidate.completion();
    }

    private void attemptDirectJoinSessionAbort(
        DirectJoinSessionAbortFlight flight) {
        if (draining
            || flight.completion().isDone()
            || directJoinSessionAborts.get(
                flight.context().seal().relocation()) != flight) {
            return;
        }
        flight.context().peer().sendRoute(flight.abort())
            .whenComplete((ack, failure) -> {
                if (failure != null) {
                    scheduleDirectJoinSessionAbortRetry(flight);
                    return;
                }
                directJoinSessionAborts.remove(
                    flight.context().seal().relocation(), flight);
                flight.completion().complete(null);
            });
    }

    private void scheduleDirectJoinSessionAbortRetry(
        DirectJoinSessionAbortFlight flight) {
        if (draining
            || flight.completion().isDone()
            || directJoinSessionAborts.get(
                flight.context().seal().relocation()) != flight) {
            return;
        }
        ZLinkActorRetryScheduler.scheduleRouteAfter(
            () -> attemptDirectJoinSessionAbort(flight),
            Duration.ofSeconds(5));
    }

    private static boolean sameDirectJoinSessionRouteContext(
        DirectJoinSessionRouteCommand left,
        DirectJoinSessionRouteCommand right) {
        return left.seal().equals(right.seal())
            && Arrays.equals(left.command44(), right.command44())
            && left.peer() == right.peer();
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute
        directJoinSessionAbortCommand(
            DirectJoinSessionRouteCommand context) {
        if (context.seal() == null) {
            throw new ZLinkConfigurationException(
                "direct-Join Session abort has no command 42 context");
        }
        ZLinkServiceM6BWireCodec.SessionRelocationSeal command42 =
            context.seal();
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            command42.relocation(),
            command42.coordinator(),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorIdentity(
                command42.actor().actor().actorId(),
                command42.actor().actor().generation()),
            command42.session(),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            0,
            command42.actor().authorityOwnerGeneration(),
            null,
            0);
    }

    private byte[] directJoinSessionRouteCommand(
        ZLinkBackendActorRef actorRef,
        RoutingId targetNodeRid,
        UUID relocationId,
        DirectJoinSessionRoute route) {
        var authority = route.authority();
        var session = route.session();
        var source = authority.sourceActorOwner();
        var sessionOwner = authority.sessionOwner();
        var target = authority.targetActorOwner();
        if (authority.sourceAuthorityOwnerGeneration() == Long.MAX_VALUE) {
            throw new ZLinkConfigurationException(
                "bound Session authority generation is exhausted");
        }
        var command =
                    new systems.zlink.framework.runtime.internal.service
                        .ZLinkServiceM6BWireCodec.SessionRelocationRoute(
                            new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceM6BWireCodec.RelocationIdentity(
                                    relocationId.getMostSignificantBits(),
                                    relocationId.getLeastSignificantBits()),
                            new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceM6BWireCodec
                                .RelocationCoordinatorFence(
                                    authority.sourceAuthorityOwnerId(),
                                    authority
                                        .sourceAuthorityOwnerLeaseGeneration(),
                                    source.rid(),
                                    source.lifecycleGeneration(),
                                    authority.sourceAuthorityStoreVersion()),
                            systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
                            new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceM6BWireCodec.ActorIdentity(
                                    actorRef.actorId(),
                                    actorRef.generation()),
                            new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceM6BWireCodec.SessionOwnerFence(
                                    session.sessionOwnerNodeRid(),
                                    sessionOwner.lifecycleGeneration(),
                                    sessionOwner.ownerId(),
                                    sessionOwner.leaseGeneration(),
                                    session.sessionRid(),
                                    session.bindingGeneration()),
                            systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceM6BWireCodec
                                .SessionRelocationRouteAction.COMMIT,
                            authority.sourceAuthorityOwnerGeneration(),
                            authority.sourceAuthorityOwnerGeneration() + 1,
                            targetNodeRid,
                            target.lifecycleGeneration());
        return new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec()
            .encodeSessionRelocationRoute(command);
    }

    record DirectJoinSessionRoute(
        ZLinkStoreLocationResolvers
            .DirectJoinSessionFence authority,
        BoundSessionRouteSnapshot session) {
    }

    record DirectJoinSessionRouteCommand(
        byte[] command44,
        ZLinkServiceM6BWireCodec.SessionRelocationSeal seal,
        ZLinkSessionRelocationPeerClient peer) {
        DirectJoinSessionRouteCommand {
            command44 = Objects.requireNonNull(command44, "command44").clone();
            if ((seal == null && (command44.length != 0
                    || peer != null))
                || (seal != null && (command44.length == 0 || peer == null))) {
                throw new IllegalArgumentException(
                    "direct-Join Session relocation context is invalid");
            }
        }

        static DirectJoinSessionRouteCommand empty() {
            return new DirectJoinSessionRouteCommand(
                new byte[0], null, null);
        }

        @Override public byte[] command44() {
            return command44.clone();
        }
    }

    private record DirectJoinSessionAbortFlight(
        DirectJoinSessionRouteCommand context,
        ZLinkServiceM6BWireCodec.SessionRelocationRoute abort,
        CompletableFuture<Void> completion) {
    }

    private record SealedDirectJoinSessionRoute(
        ZLinkServiceM6BWireCodec.SessionRelocationSeal seal,
        ZLinkSessionRelocationPeerClient peer) {
        private SealedDirectJoinSessionRoute {
            Objects.requireNonNull(seal, "seal");
            Objects.requireNonNull(peer, "peer");
        }
    }

    public record BoundSessionRouteSnapshot(
        RoutingId sessionOwnerNodeRid,
        RoutingId sessionRid,
        long bindingGeneration,
        long lastAcceptedSessionSequence) {
        public BoundSessionRouteSnapshot {
            Objects.requireNonNull(
                sessionOwnerNodeRid, "sessionOwnerNodeRid");
            Objects.requireNonNull(sessionRid, "sessionRid");
            if (bindingGeneration <= 0
                || lastAcceptedSessionSequence < 0) {
                throw new IllegalArgumentException(
                    "bound Session route generations are invalid");
            }
        }
    }

    public boolean hasBoundSession(
        ZLinkActor actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        return context.hasBoundSession(sourceNodeRid, sourceSessionRid);
    }

    public CompletionStage<Boolean> sendBoundSessionFrame(
        ZLinkActor actor,
        byte[] frameBytes) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        return context.sendBoundSessionFrame(frameBytes);
    }

    public Optional<String> spotId(ZLinkActor actor) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        return Optional.ofNullable(context.joinedSpotId());
    }

    public ZLinkBackendActorRef actorRef(ZLinkActor actor) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        return context.actorRef();
    }

    public boolean canRouteRemoteJoinedSpot(String spotId) {
        return spotId != null
            && spotResolver.apply(spotId) == null
            && remoteAddressResolver != null
            && (routedTransport != null || actorJoinTransferTransport != null);
    }

    public void setMessageFollowNoticeSender(
        MessageFollowNoticeSender sender) {
        messageFollowNoticeSender = sender;
    }

    /**
     * Relays one stale raw Actor record through the committed typed Actor
     * route. The caller transfers the received parts and admission lease only
     * when this method returns true.
     */
    public boolean relayMessageFollow(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        ZLinkServiceM6BWireCodec.ActorMessage header,
        byte[] acceptedJournalRecord,
        List<Message> parts,
        String contentType,
        Consumer<List<Message>> reply,
        Consumer<Throwable> failure,
        Runnable terminalRelease) {
        if (sourceNodeRid == null
            || sourceNodeGeneration <= 0
            || header == null
            || parts == null
            || parts.size() != 2) {
            return false;
        }
        ZLinkBackendActorRef staleActor = header.target().actor();
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute =
            messageFollowSourceRoute(header);
        ZLinkActorTransferHandoff.MessageFollowSource followSource =
            handoff.messageFollowSource(staleActor.actorId()).orElse(null);
        if (followSource == null
            || !followSource.sourceActorRef().equals(staleActor)) {
            return false;
        }
        if (!followSource.matchesSourceRoute(sourceRoute)
            || followSource.targetAddress() == null
            || followSource.targetActorRef().generation()
                != staleActor.generation()) {
            return rejectRetainedMessageFollowIngress(
                parts,
                failure,
                "Message Follow source fence is unavailable or stale",
                terminalRelease);
        }
        ZLinkStreamHeader streamHeader;
        try {
            streamHeader = ZLinkStreamHeaderCodec.decodeOrPlain(
                parts.get(0).toByteArray());
            if (streamHeader.requestSequence().isPresent() != header.request()
                || (header.request()
                    && !streamHeader.requestSequence().orElseThrow()
                        .equals(header.correlation()))) {
                return rejectRetainedMessageFollowIngress(
                    parts,
                    failure,
                    "Message Follow request fence is invalid",
                    terminalRelease);
            }
        } catch (RuntimeException invalidHeader) {
            return rejectRetainedMessageFollowIngress(
                parts,
                failure,
                "Message Follow stream header is invalid",
                terminalRelease);
        }

        Message payload;
        try {
            payload = parts.get(1);
        } catch (RuntimeException invalidPayload) {
            return rejectRetainedMessageFollowIngress(
                parts,
                failure,
                "Message Follow payload is invalid",
                terminalRelease);
        }
        parts.get(0).close();

        Consumer<Throwable> onFailure = failure == null
            ? ignored -> { }
            : failure;
        if (header.messageFollowHopCount() + 1
            > ZLinkServiceMessageFollowWireCodec.MAX_HOP_COUNT) {
            onFailure.accept(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "Message Follow hop limit exceeded"));
            payload.close();
            terminalRelease.run();
            return true;
        }
        MessageFollowNoticeSender noticeSender = messageFollowNoticeSender;
        Optional<ZLinkMessageFollowSuppressionRegistry.Claim> noticeClaim =
            noticeSender == null
                ? Optional.empty()
                : followSource.beginMessageFollowNotice(sourceRoute);
        CompletionStage<ZLinkServiceMessageFollowWireCodec.ActorRoute> targetRoute =
            CompletableFuture.completedFuture(followSource.targetRoute());
        targetRoute.thenCompose(route -> {
            ZLinkActorTransferHandoff.MessageFollowSource current =
                handoff.messageFollowSource(staleActor.actorId()).orElse(null);
            if (current != followSource) {
                return CompletableFuture.<MessageFollowRelayResult>failedFuture(
                    new ZLinkConfigurationException(
                        "Message Follow route was replaced while relaying"));
            }
            return handoff.followWithQueueSnapshot(
                    staleActor.actorId(),
                    staleActor.generation(),
                    payload.size(),
                    () -> dispatchRemoteJoinedActor(
                        followSource.targetActorRef(),
                        streamHeader,
                        payload,
                        followSource.targetAddress(),
                        acceptedJournalRecord))
                .thenApply(follow -> new MessageFollowRelayResult(
                    follow.value(),
                    route,
                    follow.queue()));
        }).whenComplete((result, relayFailure) -> {
            try {
                if (relayFailure != null) {
                    noticeClaim.ifPresent(followSource::abortMessageFollowNotice);
                    onFailure.accept(unwrapCompletionFailure(relayFailure));
                    return;
                }
                if (result.reply().isPresent()) {
                    Message replyMessage = result.reply().orElseThrow();
                    try {
                        if (reply != null) {
                            reply.accept(List.of(replyMessage));
                        } else {
                            replyMessage.close();
                        }
                    } catch (RuntimeException replyFailure) {
                        replyMessage.close();
                        onFailure.accept(replyFailure);
                    }
                }
                if (noticeClaim.isPresent()) {
                    sendMessageFollowNotice(
                        noticeSender,
                        sourceNodeRid,
                        header,
                        followSource,
                        sourceRoute,
                        result.route(),
                        result.queue(),
                        sourceNodeGeneration,
                        noticeClaim.orElseThrow());
                }
            } finally {
                payload.close();
                terminalRelease.run();
            }
        });
        return true;
    }

    private static boolean rejectRetainedMessageFollowIngress(
        List<Message> parts,
        Consumer<Throwable> failure,
        String message,
        Runnable terminalRelease) {
        try {
            if (failure != null) {
                try {
                    failure.accept(new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                        message));
                } catch (RuntimeException ignored) {
                    // The retained source still owns and rejects the packet.
                }
            }
        } finally {
            parts.forEach(Message::close);
            terminalRelease.run();
        }
        return true;
    }

    private void sendMessageFollowNotice(
        MessageFollowNoticeSender noticeSender,
        RoutingId sourceNodeRid,
        ZLinkServiceM6BWireCodec.ActorMessage staleHeader,
        ZLinkActorTransferHandoff.MessageFollowSource followSource,
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
        ZLinkActorTransferHandoff.MessageFollowQueueSnapshot queue,
        long sourceNodeGeneration,
        ZLinkMessageFollowSuppressionRegistry.Claim noticeClaim) {
        if (targetRoute == null
            || !targetRoute.actorId().equals(
                staleHeader.target().actor().actorId())
            || targetRoute.objectGeneration()
                != staleHeader.target().actor().generation()
            || !targetRoute.targetNodeRid().equals(
                followSource.targetActorRef().nodeRid())
            || targetRoute.targetNodeGeneration()
                != followSource.targetAddress().targetNodeGeneration()) {
            LOGGER.warning("Message Follow target route fence changed before notice "
                + "for actor " + staleHeader.target().actor().actorId());
            followSource.abortMessageFollowNotice(noticeClaim);
            return;
        }
        int hopCount = staleHeader.messageFollowHopCount() + 1;
        if (hopCount > ZLinkServiceMessageFollowWireCodec.MAX_HOP_COUNT) {
            followSource.abortMessageFollowNotice(noticeClaim);
            return;
        }
        ZLinkServiceMessageFollowWireCodec.Notice notice =
            messageFollowNotice(
                sourceRoute,
                targetRoute,
                hopCount,
                queue,
                staleHeader.operationHigh(),
                staleHeader.operationLow(),
                staleHeader.request() ? staleHeader.correlation() : 0L);
        try {
            CompletionStage<Void> sent = Objects.requireNonNull(
                noticeSender.send(sourceNodeRid, notice),
                "Message Follow notice sender returned null");
            sent.whenComplete((ignored, failure) -> {
                completeMessageFollowNotice(followSource, noticeClaim, failure);
                if (failure != null) {
                    LOGGER.warning("Message Follow notice failed for actor "
                        + staleHeader.target().actor().actorId()
                        + " from " + sourceNodeRid
                        + " sourceGeneration=" + sourceNodeGeneration
                        + ": " + unwrapCompletionFailure(failure));
                }
            });
        } catch (RuntimeException failure) {
            followSource.abortMessageFollowNotice(noticeClaim);
            LOGGER.warning("Message Follow notice submission failed for actor "
                + staleHeader.target().actor().actorId()
                + " from " + sourceNodeRid
                + ": " + failure);
        }
    }

    static ZLinkServiceMessageFollowWireCodec.Notice messageFollowNotice(
        ZLinkServiceMessageFollowWireCodec.ActorRoute sourceRoute,
        ZLinkServiceMessageFollowWireCodec.ActorRoute targetRoute,
        int hopCount,
        ZLinkActorTransferHandoff.MessageFollowQueueSnapshot queue,
        long operationHigh,
        long operationLow,
        long correlation) {
        final long uint32Max = 0xffff_ffffL;
        return new ZLinkServiceMessageFollowWireCodec.Notice(
            sourceRoute,
            targetRoute,
            hopCount,
            Math.min(queue.messages(), uint32Max),
            Math.min(queue.bytes(), uint32Max),
            operationHigh,
            operationLow,
            correlation);
    }

    static void completeMessageFollowNotice(
        ZLinkActorTransferHandoff.MessageFollowSource followSource,
        ZLinkMessageFollowSuppressionRegistry.Claim noticeClaim,
        Throwable failure) {
        if (failure == null) {
            followSource.markMessageFollowNoticeSent(noticeClaim);
        } else {
            followSource.abortMessageFollowNotice(noticeClaim);
        }
    }

    private static ZLinkServiceMessageFollowWireCodec.ActorRoute
        messageFollowSourceRoute(ZLinkServiceM6BWireCodec.ActorMessage staleHeader) {
        return new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            staleHeader.target().actor().actorId(),
            staleHeader.target().actor().generation(),
            staleHeader.target().actor().nodeRid(),
            staleHeader.target().targetNodeGeneration(),
            staleHeader.target().authorityOwnerGeneration(),
            staleHeader.target().ownerLeaseGeneration());
    }

    private ZLinkServiceMessageFollowWireCodec.ActorRoute
        localMessageFollowSourceRoute(ZLinkBackendActorRef sourceActor) {
        long nodeGeneration = spotNode.actorNodeGeneration(sourceActor);
        long authorityOwnerGeneration =
            spotNode.actorAuthorityOwnerGeneration(sourceActor);
        long ownerLeaseGeneration =
            spotNode.actorAuthorityOwnerLeaseGeneration(sourceActor);
        if (nodeGeneration <= 0
            || authorityOwnerGeneration <= 0
            || ownerLeaseGeneration <= 0) {
            throw new ZLinkConfigurationException(
                "committed Message Follow source route has an incomplete owner fence");
        }
        return new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            sourceActor.actorId(),
            sourceActor.generation(),
            sourceActor.nodeRid(),
            nodeGeneration,
            authorityOwnerGeneration,
            ownerLeaseGeneration);
    }

    private static Throwable unwrapCompletionFailure(Throwable failure) {
        return failure instanceof CompletionException && failure.getCause() != null
            ? failure.getCause()
            : failure;
    }

    private record MessageFollowRelayResult(
        Optional<Message> reply,
        ZLinkServiceMessageFollowWireCodec.ActorRoute route,
        ZLinkActorTransferHandoff.MessageFollowQueueSnapshot queue) {
    }

    public CompletionStage<Optional<Message>> dispatchRemoteJoinedActor(
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkStreamHeader header,
        Message payload) {
        ZLinkActorTransferHandoff.MessageFollowSource followSource =
            handoff.messageFollowSource(actorRef.actorId()).orElse(null);
        if (followSource != null) {
            if (followSource.targetAddress() == null) {
                return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "committed Message Follow route has no target address"));
            }
            return handoff.follow(
                actorRef.actorId(), actorRef.generation(), payload.size(),
                () -> dispatchRemoteJoinedActor(
                    actorRef,
                    header,
                    payload,
                    followSource.targetAddress(),
                    new byte[0]));
        }
        if (!canRouteRemoteJoinedSpot(spotId)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor Spot is not routable: " + spotId));
        }
        return resolveHandle(spotId)
            .thenCompose(remoteAddressResolver::resolve)
            .thenCompose(address -> dispatchRemoteJoinedActor(
                actorRef,
                header,
                payload,
                address.orElseThrow(() -> new ZLinkConfigurationException(
                    "SPOT transport address was not found: " + spotId)),
                new byte[0]));
    }

    private CompletionStage<Optional<Message>> dispatchRemoteJoinedActor(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        SpotTransportAddress target,
        byte[] acceptedJournalRecord) {
                List<Message> parts =
                    ZLinkActorSpotRoutePackets.createActorPacketParts(
                        actorRef,
                        header,
                        payload,
                        null,
                        null,
                        acceptedJournalRecord);
                Message packetName = Message.from(parts.getFirst());
                Message envelope = ZLinkActorEntryTransferEnvelope.encode(parts);
                List<Message> wireParts = List.of(packetName, envelope);
                try {
                    if (header.requestSequence().isPresent()
                        || header.kind() == ZLinkStreamMessageKind.REQUEST) {
                        return routedTransport.requestToSpotViaRouterChannel(
                                target.routerChannelId(),
                                target.targetNodeRid(),
                                target.spotId(),
                                target.spotGeneration(),
                                wireParts,
                                defaultRequestTimeout)
                            .thenApply(replyParts -> {
                                try {
                                    if (replyParts.isEmpty()
                                        || ZLinkActorSpotRoutePackets
                                            .isHandoffDirectReplyAck(
                                                replyParts.getFirst())) {
                                        return Optional.<Message>empty();
                                    }
                                    return Optional.of(
                                        Message.from(replyParts.getFirst()));
                                } finally {
                                    replyParts.forEach(Message::close);
                                }
                            });
                    }
                    return routedTransport.sendToSpotViaRouterChannel(
                            target.routerChannelId(),
                            target.targetNodeRid(),
                            target.spotId(),
                            target.spotGeneration(),
                            wireParts)
                        .thenApply(ignored -> Optional.<Message>empty());
                } finally {
                    wireParts.forEach(Message::close);
                    parts.forEach(Message::close);
                }
    }

    public boolean hasActorsInSpot(String spotId) {
        for (DefaultActorContext context : actorRegistry.contexts()) {
            if (spotId.equals(context.joinedSpotId())) {
                return true;
            }
        }
        return false;
    }

    public boolean isActorAtSpot(String actorId, String spotId) {
        ZLinkActor actor = actorRegistry.actor(actorId);
        DefaultActorContext context =
            actor == null ? null : actorRegistry.context(actor);
        return context != null
            && (Objects.equals(context.joinedSpotId(), spotId)
                || (context.currentSpot() == null
                    && Objects.equals(context.entrySpotId(), spotId)));
    }

    ZLinkActor actorById(String actorId) {
        return actorRegistry.actor(actorId);
    }

    public List<String> actorIdsInSpot(String spotId) {
        Objects.requireNonNull(spotId, "spotId");
        synchronized (this) {
            return actorRegistry.entries().stream()
                .filter(entry -> spotId.equals(
                    entry.context().joinedSpotId()))
                .map(entry -> entry.actor().context().actorId())
                .sorted()
                .toList();
        }
    }

    public CompletionStage<Optional<ZLinkActor>> getOrCreateLocalActor(
        String actorId,
        Class<?> expectedActorType) {
        requireActorId(actorId);
        ZLinkActor existing = actorRegistry.actor(actorId);
        if (existing != null) {
            return CompletableFuture.completedFuture(
                expectedActorType.isInstance(existing)
                    ? Optional.of(existing)
                    : Optional.empty());
        }
        if (factories.size() != 1) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        String actorType = factories.keySet().iterator().next();
        return createLocalActor(actorId, actorType, ZLinkMessage.empty(), false)
            .thenApply(actor -> expectedActorType.isInstance(actor)
                ? Optional.of(actor)
                : Optional.empty());
    }

    public CompletionStage<Void> submitActorDispatch(
        String actorId,
        Supplier<CompletionStage<Void>> operation) {
        return submitActorDispatch(actorId, null, operation);
    }

    public CompletionStage<Void> submitActorDispatch(
        String actorId,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return submitActorDispatch(
            actorId,
            null,
            payloadBytes,
            operation,
            () -> { });
    }

    public CompletionStage<Void> submitActorDispatch(
        String actorId,
        byte[] acceptedJournalRecord,
        Supplier<CompletionStage<Void>> operation) {
        return submitActorDispatch(
            actorId, acceptedJournalRecord, operation, () -> { });
    }

    public CompletionStage<Void> submitActorDispatch(
        String actorId,
        byte[] acceptedJournalRecord,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        return submitActorDispatch(
            actorId,
            acceptedJournalRecord,
            null,
            operation,
            relocationRelease);
    }

    public CompletionStage<Void> submitActorDispatchLazyRecord(
        String actorId,
        Supplier<byte[]> acceptedJournalRecord,
        long acceptedJournalRecordSizeHint,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        if (dispatches.isCurrent(actorId)) {
            try {
                return operation.get();
            } catch (RuntimeException ex) {
                return CompletableFuture.failedFuture(ex);
            }
        }
        ZLinkActorDispatchSerials.QueuedTurn turn;
        synchronized (this) {
            if (!actorRegistry.contains(actorId)) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "actor is not managed by this runtime: " + actorId));
            }
            turn = dispatches.prepare(actorId);
        }
        return dispatches.enqueueLazyRecord(
            turn,
            acceptedJournalRecord,
            acceptedJournalRecordSizeHint,
            operation,
            relocationRelease);
    }

    private CompletionStage<Void> submitActorDispatch(
        String actorId,
        byte[] acceptedJournalRecord,
        Long payloadBytes,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        if (dispatches.isCurrent(actorId)) {
            try {
                return operation.get();
            } catch (RuntimeException ex) {
                return CompletableFuture.failedFuture(ex);
            }
        }
        ZLinkActorDispatchSerials.QueuedTurn turn;
        synchronized (this) {
            if (!actorRegistry.contains(actorId)) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "actor is not managed by this runtime: " + actorId));
            }
            turn = dispatches.prepare(actorId);
        }
        if (acceptedJournalRecord != null) {
            return dispatches.enqueue(
                turn, acceptedJournalRecord, operation, relocationRelease);
        }
        return payloadBytes == null
            ? dispatches.enqueue(turn, operation)
            : dispatches.enqueue(
                turn, payloadBytes, operation, relocationRelease);
    }

    public Optional<ZLinkAsyncSerialQueue.RelocationSeal> trySealActorRelocation(
        String actorId) {
        return dispatches.trySeal(actorId);
    }

    /**
     * Returns the Framework-owned serial lane used to coordinate an Actor as
     * part of a User Spot aggregate relocation barrier.
     */
    public ZLinkAsyncSerialQueue actorRelocationLane(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        return dispatches.relocationLane(actorId);
    }

    public boolean abortActorRelocation(
        String actorId,
        ZLinkAsyncSerialQueue.RelocationSeal seal) {
        return dispatches.abort(actorId, seal);
    }

    public Optional<List<ZLinkAsyncSerialQueue.QueuedRecord>> commitActorRelocation(
        String actorId,
        ZLinkAsyncSerialQueue.RelocationSeal seal) {
        return dispatches.commit(actorId, seal);
    }

    public Optional<systems.zlink.framework.runtime.internal.relocation
        .ZLinkRetainedSerialQueueCommit.Commit>
        retainActorRelocationCommit(
            String actorId,
            ZLinkAsyncSerialQueue.RelocationSeal seal) {
        return dispatches.retainCommit(actorId, seal);
    }

    public Optional<List<ZLinkAsyncSerialQueue.QueuedRecord>>
        freezeActorRelocationIngress(
            String actorId,
            ZLinkAsyncSerialQueue.RelocationSeal seal) {
        return dispatches.freezeIngress(actorId, seal);
    }

    public <T> CompletionStage<T> runActorDispatchTurn(
        String actorId,
        Supplier<CompletionStage<T>> operation) {
        return dispatches.runTurn(actorId, operation);
    }

    public <T> CompletionStage<T> invokeActorLifecycle(
        ZLinkActor actor,
        Supplier<CompletionStage<T>> operation) {
        Objects.requireNonNull(actor, "actor");
        Objects.requireNonNull(operation, "operation");
        try (systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.Scope ignored =
                 systems.zlink.framework.runtime.internal.handlers
                     .ZLinkSuspendInvocationContext.enterActorDispatch(
                         actor.context().actorId())) {
            return Objects.requireNonNull(
                operation.get(), "Actor lifecycle callback result");
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    CompletionStage<Void> submitDeferredJoinBarrier(
        String actorId,
        Supplier<CompletionStage<Void>> operation) {
        if (!actorRegistry.contains(actorId)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actorId));
        }
        return dispatches.enqueueBarrier(actorId, operation);
    }

    boolean isActorDispatchActive(ZLinkActor actor) {
        return dispatches.isActive(actor.context().actorId());
    }

    void requireDeferredJoinRegistration(DefaultActorContext context) {
        if (draining || relocating) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                "Actor join admission is sealed while the runtime is draining");
        }
        ZLinkActor actor = context.actor();
        if (actor == null || actorRegistry.context(actor) != context) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "Actor context does not represent the current local incarnation");
        }
        if (context.moving()) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "Actor already has a membership transition in progress");
        }
    }

    Object deferredJoinRuntimeScope() {
        return this;
    }

    Object deferredJoinIncarnation(DefaultActorContext context) {
        return context;
    }

    private Object deferredJoinIncarnation(String actorId) {
        ZLinkActor actor = actorRegistry.actor(actorId);
        return actor == null ? actorId : actorRegistry.context(actor);
    }

    void continueAfterActorDispatch(
        ZLinkActor actor,
        Supplier<CompletionStage<Void>> operation) {
        ZLinkActorDispatchSerials.QueuedTurn turn;
        synchronized (this) {
            turn = dispatches.prepare(actor.context().actorId());
        }
        dispatches.enqueue(
            turn,
            () -> ZLinkAsyncSerialQueue.yieldCurrent(operation.get()))
            .whenComplete((ignored, error) -> {
                if (error != null) {
                    failRemoteMove(actor, error);
                }
            });
    }

    public void setDisconnectedNotifier(
        Function<ZLinkActor, CompletionStage<Void>> disconnectedNotifier) {
        this.disconnectedNotifier = disconnectedNotifier == null
            ? ignored -> CompletableFuture.completedFuture(null)
            : disconnectedNotifier;
    }

    public void setSourceActorLeaver(SourceActorLeaver sourceActorLeaver) {
        this.sourceActorLeaver = sourceActorLeaver == null
            ? ignored -> CompletableFuture.completedFuture(null)
            : sourceActorLeaver;
    }

    /**
     * Installs the host-owned local dispatch path used to restore packets
     * after a remote move fails before the target membership commit.
     * This is runtime wiring; it is not an application Actor contract.
     */
    public void setTransferBacklogRestorer(
        TransferBacklogRestorer transferBacklogRestorer) {
        this.transferBacklogRestorer = transferBacklogRestorer == null
            ? (ignoredActor, ignoredPacket) -> CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "remote Actor move backlog restorer is not configured"))
            : transferBacklogRestorer;
    }

    public void setLocalJoinCompleter(LocalJoinCompleter localJoinCompleter) {
        this.localJoinCompleter = localJoinCompleter == null
            ? unavailableLocalJoinCompleter()
            : localJoinCompleter;
    }

    public CompletionStage<Void> completeLocalJoinFromCaller(ZLinkActor actor) {
        return localJoinCompleter.complete(actor);
    }

    public void cancelLocalJoin(ZLinkActor actor) {
        localJoinCompleter.cancel(actor);
    }

    private static LocalJoinCompleter unavailableLocalJoinCompleter() {
        return new LocalJoinCompleter() {
            @Override
            public CompletionStage<Void> complete(ZLinkActor actor) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "local actor Spot join completion is unavailable"));
            }

            @Override
            public void cancel(ZLinkActor actor) {
            }
        };
    }

    public void setCreatedNotifier(CreatedNotifier createdNotifier) {
        this.createdNotifier = createdNotifier == null
            ? (ignoredNode, ignoredActor, ignoredRequest, ignoredContext) ->
                CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept())
            : createdNotifier;
    }

    private static final class ActorCreateRejected extends RuntimeException {
        private final ZLinkMessage reply;

        private ActorCreateRejected(ZLinkMessage reply) {
            super("actor creation was rejected by the Entry Spot");
            this.reply = reply;
        }

        private ZLinkMessage reply() {
            return reply;
        }
    }

    public void setActorCreateContextSupplier(Supplier<Object> actorCreateContextSupplier) {
        this.actorCreateContextSupplier = actorCreateContextSupplier == null
            ? () -> null
            : actorCreateContextSupplier;
    }

    public void setCreationSubmitter(CreationSubmitter submitter) {
        creationSubmitter = submitter;
    }

    public void setEntrySpotTargetSelector(
        EntrySpotTargetSelector selector) {
        entrySpotTargetSelector = selector == null
            ? (ignoredType, ignoredTimeout) ->
                CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                        "eligible Entry Spot selection is unavailable"))
            : selector;
    }

    public void setSpotResolver(Function<String, ZLinkSpot<?>> spotResolver) {
        this.spotResolver = spotResolver == null ? ignored -> null : spotResolver;
    }

    public void setSpotMeshResolver(Function<String, String> spotMeshResolver) {
        locations.setSpotMeshResolver(spotMeshResolver);
    }

    public void setRemoteAddressResolver(SpotTransportAddressResolver remoteAddressResolver) {
        this.remoteAddressResolver = remoteAddressResolver;
    }

    private CompletionStage<SpotHandle> resolveHandle(String spotId) {
        if (!(remoteAddressResolver instanceof SpotHandleResolver handles)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "SPOT transport resolver does not provide opaque handles"));
        }
        return handles.resolveSpotHandle(spotId).thenApply(handle -> handle.orElseThrow(() ->
            new ZLinkConfigurationException("SPOT handle was not found: " + spotId)));
    }

    public void setLocationLifecycle(ZLinkLocationLifecycle lifecycle) {
        locations.setLifecycle(lifecycle);
    }

    public void setStoreLocationResolvers(ZLinkStoreLocationResolvers resolvers) {
        locations.setResolvers(resolvers);
    }

    public void setRoutedTransport(
        ZLinkChannelRuntime routedTransport,
        Supplier<String> sourceEntrySpotId) {
        this.routedTransport = routedTransport;
        this.sourceEntrySpotId = sourceEntrySpotId == null
            ? () -> ""
            : sourceEntrySpotId;
    }

    void setActorJoinTransferTransport(
        ZLinkActorSpotJoinCall.TransferTransport transferTransport) {
        actorJoinTransferTransport = transferTransport;
    }

    public CompletionStage<Void> notifyDisconnected(ZLinkActor actor) {
        if (actor == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "actor is required"));
        }
        return submitActorDispatch(
            actor.context().actorId(),
            () -> disconnectedNotifier.apply(actor));
    }

    private void markJoinedState(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkSpot<?> spot) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        context.markJoined(actorRef, spotId, spot);
    }

    public CompletionStage<Void> markJoined(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkSpot<?> spot) {
        markJoinedState(actor, actorRef, spotId, spot);
        return renewActorJoinedLocation(actor, spotId);
    }

    /** Applies the membership already selected by the canonical target CAS. */
    public void markRelocatedActorJoined(
        ZLinkActor actor,
        ZLinkBackendActorRef actorRef,
        String spotId,
        ZLinkSpot<?> spot) {
        markJoinedState(actor, actorRef, spotId, spot);
    }

    public long bindNativeSession(
        ZLinkActor actor,
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendActorRef actorRef) {
        return bindNativeSession(actor, spotNode, actorRef, null, null);
    }

    public long bindNativeSession(
        ZLinkActor actor,
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
        return bindNativeSession(
            actor,
            spotNode,
            actorRef,
            sourceNodeRid,
            sourceSessionRid,
            0,
            0);
    }

    public long bindNativeSession(
        ZLinkActor actor,
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long bindingGeneration,
        long lastAcceptedSessionSequence) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        context.markNativeActorRef(actorRef, sourceNodeRid, sourceSessionRid);
        if (sourceNodeRid != null
            && sourceSessionRid != null
            && !sourceNodeRid.equals(actorRef.nodeRid())) {
            spotNode.bindRemoteActorBoundSession(actorRef, sourceNodeRid, sourceSessionRid);
        }
        ZLinkNativeBoundSessionRuntime boundSession = new ZLinkNativeBoundSessionRuntime(
            spotNode,
            actorRef,
            serializer,
            this,
            actor,
            sourceNodeRid,
            sourceSessionRid,
            defaultRequestTimeout,
            defaultStreamCodec,
            metadataPolicy);
        long bindingToken = bindSession(
            actor,
            boundSession,
            sourceNodeRid,
            sourceSessionRid,
            bindingGeneration,
            lastAcceptedSessionSequence);
        boundSession.setBindingToken(bindingToken);
        return bindingToken;
    }

    public long bindRoutedSession(
        ZLinkActor actor,
        String routeChannelName,
        RoutingId targetRoutePeerRid,
        String targetEntrySpotId,
        ZLinkBackendActorRef actorRef) {
        ZLinkRoutedBoundSessionRuntime boundSession = new ZLinkRoutedBoundSessionRuntime(
            spotNode.entrySpot(),
            routedTransport,
            routeChannelName,
            targetRoutePeerRid,
            targetEntrySpotId,
            actorRef,
            serializer,
            this,
            actor,
            defaultRequestTimeout,
            defaultStreamCodec,
            metadataPolicy);
        long bindingToken = bindSession(actor, boundSession);
        boundSession.setBindingToken(bindingToken);
        return bindingToken;
    }

    public RoutingId entrySpotNodeRid(ZLinkActor actor) {
        return requireContext(actor).entrySpotNodeRid();
    }

    public boolean isRoutedTransferActor(ZLinkActor actor) {
        requireContext(actor);
        return actorRegistry.isRoutedTransfer(actor.context().actorId());
    }

    public String entrySpotId(ZLinkActor actor) {
        return requireContext(actor).entrySpotId();
    }

    public String entryRouterChannelId(ZLinkActor actor) {
        return requireContext(actor).entryRouterChannelId();
    }

    public void setEntrySpotNodeRid(ZLinkActor actor, RoutingId entrySpotNodeRid) {
        requireContext(actor).setEntrySpotNodeRid(entrySpotNodeRid);
    }

    public void setEntrySpotId(ZLinkActor actor, String entrySpotId) {
        requireContext(actor).setEntrySpotId(entrySpotId);
    }

    public void setEntryRouterChannelId(ZLinkActor actor, String entryRouterChannelId) {
        requireContext(actor).setEntryRouterChannelId(entryRouterChannelId);
    }

    public CompletionStage<Void> joinEntrySpot(
        ZLinkActor actor,
        RoutingId entrySpotNodeRid,
        Duration timeout) {
        DefaultActorContext context = requireContext(actor);
        Message request = Message.from(new byte[0]);
        return new ZLinkActorEntrySpotJoinCall(
                context,
                ignored -> CompletableFuture.completedFuture(
                    new EntrySpotTarget(
                        entrySpotNodeRid,
                        context.entrySpotId())),
                request,
                timeout,
                context.entrySpotJoinServices())
            .execute()
            .thenCompose(result -> result instanceof ZLinkActorJoinOutcome.Accepted
                ? CompletableFuture.<Void>completedFuture(null)
                //  Spec 15-spot-actor:404,459 — the internal return-to-Entry path
                //  has no application admission, so a received Rejected reply
                //  violates this operation's reply contract: ProtocolError.
                : CompletableFuture.<Void>failedFuture(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "actor Entry Spot join was rejected: " + actor.context().actorId())))
            .whenComplete((ignored, error) -> request.close());
    }

    private void markLeftState(ZLinkActor actor) {
        DefaultActorContext context = actorRegistry.context(actor);
        if (context == null) {
            throw new ZLinkConfigurationException(
                "actor is not managed by this runtime: " + actor.context().actorId());
        }
        context.markLeft();
    }

    public CompletionStage<Void> markLeft(ZLinkActor actor) {
        markLeftState(actor);
        return renewActorLeftLocation(actor);
    }

    public CompletionStage<Void> destroyFromEntrySpot(
        RoutingId entryNodeRid,
        ZLinkActor actor) {
        if (entryNodeRid == null) {
            throw new ZLinkConfigurationException("entryNodeRid is required");
        }
        if (actor == null) {
            throw new ZLinkConfigurationException("actor is required");
        }

        DefaultActorContext context;
        ZLinkBackendActorRef actorRef;
        synchronized (this) {
            ZLinkActor current = actorRegistry.actor(actor.context().actorId());
            if (current == null || current != actor) {
                return CompletableFuture.completedFuture(null);
            }

            context = actorRegistry.context(actor);
            if (context == null) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "actor does not have a native Actor ref: " + actor.context().actorId()));
            }
            try {
                actorRef = context.beginDestroy(entryNodeRid, actor.context().actorId());
            } catch (ZLinkConfigurationException ex) {
                return CompletableFuture.failedFuture(ex);
            }
            if (actorRef == null) {
                return CompletableFuture.completedFuture(null);
            }
        }

        String actorType = actorRegistry.actorTypeOrDefault(actor.context().actorId(), "");
        String actorId = actor.context().actorId();
        return dispatches.beginTeardown(actorId, () ->
            spotNode.destroyActor(actorRef, defaultRequestTimeout)
                .thenCompose(ignored -> context.disconnectBoundSessionForDestroy()
                    .exceptionally(error -> null))
                .thenCompose(ignored -> locations.releaseActorExact(
                    actorType,
                    new ActorRef(
                        actorId,
                        actorRef.generation(),
                        meshName,
                        actorRef.nodeRid())))
                .thenRun(() -> {
                    synchronized (this) {
                        removeActorSessionRouteForContext(context);
                        actorRegistry.remove(actorId, actor);
                    }
                    context.clearAfterDestroy();
                }))
            .whenComplete((ignored, error) -> {
                if (error != null) {
                    synchronized (this) {
                        context.resetDestroying();
                    }
                }
            });
    }

    public void close() {
        closeAsync();
    }

    public CompletionStage<Void> closeAsync() {
        draining = true;
        stopDirectJoinSessionRecovery(
            "Actor runtime closed with a pending direct-Join Session abort");
        handoff.close();
        List<ActorEntry> snapshot;
        synchronized (this) {
            snapshot = actorRegistry.entries();
        }
        CompletableFuture<?>[] closed = snapshot.stream()
            .map(this::closeActorEntry)
            .map(CompletionStage::toCompletableFuture)
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(closed);
    }

    private void stopDirectJoinSessionRecovery(String message) {
        //  Spec 32-framework-error-model:39, 15-spot-actor:375 — a pending
        //  operation aborted by runtime shutdown/drain is ShuttingDown.
        ZLinkFrameworkException pendingAbortClosed =
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.SHUTTING_DOWN, message);
        directJoinSessionAborts.forEach((key, flight) -> {
            if (directJoinSessionAborts.remove(key, flight)) {
                flight.completion().completeExceptionally(pendingAbortClosed);
            }
        });
    }

    /** Removes a committed relocation source without releasing its published
     * authority, which already belongs to the target owner. */
    public CompletionStage<Void> completeRelocationSource(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        ZLinkActor actor;
        DefaultActorContext context;
        synchronized (this) {
            actor = actorRegistry.actor(actorId);
            if (actor == null) {
                return CompletableFuture.completedFuture(null);
            }
            context = actorRegistry.context(actor);
            if (context == null) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "relocation source Actor context is unavailable: "
                            + actorId));
            }
        }
        return dispatches.beginTeardown(actorId, () -> {
            synchronized (this) {
                removeActorSessionRouteForContext(context);
                actorRegistry.remove(actorId, actor);
            }
            context.clearAfterDestroy();
            return CompletableFuture.completedFuture(null);
        });
    }

    private CompletionStage<Void> closeActorEntry(ActorEntry entry) {
        ZLinkActor actor = entry.actor();
        DefaultActorContext context = entry.context();
        String actorId = actor.context().actorId();
        return dispatches.beginTeardown(actorId, () ->
            locations.releaseActor(entry.actorType(), actorId)
                .exceptionally(error -> null)
                .thenRun(() -> {
                    synchronized (this) {
                        removeActorSessionRouteForContext(context);
                        actorRegistry.remove(actorId, actor);
                    }
                    context.clearAfterDestroy();
                }));
    }

    private final class ActorRegistry {
        private final Map<String, ActorEntry> byId = new HashMap<>();
        private final Map<ZLinkActor, ActorEntry> byActor = new IdentityHashMap<>();

        synchronized void register(
            String actorId,
            String actorType,
            ZLinkActor actor,
            DefaultActorContext context) {
            ActorEntry entry = new ActorEntry(actorType, actor, context);
            ActorEntry previous = byId.put(actorId, entry);
            if (previous != null) {
                byActor.remove(previous.actor());
            }
            byActor.put(actor, entry);
        }

        synchronized boolean contains(String actorId) {
            return byId.containsKey(actorId);
        }

        synchronized ZLinkActor actor(String actorId) {
            ActorEntry entry = byId.get(actorId);
            return entry == null ? null : entry.actor();
        }

        synchronized DefaultActorContext context(ZLinkActor actor) {
            ActorEntry entry = byActor.get(actor);
            return entry == null ? null : entry.context();
        }

        synchronized String actorType(String actorId) {
            ActorEntry entry = byId.get(actorId);
            return entry == null ? null : entry.actorType();
        }

        synchronized String actorTypeOrDefault(String actorId, String fallback) {
            String actorType = actorType(actorId);
            return actorType == null ? fallback : actorType;
        }

        synchronized void markTransferred(String actorId) {
            ActorEntry entry = byId.get(actorId);
            if (entry == null) {
                throw new ZLinkConfigurationException(
                    "actor is not managed by this runtime: " + actorId);
            }
            entry.pendingTransfer = true;
            entry.routedTransfer = true;
        }

        synchronized boolean isPendingTransfer(String actorId) {
            ActorEntry entry = byId.get(actorId);
            return entry != null && entry.pendingTransfer;
        }

        synchronized boolean clearPendingTransfer(String actorId) {
            ActorEntry entry = byId.get(actorId);
            if (entry == null || !entry.pendingTransfer) {
                return false;
            }
            entry.pendingTransfer = false;
            return true;
        }

        synchronized boolean isRoutedTransfer(String actorId) {
            ActorEntry entry = byId.get(actorId);
            return entry != null && entry.routedTransfer;
        }

        synchronized ActorEntry remove(String actorId) {
            ActorEntry removed = byId.remove(actorId);
            if (removed != null) {
                byActor.remove(removed.actor());
                acceptedHandoffOperations.remove(actorId);
            }
            return removed;
        }

        synchronized boolean remove(String actorId, ZLinkActor actor) {
            ActorEntry entry = byId.get(actorId);
            if (entry == null || entry.actor() != actor) {
                return false;
            }
            byId.remove(actorId);
            byActor.remove(actor);
            acceptedHandoffOperations.remove(actorId);
            return true;
        }

        synchronized List<ActorEntry> entries() {
            return List.copyOf(byId.values());
        }

        synchronized List<DefaultActorContext> contexts() {
            return byId.values().stream().map(ActorEntry::context).toList();
        }
    }

    private final class ActorEntry {
        private final String actorType;
        private final ZLinkActor actor;
        private final DefaultActorContext context;
        private boolean pendingTransfer;
        private boolean routedTransfer;

        private ActorEntry(
            String actorType,
            ZLinkActor actor,
            DefaultActorContext context) {
            this.actorType = actorType;
            this.actor = actor;
            this.context = context;
        }

        String actorType() {
            return actorType;
        }

        ZLinkActor actor() {
            return actor;
        }

        DefaultActorContext context() {
            return context;
        }
    }

    final class DefaultActorContext implements ZLinkActorContext {
        private final ZLinkActorContextState state;
        private final systems.zlink.framework.runtime.internal.handlers
            .ZLinkHandlerInstanceOwner handlerInstances;

        DefaultActorContext(ZLinkBackendActorRef actorRef) {
            String configuredEntrySpotId = sourceEntrySpotId.get();
            this.state = new ZLinkActorContextState(
                actorRef,
                meshName,
                configuredEntrySpotId == null || configuredEntrySpotId.toString().isBlank()
                    ? actorRef.nodeRid().toString()
                    : configuredEntrySpotId);
            this.handlerInstances = new systems.zlink.framework.runtime.internal.handlers
                .ZLinkHandlerInstanceOwner(handlerFactory);
        }

        ZLinkActor actor() {
            return state.actor();
        }

        void setActor(ZLinkActor actor) {
            state.setActor(actor);
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkActorHandlerInstances.bind(actor, handlerInstances);
        }

        boolean tryClaimDeferredJoin(Object claim) {
            return state.tryClaimDeferredJoin(claim);
        }

        void releaseDeferredJoin(Object claim) {
            state.releaseDeferredJoin(claim);
        }

        void beginMove() {
            state.beginMove();
        }

        void endMove() {
            state.endMove();
        }

        void failMove(Throwable error) {
            state.failMove(error);
        }

        boolean moving() {
            return state.moving();
        }

        CompletionStage<Void> moveCompletion() {
            return state.moveCompletion();
        }

        ZLinkBackendActorRef actorRef() {
            return state.actorRef();
        }

        String joinedSpotId() {
            return state.spotId();
        }

        ZLinkSpot<?> currentSpot() {
            return state.spot();
        }

        RoutingId boundSessionSourceNodeRid() {
            return state.boundSessionSourceNodeRid();
        }

        RoutingId boundSessionSourceSessionRid() {
            return state.boundSessionSourceSessionRid();
        }

        RoutingId entrySpotNodeRid() {
            return state.entrySpotNodeRid();
        }

        String entrySpotId() {
            return state.entrySpotId();
        }

        String entryRouterChannelId() {
            return state.entryRouterChannelId();
        }

        void setEntrySpotNodeRid(RoutingId entrySpotNodeRid) {
            state.setEntrySpotNodeRid(entrySpotNodeRid);
        }

        void setEntrySpotId(String entrySpotId) {
            state.setEntrySpotId(entrySpotId);
        }

        void setEntryRouterChannelId(String entryRouterChannelId) {
            state.setEntryRouterChannelId(entryRouterChannelId);
        }

        void markJoined(
            ZLinkBackendActorRef actorRef,
            String spotId,
            ZLinkSpot<?> spot) {
            state.markJoined(actorRef, spotId, spot);
        }

        void markMovedToEntrySpot(
            ZLinkBackendActorRef actorRef,
            EntrySpotTarget target) {
            state.markMovedToEntrySpot(
                actorRef,
                target.nodeRid(),
                target.spotId());
        }

        void markLeft() {
            state.markLeft();
        }

        void markNativeActorRef(
            ZLinkBackendActorRef actorRef,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid) {
            state.markNativeActorRef(actorRef, sourceNodeRid, sourceSessionRid);
        }

        @Override
        public String actorId() {
            return state.actorId();
        }

        @Override
        public long objectGeneration() {
            return state.objectGeneration();
        }

        @Override
        public String meshName() {
            return state.meshName();
        }

        @Override
        public Optional<String> spotId() {
            return Optional.ofNullable(state.spotId());
        }

        @Override
        public ZLinkBoundSession boundSession() {
            return state.requireBoundSession();
        }

        @Override
        public ZLinkActorJoinCall joinEntrySpot() {
            return createJoinEntrySpotCall(
                Message.from(new byte[0]),
                defaultRequestTimeout);
        }

        @Override
        public ZLinkActorJoinCall joinEntrySpot(Object request) {
            if (request == null) {
                throw new ZLinkConfigurationException("request is required");
            }
            return createJoinEntrySpotCall(
                messageFromRequest(request),
                defaultRequestTimeout);
        }

        private ZLinkActorJoinCall createJoinEntrySpotCall(
            Message request,
            Duration timeout) {
            return new ZLinkActorEntrySpotJoinCall(
                this,
                requestedTimeout -> entrySpotTargetSelector.select(
                    actorRegistry.actorType(state.actorId()),
                    requestedTimeout),
                request,
                timeout,
                entrySpotJoinServices());
        }

        private ZLinkActorEntrySpotJoinCall.Services entrySpotJoinServices() {
            return new ZLinkActorEntrySpotJoinCall.Services(
                spotNode,
                serializer,
                ZLinkActorRuntime.this,
                ZLinkActorRuntime.this::renewActorMovedToEntrySpotLocation);
        }

        @Override
        public ZLinkActorJoinCall joinSpot(String spotId) {
            if (spotId == null) {
                throw new ZLinkConfigurationException("spotId is required");
            }
            return createJoinSpotCall(
                spotId,
                Message.from(new byte[0]),
                defaultRequestTimeout);
        }

        @Override
        public ZLinkActorJoinCall joinSpot(String spotId, Object request) {
            if (spotId == null) {
                throw new ZLinkConfigurationException("spotId is required");
            }
            if (request == null) {
                throw new ZLinkConfigurationException("request is required");
            }
            return createJoinSpotCall(spotId, messageFromRequest(request), defaultRequestTimeout);
        }

        private ZLinkActorJoinCall createJoinSpotCall(
            String spotId,
            Message request,
            Duration timeout) {
            return new ZLinkActorSpotJoinCall(
                this,
                spotId,
                request,
                timeout,
                spotJoinServices());
        }

        ZLinkActorSpotJoinCall.Services spotJoinServices() {
            return new ZLinkActorSpotJoinCall.Services(
                spotNode,
                spotResolver,
                remoteAddressResolver,
                routedTransport,
                actorJoinTransferTransport,
                actorRegistry::actorType,
                serializer,
                flow,
                ZLinkActorRuntime.this,
                ZLinkActorRuntime.this::renewActorJoinedLocation);
        }

        private Message messageFromRequest(Object request) {
            if (request instanceof ZLinkMessage message) {
                return Message.from(message.toEncodedPayload(serializer).bytes());
            }
            ZLinkPayloadEncoding.EncodedPayload encoded =
                ZLinkPayloadEncoding.encode(serializer, request);
            return encoded.payload();
        }

        long bindSession(
            ZLinkBoundSession boundSession,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid) {
            return bindSession(
                boundSession,
                sourceNodeRid,
                sourceSessionRid,
                0,
                0);
        }

        long bindSession(
            ZLinkBoundSession boundSession,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            long bindingGeneration,
            long lastAcceptedSessionSequence) {
            return state.bindSession(
                boundSession,
                sourceNodeRid,
                sourceSessionRid,
                bindingGeneration,
                lastAcceptedSessionSequence);
        }

        RoutingId boundSessionSourceSessionRid(long bindingToken) {
            return state.boundSessionSourceSessionRid(bindingToken);
        }

        ZLinkActorContextState.BoundSessionSource nextBoundSessionSource() {
            return state.nextBoundSessionSource();
        }

        ZLinkActorContextState.BoundSessionSource nextBoundSessionSource(
            long acceptedSessionSequence) {
            return state.nextBoundSessionSource(acceptedSessionSequence);
        }

        ZLinkActorContextState.BoundSessionSource
            boundSessionSourceSnapshot() {
            return state.boundSessionSourceSnapshot();
        }

        boolean clearBoundSession(long bindingToken) {
            return state.clearBoundSession(bindingToken);
        }

        CompletionStage<Void> rebindNativeActor(
            ZLinkBackendActorRef targetActor,
            Duration timeout) {
            return state.rebindNativeActor(targetActor, timeout);
        }

        void updateNativeBoundSessionActorRef(ZLinkBackendActorRef targetActor) {
            state.updateNativeBoundSessionActorRef(targetActor);
        }

        boolean hasBoundSession() {
            return state.hasBoundSession();
        }

        boolean hasBoundSession(
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid) {
            return state.hasBoundSession(sourceNodeRid, sourceSessionRid);
        }

        CompletionStage<Boolean> sendBoundSessionFrame(byte[] frameBytes) {
            return state.sendBoundSessionFrame(frameBytes);
        }

        CompletionStage<Void> disconnectBoundSessionForDestroy() {
            return state.disconnectBoundSessionForDestroy();
        }

        void clearAfterDestroy() {
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkActorHandlerInstances.unbind(state.actor(), handlerInstances);
            handlerInstances.close();
            state.clearAfterDestroy();
        }

        void closeHandlerInstances() {
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkActorHandlerInstances.unbind(state.actor(), handlerInstances);
            handlerInstances.close();
        }

        ZLinkHandlerInstanceOwner
            handlerInstances() {
            return handlerInstances;
        }

        ZLinkBackendActorRef beginDestroy(RoutingId entryNodeRid, String actorId) {
            return state.beginDestroy(entryNodeRid, actorId);
        }

        void resetDestroying() {
            state.resetDestroying();
        }
    }

}
