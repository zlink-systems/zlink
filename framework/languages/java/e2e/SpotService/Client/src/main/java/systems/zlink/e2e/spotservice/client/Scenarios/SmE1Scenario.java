package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmE1Scenario extends SpotServiceScenarioContext {
    private SmE1Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmE1Scenario(context).execute();
    }

    private void execute() {
        String ownerEndpoint = options().httpAEndpoint();
        String spotRid = "spot-sm-e1-" + java.util.UUID.randomUUID().toString().replace("-", "");
        Contracts.CreateSpotRes created = postJson(
            ownerEndpoint,
            "/spot/create",
            new Contracts.CreateSpotReq(spotRid),
            Contracts.CreateSpotRes.class);
        ensure(spotRid.equals(created.spotRid()) && "play-a".equals(created.nodeRid()),
            "SM-E1 spot was not created on play-a");
        Contracts.StateRes ready = postJson(
            ownerEndpoint,
            "/spot/state/request",
            new Contracts.SpotStateRouteReq(spotRid, "sm-e1-ready", 0),
            Contracts.StateRes.class);
        ensure(spotRid.equals(ready.spotRid()) && "play-a".equals(ready.nodeRid()),
            "SM-E1 spot route was not ready on play-a");
        Contracts.SpotMissingHandlerRes missingRequest = postJson(
            ownerEndpoint,
            "/spot/missing-handler/request",
            new Contracts.SpotMissingHandlerReq(spotRid),
            Contracts.SpotMissingHandlerRes.class);
        ensure(missingRequest.failed(), "SM-E1 missing handler request did not fail");
        Contracts.SpotMissingCommandRes missingCommand = postJson(
            ownerEndpoint,
            "/spot/missing-handler/command",
            new Contracts.SpotMissingCommandReq(spotRid, "missing-command"),
            Contracts.SpotMissingCommandRes.class);
        ensure(missingCommand.sent(), "SM-E1 missing handler command was not sent");
        waitForEvidence(
            ownerEndpoint,
            List.of(
                "DispatchError|play-a|" + spotRid + "|HANDLER_MISSING/REPLY_ERROR/MissingSpotReq",
                "DispatchError|play-a|" + spotRid + "|HANDLER_MISSING/DROP/MissingSpotMsg"));
        System.out.println("scenario SM-C1-negative passed");
        System.out.println("scenario SM-E1 passed");

    }
}
