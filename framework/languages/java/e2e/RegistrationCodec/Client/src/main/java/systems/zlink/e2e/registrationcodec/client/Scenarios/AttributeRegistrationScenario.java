package systems.zlink.e2e.registrationcodec.client.Scenarios;

import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class AttributeRegistrationScenario {
    private AttributeRegistrationScenario() {
    }

    public static void run(ScenarioContext context) {
        Contracts.EchoRes attr = context.server().post("/registration/attribute").submit(Contracts.EchoRes.class).toCompletableFuture().join().body();
        ScenarioAssert.ensure("echo:attr-request".equals(attr.value()) && "attr".equals(attr.handler()),
            "RC-A2 request mismatch");
        ScenarioAssert.waitForEvidence(context.evidence(), "Send", "EchoAttr", "attr-send");
        System.out.println("scenario RC-A2 passed");
    }
}
