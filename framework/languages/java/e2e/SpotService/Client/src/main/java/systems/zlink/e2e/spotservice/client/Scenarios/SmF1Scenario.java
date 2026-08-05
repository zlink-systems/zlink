package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmF1Scenario extends SpotServiceScenarioContext {
    private SmF1Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmF1Scenario(context).execute();
    }

    private void execute() {
        Contracts.RouteRes routeReply = requestRoute("play-a", "route-mesh-normal");
        ensure("play-a".equals(routeReply.nodeRid()), "SM-F3 route-channel target node mismatch");
        ensure("client-route-mesh".equals(routeReply.routeRid()), "SM-F3 route-channel source routing id mismatch");
        ensure("route:route-mesh-normal".equals(routeReply.value()), "SM-F3 route-channel reply mismatch");

        Contracts.StateRes reply = eventually(() -> requestState("room-a", "route-mesh", REQUEST_TIMEOUT));
        ensure("play-a".equals(reply.nodeRid()), "SM-F2 route mesh target mismatch");
        sendState("room-a", "mixed-route-send");
        System.out.println("scenario SM-F1 passed");
        System.out.println("scenario SM-F2 passed");
        System.out.println("scenario SM-F3 passed");
        expectFailure(() -> requestState("missing-route", "missing-route", Duration.ofMillis(300)));
        System.out.println("scenario SM-F4-missing-route passed");

    }
}
