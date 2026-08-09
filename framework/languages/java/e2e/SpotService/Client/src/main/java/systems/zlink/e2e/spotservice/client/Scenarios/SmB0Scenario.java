package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmB0Scenario extends SpotServiceScenarioContext {
    private SmB0Scenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) { new SmB0Scenario(context).execute(); }

    private void execute() {
        String actorId = "actor-sm-b0-" + UUID.randomUUID().toString().replace("-", "");
        Contracts.ActorManagerProbeRes missing = postJson(options().httpAEndpoint(), "/actor/manager-probe",
            new Contracts.ActorManagerProbeReq("find", actorId), Contracts.ActorManagerProbeRes.class);
        ensure("Missing".equals(missing.state()) && missing.actor() == null,
            "SM-B0 missing Find returned an actor");
        Contracts.ActorManagerProbeRes created = postJson(options().httpAEndpoint(), "/actor/manager-probe",
            new Contracts.ActorManagerProbeReq("create", actorId), Contracts.ActorManagerProbeRes.class);
        Contracts.ActorManagerProbeRes existing = postJson(options().httpAEndpoint(), "/actor/manager-probe",
            new Contracts.ActorManagerProbeReq("get-or-create", actorId), Contracts.ActorManagerProbeRes.class);
        ensure("Created".equals(created.state()) && "Existing".equals(existing.state())
                && created.actor().generation() == existing.actor().generation()
                && created.actor().nodeRid().equals(existing.actor().nodeRid()),
            "SM-B0 Create/GetOrCreate did not converge on one Actor");
        System.out.println("scenario SM-B0 passed");
    }
}
