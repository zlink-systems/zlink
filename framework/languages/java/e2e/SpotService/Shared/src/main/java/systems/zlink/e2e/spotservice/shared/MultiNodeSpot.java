package systems.zlink.e2e.spotservice.shared;

import java.time.Duration;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

public final class MultiNodeSpot implements ZLinkSpot<ScenarioActor> {
    private final ZLinkSpotContext context;
    private final ScenarioState evidence;
    private int value;

    public MultiNodeSpot(
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
        context.handlers().addHandler(MultiNodeStateReqHandler.class);
        context.handlers().addHandler(MultiNodeStateMsgHandler.class);
    }

    @Override
    public java.util.concurrent.CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
        evidence.record("MultiNodeSpotCreated", context.spotId(), request.isEmpty() ? "" : "request");
        if (!request.isEmpty()) {
            Contracts.SpotOnlyMeshReq command = request.decode(Contracts.SpotOnlyMeshReq.class);
            return context.outbound()
                    .requestToSpot(command.targetSpotRid(), new Contracts.MultiNodeStateReq(7))
                    .timeout(Duration.ofSeconds(5))
                    .submit(Contracts.MultiNodeStateRes.class)
                    .thenApply(reply -> {
                        context.outbound().sendToSpot(command.targetSpotRid(),
                            new Contracts.MultiNodeStateMsg("sm-f6-send-" + command.marker())).submit();
                        evidence.record("SpotOnlyRequest", context.spotId(),
                            command.targetSpotRid() + "/" + reply.value() + "/" + command.marker());
                        return ZLinkSpotCreateResponse.accept();
                    });
        }
        return java.util.concurrent.CompletableFuture.completedFuture(ZLinkSpotCreateResponse.accept());
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onInitialize() {
        evidence.record("MultiNodeSpotInitialized", context.spotId(), "");
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
        String actorId,
        ZLinkMessage request) {
        return java.util.concurrent.CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onJoinedActor(ScenarioActor actor) {
        evidence.record("SpotOnlyActorJoined", context.spotId(), actor.actorId());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> onLeaveActor(ScenarioActor actor) {
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    public int add(int delta) {
        value += delta;
        evidence.record("MultiNodeStateReq", context.spotId(), Integer.toString(value));
        return value;
    }

    public void command(String marker) {
        evidence.record("MultiNodeStateMsg", context.spotId(), marker);
    }
}
