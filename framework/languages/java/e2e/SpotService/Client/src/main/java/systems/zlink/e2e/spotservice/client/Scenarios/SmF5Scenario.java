package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmF5Scenario extends SpotServiceScenarioContext {
    private SmF5Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmF5Scenario(context).execute();
    }

    private void execute() {
        Contracts.RouteRes before = requestRoute("play-a", "sm-f5-before");
        ensure("play-a".equals(before.nodeRid()), "SM-F5 pre-close route-channel target node mismatch");
        ensure("client-route-mesh".equals(before.routeRid()), "SM-F5 pre-close source routing id mismatch");
        ensure("route:sm-f5-before".equals(before.value()), "SM-F5 pre-close route-channel reply mismatch");

        Contracts.StateRes routed = eventually(() -> requestState("room-a", "sm-f5-route", REQUEST_TIMEOUT));
        ensure("room-a".equals(routed.spotRid()), "SM-F5 routed spot rid mismatch");
        ensure("play-a".equals(routed.nodeRid()), "SM-F5 routed spot owner mismatch");

        closeSpot("room-a");

        Contracts.RouteRes after = requestRoute("play-a", "sm-f5-after");
        ensure("play-a".equals(after.nodeRid()), "SM-F5 post-close route-channel target node mismatch");
        ensure("client-route-mesh".equals(after.routeRid()), "SM-F5 post-close source routing id mismatch");
        ensure("route:sm-f5-after".equals(after.value()), "SM-F5 post-close route-channel reply mismatch");

        waitForPlayAEvidence(List.of(
            "RouteReq|play-a|client-route-mesh|sm-f5-before",
            "StateReq|play-a|room-a|sm-f5-route",
            "SpotClosing|play-a|room-a",
            "RouteReq|play-a|client-route-mesh|sm-f5-after"));
        System.out.println("scenario SM-F5 passed");

    }
}
