package systems.zlink.e2e.registrationcodec.client.Scenarios;

import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class ManualRegistrationScenario {
    private ManualRegistrationScenario() {
    }

    public static void run(ScenarioContext context) {
        Contracts.EchoRes manual = context.server().post("/registration/manual").submit(Contracts.EchoRes.class).toCompletableFuture().join().body();
        ScenarioAssert.ensure("echo:manual-request".equals(manual.value()) && "manual".equals(manual.handler()),
            "RC-A3 request mismatch");
        ScenarioAssert.waitForEvidence(context.evidence(), "Send", "EchoManual", "manual-send");
        System.out.println("scenario RC-A3 passed");
    }
}
