package systems.zlink.e2e.spotactortransfer.actor;

import java.time.Duration;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkSessionPacketDispatcher;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkTypedSessionPacketHandler;
import systems.zlink.e2e.spotactortransfer.shared.Contracts;

public final class TransferComponents {
    private TransferComponents() {
    }

    public static final class TransferActor implements ZLinkActor {
        private final String actorId;
        private final ZLinkActorContext context;
        private final EvidenceStore evidence;
        private String actorType = Contracts.STATEFUL;
        private int stateVersion;

        public TransferActor(
            String actorId,
            ZLinkActorContext context,
            EvidenceStore evidence) {
            this.actorId = actorId;
            this.context = context;
            this.evidence = evidence;
        }

        public String actorId() {
            return actorId;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        public String actorType() {
            return actorType;
        }

        public void setActorType(String actorType) {
            this.actorType = actorType;
        }

        public int stateVersion() {
            return stateVersion;
        }

        public void setStateVersion(int stateVersion) {
            this.stateVersion = stateVersion;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(
            ZLinkActorJoinCompletion completion) {
            if (completion instanceof ZLinkActorJoinCompletion.Accepted accepted) {
                evidence.add(
                    "transfer",
                    actorId,
                    "commit_ack",
                    accepted.actor().nodeRid().toString());
            } else if (completion instanceof ZLinkActorJoinCompletion.Rejected) {
                evidence.add("transfer", actorId, "join_rejected", "");
            } else if (completion instanceof ZLinkActorJoinCompletion.Failed failed) {
                evidence.add("transfer", actorId, "join_failed", failed.kind().name());
            }
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TransferActorFactory implements ZLinkActorFactory {
        private final EvidenceStore evidence;

        public TransferActorFactory(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            String actorId = context.actorId();
            if ("actor-b".equals(evidence.nodeRid()) && actorId.startsWith("actor-no-adapter-")) {
                evidence.add("transfer", actorId, "transfer_in_empty_default", "actor-factory");
            }
            return CompletableFuture.completedFuture(
                new TransferActor(actorId, context, evidence));
        }
    }

    public static final class TransferActorAdapter
        implements ZLinkActorRelocationAdapter<TransferActor> {
        private final EvidenceStore evidence;
        private final GateStore transferGates;

        public TransferActorAdapter(EvidenceStore evidence, GateStore transferGates) {
            this.evidence = evidence;
            this.transferGates = transferGates;
        }

        @Override
        public CompletionStage<byte[]> capture(
            TransferActor actor,
            ZLinkRelocationCancellation cancellation) {
            if (Contracts.FAIL_OUT.equals(actor.actorType())) {
                evidence.add("ST-C3", actor.actorId(), "transfer_out_failed",
                    Integer.toString(actor.stateVersion()));
                return CompletableFuture.failedFuture(
                    new IllegalStateException("injected transfer out failure"));
            }
            if (Contracts.EMPTY_STATE.equals(actor.actorType())) {
                evidence.add("transfer", actor.actorId(), "transfer_out_empty", "custom-adapter");
                return CompletableFuture.completedFuture(new byte[0]);
            }
            evidence.add("transfer", actor.actorId(), "transfer_out",
                Integer.toString(actor.stateVersion()));
            if (actor.actorId().startsWith("actor-source-down-before-commit-")) {
                evidence.add("ST-C1", actor.actorId(), "before_commit_gate",
                    Integer.toString(actor.stateVersion()));
                return transferGates.waitFor(actor.actorId()).thenApply(ignored ->
                    transferState(actor));
            }
            if (actor.actorId().startsWith("actor-retire-accepted-")) {
                evidence.add(
                    "ST-R1",
                    actor.actorId(),
                    "retire_capture_gate",
                    Integer.toString(actor.stateVersion()));
                return transferGates.waitFor(actor.actorId()).thenApply(ignored ->
                    transferState(actor));
            }
            return CompletableFuture.completedFuture(transferState(actor));
        }

        private static byte[] transferState(TransferActor actor) {
            byte[] actorType = actor.actorType().getBytes(StandardCharsets.UTF_8);
            return ByteBuffer.allocate(Integer.BYTES * 2 + actorType.length)
                .putInt(actor.stateVersion())
                .putInt(actorType.length)
                .put(actorType)
                .array();
        }

        @Override
        public CompletionStage<Void> restore(
            TransferActor actor,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            String actorId = actor.context().actorId();
            if (state.length == 0) {
                evidence.add(
                    "transfer", actorId, "transfer_in_empty", "custom-adapter");
                actor.setActorType(Contracts.EMPTY_STATE);
                return CompletableFuture.completedFuture(null);
            }
            ByteBuffer payload = ByteBuffer.wrap(state);
            int stateVersion = payload.getInt();
            int actorTypeLength = payload.getInt();
            if (actorTypeLength < 0 || actorTypeLength > payload.remaining()) {
                return CompletableFuture.failedFuture(
                    new IllegalArgumentException("invalid Actor relocation state"));
            }
            byte[] actorType = new byte[actorTypeLength];
            payload.get(actorType);
            if (actorId.startsWith("actor-fail-transfer-in-")) {
                evidence.add("ST-C3", actorId, "transfer_in_failed",
                    Integer.toString(stateVersion));
                return CompletableFuture.failedFuture(
                    new IllegalStateException("injected transfer in failure"));
            }
            actor.setActorType(new String(actorType, StandardCharsets.UTF_8));
            actor.setStateVersion(stateVersion);
            evidence.add("transfer", actorId, "transfer_in",
                Integer.toString(actor.stateVersion()));
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TransferEntrySpot implements ZLinkEntrySpot<TransferActor> {
        private final ZLinkEntrySpotContext context;
        private final EvidenceStore evidence;
        private final DomainStateStore domainState;

        public TransferEntrySpot(
            ZLinkEntrySpotContext context,
            EvidenceStore evidence,
            DomainStateStore domainState) {
            this.context = context;
            this.evidence = evidence;
            this.domainState = domainState;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinTargetHandler.class);
            context.handlers().addHandler(EntryProbeHandler.class);
            context.handlers().addHandler(EntryBoundPushHandler.class);
        }

        @Override
        public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
            TransferActor actor,
            ZLinkMessage createRequest) {
            if (!createRequest.isEmpty()) {
                Contracts.ActorCreateReq request = createRequest.decode(Contracts.ActorCreateReq.class);
                actor.setActorType(request.actorType());
                actor.setStateVersion(request.stateVersion());
                if (Contracts.EMPTY_STATE.equals(request.actorType())) {
                    domainState.save(actor.actorId(), actor.stateVersion());
                }
            }
            evidence.add("create", actor.actorId(), "create",
                actor.actorType() + ":" + actor.stateVersion());
            return CompletableFuture.completedFuture(
                ZLinkActorCreateResponse.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(TransferActor actor) {
            evidence.add("local", actor.actorId(), "entry_joined",
                Integer.toString(actor.stateVersion()));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(TransferActor actor) {
            if (Contracts.NO_ADAPTER.equals(actor.actorType())) {
                evidence.add("transfer", actor.actorId(), "transfer_out_empty_default", "no-adapter");
            }
            if (Contracts.FAIL_LEAVE.equals(actor.actorType())) {
                evidence.add("ST-C3", actor.actorId(), "leave_failed",
                    Integer.toString(actor.stateVersion()));
                return CompletableFuture.failedFuture(
                    new IllegalStateException("injected source leave failure"));
            }
            evidence.add("transfer", actor.actorId(), "leave",
                Integer.toString(actor.stateVersion()));
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TransferUserSpot implements ZLinkSpot<TransferActor> {
        private final ZLinkSpotContext context;
        private final EvidenceStore evidence;
        private final GateStore joinedGates;
        private final DomainStateStore domainState;
        private final ConcurrentHashMap<String, String> scenarios = new ConcurrentHashMap<>();
        private String mode = "accept";

        public TransferUserSpot(
            ZLinkSpotContext context,
            EvidenceStore evidence,
            GateStore joinedGates,
            DomainStateStore domainState) {
            this.context = context;
            this.evidence = evidence;
            this.joinedGates = joinedGates;
            this.domainState = domainState;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(UserJoinTargetHandler.class);
            context.handlers().addHandler(ProbeHandler.class);
            context.handlers().addHandler(BoundPushHandler.class);
            context.handlers().addHandler(MessageFollowSendHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            if (!request.isEmpty()) {
                String requested = request.decode(Contracts.CreateSpotReq.class).mode();
                mode = requested == null || requested.isBlank() ? "accept" : requested;
            }
            evidence.add("create_spot", context.spotId(), "spot_created", mode);
            return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
            String actorId,
            ZLinkMessage request) {
            Contracts.JoinTargetReq join = request.decode(Contracts.JoinTargetReq.class);
            scenarios.put(actorId, join.scenario());
            evidence.add(join.scenario(), actorId, "admission",
                "spot=" + context.spotId() + ";mode=" + mode + ";input=actor-id-only");
            boolean reject = "reject".equals(mode) || "reject".equals(join.expectedMode());
            Contracts.JoinTargetRes response = new Contracts.JoinTargetRes(
                join.scenario(), actorId, !reject, "", context.spotId(), 0);
            return CompletableFuture.completedFuture(reject
                ? ZLinkSpotActorJoinResult.reject(response)
                : ZLinkSpotActorJoinResult.accept(response));
        }

        @Override
        public CompletionStage<Void> onJoinedActor(TransferActor actor) {
            String scenario = scenarios.getOrDefault(actor.actorId(), "transfer");
            if ("delay-joined".equals(mode)) {
                evidence.add(scenario, actor.actorId(), "joined_wait", context.spotId());
                return joinedGates.waitFor(context.spotId()).thenCompose(ignored -> {
                    evidence.add(scenario, actor.actorId(), "joined_released", context.spotId());
                    return completeJoined(actor);
                });
            }
            return completeJoined(actor);
        }

        private CompletionStage<Void> completeJoined(TransferActor actor) {
            if ("fail-joined".equals(mode)) {
                String scenario = scenarios.getOrDefault(actor.actorId(), "transfer");
                evidence.add(scenario, actor.actorId(), "joined_failed", context.spotId());
                return CompletableFuture.failedFuture(
                    new IllegalStateException("injected joined failure"));
            }
            evidence.add("transfer", actor.actorId(), "joined",
                context.spotId() + ":" + actor.stateVersion());
            if (Contracts.EMPTY_STATE.equals(actor.actorType())) {
                actor.setStateVersion(domainState.load(actor.actorId()));
                evidence.add("transfer", actor.actorId(), "domain_state_loaded",
                    Integer.toString(actor.stateVersion()));
            }
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(TransferActor actor) {
            evidence.add("transfer", actor.actorId(), "target_leave", context.spotId());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class JoinTargetHandler implements ZLinkEntrySpotActorRequestHandler<
        TransferEntrySpot,
        TransferActor,
        Contracts.JoinTargetReq,
        Contracts.JoinTargetRes> {
        private final EvidenceStore evidence;

        public JoinTargetHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.JoinTargetRes> handle(
            TransferEntrySpot entrySpot,
            TransferActor actor,
            ZLinkMessageContext context,
            Contracts.JoinTargetReq request) {
            return joinTarget(actor, request, evidence);
        }
    }

    public static final class UserJoinTargetHandler implements ZLinkSpotActorRequestHandler<
        TransferUserSpot,
        TransferActor,
        Contracts.JoinTargetReq,
        Contracts.JoinTargetRes> {
        private final EvidenceStore evidence;

        public UserJoinTargetHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.JoinTargetRes> handle(
            TransferUserSpot spot,
            TransferActor actor,
            ZLinkMessageContext context,
            Contracts.JoinTargetReq request) {
            return joinTarget(actor, request, evidence);
        }
    }

    private static CompletionStage<Contracts.JoinTargetRes> joinTarget(
        TransferActor actor,
        Contracts.JoinTargetReq request,
        EvidenceStore evidence) {
        actor.context()
            .joinSpot(request.targetSpotRid(), request)
            .timeout(Duration.ofSeconds(10))
            .defer();
        evidence.add(
            request.scenario(),
            actor.actorId(),
            "join_deferred",
            request.targetSpotRid());
        return CompletableFuture.completedFuture(new Contracts.JoinTargetRes(
            request.scenario(), actor.actorId(), true, evidence.nodeRid(),
            request.targetSpotRid(), actor.stateVersion()));
    }

    public static final class EntryProbeHandler implements ZLinkEntrySpotActorRequestHandler<
        TransferEntrySpot,
        TransferActor,
        Contracts.ProbeReq,
        Contracts.ProbeRes> {
        private final EvidenceStore evidence;
        private final GateStore gates;

        public EntryProbeHandler(EvidenceStore evidence, GateStore gates) {
            this.evidence = evidence;
            this.gates = gates;
        }

        @Override
        public CompletionStage<Contracts.ProbeRes> handle(
            TransferEntrySpot spot,
            TransferActor actor,
            ZLinkMessageContext context,
            Contracts.ProbeReq request) {
            evidence.add(request.scenario(), actor.actorId(), "entry_packet_handler", request.marker());
            return awaitProbeGate(request, actor.actorId(), evidence, gates)
                .thenApply(ignored -> new Contracts.ProbeRes(
                    request.scenario(), actor.actorId(), spot.context().spotId(),
                    evidence.nodeRid(), actor.stateVersion(), request.marker()));
        }
    }

    public static final class ProbeHandler implements ZLinkSpotActorRequestHandler<
        TransferUserSpot,
        TransferActor,
        Contracts.ProbeReq,
        Contracts.ProbeRes> {
        private final EvidenceStore evidence;
        private final GateStore gates;

        public ProbeHandler(EvidenceStore evidence, GateStore gates) {
            this.evidence = evidence;
            this.gates = gates;
        }

        @Override
        public CompletionStage<Contracts.ProbeRes> handle(
            TransferUserSpot spot,
            TransferActor actor,
            ZLinkMessageContext context,
            Contracts.ProbeReq request) {
            evidence.add(request.scenario(), actor.actorId(), "packet_handler", request.marker());
            return awaitProbeGate(request, actor.actorId(), evidence, gates)
                .thenApply(ignored -> new Contracts.ProbeRes(
                    request.scenario(), actor.actorId(), spot.context().spotId(),
                    evidence.nodeRid(), actor.stateVersion(), request.marker()));
        }
    }

    private static CompletionStage<Void> awaitProbeGate(
        Contracts.ProbeReq request,
        String actorId,
        EvidenceStore evidence,
        GateStore gates) {
        String prefix = "gate:";
        if (!request.marker().startsWith(prefix)) {
            return CompletableFuture.completedFuture(null);
        }
        String gate = request.marker().substring(prefix.length());
        evidence.add(request.scenario(), actorId, "probe_gate_wait", gate);
        return gates.waitFor(gate).thenRun(() ->
            evidence.add(
                request.scenario(), actorId, "probe_gate_released", gate));
    }

    public static final class MessageFollowSendHandler implements systems.zlink.framework.spots.ZLinkSpotActorSendHandler<
        TransferUserSpot,
        TransferActor,
        Contracts.MessageFollowSendReq> {
        private final EvidenceStore evidence;

        public MessageFollowSendHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Void> handle(
            TransferUserSpot spot,
            TransferActor actor,
            ZLinkMessageContext context,
            Contracts.MessageFollowSendReq request) {
            evidence.add(
                request.scenario(), actor.actorId(), "message_follow_send", request.marker());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class TransferSession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers;
        private final EvidenceStore evidence;

        public TransferSession(
            ZLinkSessionContext context,
            ZLinkSessionPacketDispatcher<ZLinkSessionContext> handlers,
            EvidenceStore evidence) {
            this.context = context;
            this.handlers = handlers;
            this.evidence = evidence;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            evidence.add("session", context.sessionId(), "connected", "");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            evidence.add("session", context.sessionId(), "disconnected", "");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            evidence.add("session", context.sessionId(), "error", error.toString());
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            return handlers.tryHandle(context, dispatch, payload).thenCompose(handled -> {
                if (handled) {
                    return CompletableFuture.completedFuture(null);
                }
                ZLinkSessionActor actor = context.actors().bound().size() == 1
                    ? context.actors().bound().get(0)
                    : context.actors().find(dispatch.metadata().get("actor-id"))
                        .orElseThrow(() -> new IllegalStateException("actor is not bound"));
                return actor.relay(dispatch, payload);
            });
        }
    }

    public static final class BindSessionHandler implements ZLinkTypedSessionPacketHandler<
        ZLinkSessionContext,
        Contracts.BindSessionReq> {
        private final systems.zlink.framework.actors.ZLinkActorManager actors;
        private final EvidenceStore evidence;

        public BindSessionHandler(
            systems.zlink.framework.actors.ZLinkActorManager actors,
            EvidenceStore evidence) {
            this.actors = actors;
            this.evidence = evidence;
        }

        @Override
        public Class<Contracts.BindSessionReq> messageType() {
            return Contracts.BindSessionReq.class;
        }

        @Override
        public CompletionStage<Void> handle(
            ZLinkSessionContext context,
            ZLinkSessionDispatchContext dispatch,
            Contracts.BindSessionReq request) {
            return actors.find(request.actorId()).thenCompose(found -> {
                var actor = found.orElseThrow(
                    () -> new IllegalStateException("actor was not found: " + request.actorId()));
                return context.actors().bind(actor);
            }).thenApply(bound -> {
                evidence.add(request.scenario(), request.actorId(), "session_bound", context.sessionId());
                context.client().reply(new Contracts.BindSessionRes(
                    request.scenario(), request.actorId(), bound.ref().nodeRid().toString(),
                    bound.ref().objectGeneration())).submit();
                return null;
            });
        }
    }

    public static final class EntryBoundPushHandler implements ZLinkEntrySpotActorRequestHandler<
        TransferEntrySpot,
        TransferActor,
        Contracts.BoundPushReq,
        Contracts.BoundPushRes> {
        private final EvidenceStore evidence;

        public EntryBoundPushHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.BoundPushRes> handle(
            TransferEntrySpot spot,
            TransferActor actor,
            ZLinkMessageContext context,
            Contracts.BoundPushReq request) {
            return push(actor, spot.context().spotId(), request, evidence);
        }
    }

    public static final class BoundPushHandler implements ZLinkSpotActorRequestHandler<
        TransferUserSpot,
        TransferActor,
        Contracts.BoundPushReq,
        Contracts.BoundPushRes> {
        private final EvidenceStore evidence;

        public BoundPushHandler(EvidenceStore evidence) {
            this.evidence = evidence;
        }

        @Override
        public CompletionStage<Contracts.BoundPushRes> handle(
            TransferUserSpot spot,
            TransferActor actor,
            ZLinkMessageContext context,
            Contracts.BoundPushReq request) {
            return push(actor, spot.context().spotId(), request, evidence);
        }
    }

    private static CompletionStage<Contracts.BoundPushRes> push(
        TransferActor actor,
        String spotRid,
        Contracts.BoundPushReq request,
        EvidenceStore evidence) {
        Contracts.BoundPushNotify notify = new Contracts.BoundPushNotify(
            request.scenario(), actor.actorId(), spotRid, evidence.nodeRid(),
            request.marker(), actor.stateVersion());
        CompletionStage<Void> submitted = actor.context().boundSession()
            .send(notify)
            .submit();
        evidence.add(request.scenario(), actor.actorId(), "bound_push", request.marker());
        return submitted.thenApply(ignored -> new Contracts.BoundPushRes(
                request.scenario(), actor.actorId(), spotRid, evidence.nodeRid(),
                request.marker(), actor.stateVersion()));
    }
}
