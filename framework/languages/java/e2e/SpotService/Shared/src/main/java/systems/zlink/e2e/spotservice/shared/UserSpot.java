package systems.zlink.e2e.spotservice.shared;

import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

public final class UserSpot implements ZLinkSpot<ScenarioActor> {
    private final ZLinkSpotContext context;
    private final ScenarioState evidence;
    private String state = "";
    private boolean workerDone = true;
    private CountDownLatch workerFollowUp;
    private final Map<String, Contracts.ActorProfile> pendingProfiles = new HashMap<>();

    public UserSpot(
        ZLinkSpotContext context,
        ScenarioState evidence) {
        this.context = context;
        this.evidence = evidence;
    }

    @Override
    public ZLinkSpotContext context() {
        return context;
    }

    @Override
    public void configure() {
        context.handlers().addHandler(StateReqHandler.class);
        context.handlers().addHandler(StateMsgHandler.class);
        context.handlers().addHandler(StageProbeReqHandler.class);
        context.handlers().addHandler(StageTimerStartReqHandler.class);
        context.handlers().addHandler(SlowReqHandler.class);
        context.handlers().addHandler(OutboundReqHandler.class);
        context.handlers().addHandler(OutboundMsgHandler.class);
        context.handlers().addHandler(SpotEventHandler.class);
        context.handlers().addHandler(UserActorEchoHandler.class);
        context.handlers().addHandler(UserActorLeaveHandler.class);
    }

    @Override
    public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        evidence.record("SpotCreated", context.spotId(), request.isEmpty() ? "" : "request");
        context.addTimer("state-timer", Duration.ofSeconds(2),
            StateTimerHandler.class, null);
        return CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    @Override
    public CompletionStage<Void> onInitialize() {
        evidence.record("SpotInitialized", context.spotId(), "");
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onClosing() {
        evidence.record("SpotClosing", context.spotId(), state);
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        Contracts.JoinAdmittedUserSpotActorReq admission = decodeAdmission(request);
        if (admission != null) {
            if (!admission.admit()) {
                evidence.record("ActorUserJoinRejected", context.spotId(),
                    actorId + "/" + admission.reason());
                return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject(new Contracts.ActorJoinRes(
                    actorId,
                    context.spotId(),
                    evidence.nodeRid(),
                    admission.profile().displayName(),
                    admission.profile().level(),
                    admission.tags())));
            }
            pendingProfiles.put(actorId, admission.profile());
            evidence.record("ActorUserJoinAdmitted", context.spotId(),
                actorId + "/" + admission.reason());
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept(new Contracts.ActorJoinRes(
                actorId,
                context.spotId(),
                evidence.nodeRid(),
                admission.profile().displayName(),
                admission.profile().level(),
                admission.tags())));
        }
        Contracts.ActorJoinReq join = request.decode(Contracts.ActorJoinReq.class);
        pendingProfiles.put(actorId, join.profile());
        evidence.record("ActorUserJoinRequested", context.spotId(),
            actorId + "/" + join.profile().displayName() + "/" + String.join(",", join.tags()));
        return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept(new Contracts.ActorJoinRes(
            actorId,
            context.spotId(),
            evidence.nodeRid(),
            join.profile().displayName(),
            join.profile().level(),
            join.tags())));
    }

    private static Contracts.JoinAdmittedUserSpotActorReq decodeAdmission(ZLinkMessage request) {
        try {
            Contracts.JoinAdmittedUserSpotActorReq admission =
                request.decode(Contracts.JoinAdmittedUserSpotActorReq.class);
            if (!admission.admit() && admission.reason() == null) {
                return null;
            }
            return admission;
        } catch (RuntimeException ignored) {
            return null;
        }
    }

    @Override
    public CompletionStage<Void> onJoinedActor(ScenarioActor actor) {
        Contracts.ActorProfile profile = pendingProfiles.remove(actor.actorId());
        if (profile == null) {
            throw new IllegalStateException("joined actor does not have a pending admission");
        }
        actor.applyProfile(profile);
        evidence.record("ActorUserJoined", context.spotId(),
            actor.actorId() + "#" + actor.nextSequence());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onLeaveActor(ScenarioActor actor) {
        evidence.record("ActorUserLeft", context.spotId(), actor.actorId());
        return CompletableFuture.completedFuture(null);
    }

    @Override
    public CompletionStage<Void> onDisconnectActor(ScenarioActor actor) {
        evidence.record("ActorUserDisconnected", context.spotId(), actor.actorId());
        return CompletableFuture.completedFuture(null);
    }

    public String apply(String op) {
        state = state.isBlank() ? op : state + "," + op;
        evidence.record("StateReq", context.spotId(), state);
        if (op.equals("worker-follow-up") && !workerDone) {
            evidence.record("WorkerFollowUpBeforeComplete", context.spotId(), state);
            CountDownLatch latch = workerFollowUp;
            if (latch != null) {
                latch.countDown();
            }
        }
        return state;
    }

    public String startWorker(String op) {
        workerDone = false;
        workerFollowUp = new CountDownLatch(1);
        evidence.record("WorkerStarted", context.spotId(), op);
        CountDownLatch latch = workerFollowUp;
        context.runCpuWorker(cancellation -> {
            latch.await(5, TimeUnit.SECONDS);
            return op + "-done";
        }).submit().whenComplete((value, error) -> {
            if (error == null) {
                workerDone = true;
                workerFollowUp = null;
                state = state.isBlank() ? value : state + "," + value;
                evidence.record("WorkerCompleted", context.spotId(), value);
            } else {
                workerFollowUp = null;
                evidence.record(
                    "WorkerFailed",
                    context.spotId(),
                    error.getClass().getSimpleName());
            }
        });
        return state;
    }

    public void command(String value) {
        evidence.record("StateMsg", context.spotId(), value);
    }

    public void record(String marker, String value) {
        evidence.record(marker, context.spotId(), value);
    }

    public ScenarioStage stage() {
        return new ScenarioStage(this);
    }

    String spotRid() {
        return context.spotId();
    }

    String nodeRid() {
        return evidence.nodeRid();
    }

    public void timerTick(long deliveryIndex) {
        evidence.record("SpotTimer", context.spotId(), Long.toString(deliveryIndex));
    }
}
