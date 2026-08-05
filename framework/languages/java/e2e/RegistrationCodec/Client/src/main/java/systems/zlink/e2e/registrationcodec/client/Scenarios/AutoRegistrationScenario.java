package systems.zlink.e2e.registrationcodec.client.Scenarios;

import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class AutoRegistrationScenario {
    private AutoRegistrationScenario() {
    }

    public static void run(ScenarioContext context) {
        Contracts.EchoRes auto = context.server().post("/registration/auto").submit(Contracts.EchoRes.class).toCompletableFuture().join().body();
        ScenarioAssert.ensure("echo:auto-request".equals(auto.value()) && "auto".equals(auto.handler()),
            "RC-A1 request mismatch");
        ScenarioAssert.waitForEvidence(context.evidence(), "Send", "EchoAuto", "auto-send");
        System.out.println("scenario RC-A1 passed");
    }
}
