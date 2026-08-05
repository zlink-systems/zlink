package systems.zlink.e2e.channelegress.role;

import java.time.Duration;
import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.channelegress.shared.Contracts;
import systems.zlink.e2e.channelegress.shared.EvidenceState;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class SpotWorkflowHandler
    implements ZLinkSpotRequestHandler<Config12Spot, Contracts.SpotWorkflowReq, Contracts.SpotWorkflowRes> {
    private final EvidenceState evidence;

    public SpotWorkflowHandler(EvidenceState evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Contracts.SpotWorkflowRes> handle(
        Config12Spot spot,
        Contracts.SpotWorkflowReq request) {
        ArrayList<String> sequence = new ArrayList<>();
        sequence.add("handler-start");
        evidence.add("spot-handler-start", "spot=" + spot.context().spotId() + "|id=" + request.id());
        return spot.context().outbound()
            .requestToChannel(
                Contracts.WORKFLOW_CHANNEL,
                new Contracts.ChannelProbeReq(request.id() + "-workflow"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.ChannelProbeRes.class)
            .thenCompose(ignored -> {
                sequence.add("workflow-reply");
                sequence.add("handler-end");
                evidence.add("spot-workflow-reply", "spot=" + spot.context().spotId() + "|id=" + request.id());
                evidence.add("spot-handler-end", "spot=" + spot.context().spotId() + "|id=" + request.id());
                return spot.startTimer(request.timerName()).thenApply(timerIgnored -> {
                    sequence.add("timer-start");
                    evidence.add(
                        "spot-timer-start",
                        "spot=" + spot.context().spotId() + "|id=" + request.id()
                            + "|sequence=" + String.join(",", sequence));
                    return new Contracts.SpotWorkflowRes(request.id(), sequence);
                });
            });
    }
}

final class SpotWorkflowTimerHandler implements ZLinkSpotTimerHandler<Config12Spot> {
    private final EvidenceState evidence;

    SpotWorkflowTimerHandler(EvidenceState evidence) {
        this.evidence = evidence;
    }

    @Override
    public CompletionStage<Void> handle(Config12Spot spot, ZLinkTimerTick tick) {
        String spotId = spot.context().spotId();
        return spot.context().outbound()
            .requestToChannel(
                Contracts.WORKFLOW_CHANNEL,
                new Contracts.ChannelProbeReq(spotId + "-timer-workflow"))
            .timeout(Duration.ofSeconds(5))
            .submit(Contracts.ChannelProbeRes.class)
            .thenAccept(ignored -> {
                evidence.add("spot-timer-workflow-reply", "spot=" + spotId + "|timer=" + tick.name());
                evidence.add(
                    "spot-timer-end",
                    "spot=" + spotId
                        + "|timer=" + tick.name()
                        + "|sequence=handler-start,workflow-reply,handler-end,timer-start,workflow-reply,timer-end");
                spot.closeTimer();
            });
    }
}
