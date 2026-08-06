package systems.zlink.e2e.spotservice.client.Scenarios;

import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmB11Scenario extends SpotServiceScenarioContext {
    private SmB11Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) { new SmB11Scenario(context).execute(); }

    private void execute() {
        String actorId = "actor-sm-b11-" + UUID.randomUUID().toString().replace("-", "");
        postJson(options().httpAEndpoint(), "/actor/b11/start", new Contracts.ActorRefReq(actorId),
            Contracts.GateControlRes.class);
        Contracts.ActorRequestRes held = postJson(options().httpAEndpoint(), "/actor/request",
            new Contracts.ActorRequestReq(actorId, "held", 200), Contracts.ActorRequestRes.class);
        ensure(!held.succeeded(), "SM-B11 held Actor was routable before factory release");
        postJson(options().httpAEndpoint(), "/actor/b11/release", new Contracts.ActorRefReq(actorId),
            Contracts.GateControlRes.class);
        Contracts.ActorRequestRes released = eventually(() ->
            postJson(options().httpAEndpoint(), "/actor/request",
                new Contracts.ActorRequestReq(actorId, "released", 2_000), Contracts.ActorRequestRes.class));
        ensure(released.succeeded() && actorId.equals(released.actorId())
                && "released".equals(released.value()), "SM-B11 Actor did not become routable after release");
        System.out.println("scenario SM-B11 passed");
    }
}
