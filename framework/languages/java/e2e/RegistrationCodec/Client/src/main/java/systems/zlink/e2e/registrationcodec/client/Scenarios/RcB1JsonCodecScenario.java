package systems.zlink.e2e.registrationcodec.client.Scenarios;

import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class RcB1JsonCodecScenario {
    private RcB1JsonCodecScenario() {
    }

    public static void run(ScenarioContext context) {
        Contracts.EchoRes jsonReply = context.server()
            .post("/codec/roundtrip")
            .submit(Contracts.CodecScenarioRes.class).toCompletableFuture().join().body()
            .json();
        ScenarioAssert.ensure("echo:json-request".equals(jsonReply.value()) && "json".equals(jsonReply.handler()),
            "RC-B1 request mismatch");
        ScenarioAssert.waitForEvidence(context.evidence(), "ContentType", "JsonEcho", "application/json");
        ScenarioAssert.waitForEvidence(context.evidence(), "Send", "JsonEcho", "json-send");
        System.out.println("scenario RC-B1 passed");
    }
}
