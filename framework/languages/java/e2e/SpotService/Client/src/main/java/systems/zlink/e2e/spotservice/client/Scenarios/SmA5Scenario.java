package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA5Scenario extends SpotServiceScenarioContext {
    private SmA5Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmA5Scenario(context).execute();
    }

    private void execute() {
        String endpointA = options().httpAEndpoint();
        String spotRid = "spot-sm-a5-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.CreateSpotRes created = postJson(
            endpointA,
            "/spot/create",
            new Contracts.CreateSpotReq(spotRid),
            Contracts.CreateSpotRes.class);
        ensure(spotRid.equals(created.spotRid()), "SM-A5 created spot mismatch");
        ensure("play-a".equals(created.nodeRid()), "SM-A5 created spot owner mismatch");

        Contracts.StateRes ready = postJson(
            endpointA,
            "/spot/state/request",
            new Contracts.SpotStateRouteReq(spotRid, "sm-a5-ready", 0),
            Contracts.StateRes.class);
        ensure(spotRid.equals(ready.spotRid()), "SM-A5 route readiness spot mismatch");
        ensure("play-a".equals(ready.nodeRid()), "SM-A5 route readiness owner mismatch");

        Contracts.StateRes stageReply = postJson(
            endpointA,
            "/spot/stage/request",
            new Contracts.SpotStageProbeRouteReq(spotRid, "sm-a5-stage", 9),
            Contracts.StateRes.class);
        ensure(spotRid.equals(stageReply.spotRid()), "SM-A5 stage request spot mismatch");
        ensure("play-a".equals(stageReply.nodeRid()), "SM-A5 stage request owner mismatch");
        ensure(stageReply.value().contains("sm-a5-stage-9"), "SM-A5 stage request state mismatch");

        Contracts.StageTimerStartRes timer = postJson(
            endpointA,
            "/spot/stage/timer",
            new Contracts.SpotStageTimerRouteReq(spotRid, "sm-a5-stage-timer", 50),
            Contracts.StageTimerStartRes.class);
        ensure(spotRid.equals(timer.spotRid()) && timer.started(), "SM-A5 stage timer did not start");

        waitForPlayAEvidence(List.of(
            "SpotInitialized|play-a|" + spotRid,
            "StageRequest|play-a|" + spotRid + "|sm-a5-stage|9",
            "StageTimer|play-a|" + spotRid + "|sm-a5-stage-timer"));
        closeSpot(spotRid);
        waitForPlayAEvidence(List.of("SpotClosing|play-a|" + spotRid));
        System.out.println("scenario SM-A5 passed");

    }
}
