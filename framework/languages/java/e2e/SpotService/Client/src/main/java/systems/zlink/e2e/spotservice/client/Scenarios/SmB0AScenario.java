package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
import java.util.UUID;
import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmB0AScenario extends SpotServiceScenarioContext {
    private SmB0AScenario(SpotServiceScenarioContext context) { super(context); }

    public static void run(SpotServiceScenarioContext context) { new SmB0AScenario(context).execute(); }

    private void execute() {
        Contracts.ActorCreateRaceRes result = postJson(options().httpAEndpoint(), "/actor/create-race",
            new Contracts.ActorCreateRaceReq("actor-sm-b0a-" + UUID.randomUUID().toString().replace("-", "")),
            Contracts.ActorCreateRaceRes.class);
        ensure("Rejected".equals(result.firstState()) && "rejected:first".equals(result.firstReply())
                && "Created".equals(result.secondState()) && result.secondActor() != null
                && result.finalActor() != null
                && result.secondActor().generation() == result.finalActor().generation(),
            "SM-B0A creation race did not isolate reject and accept results");
        System.out.println("scenario SM-B0A passed");
    }
}
