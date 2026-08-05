package systems.zlink.e2e.registrationcodec.client.Scenarios;

import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class RcB4CodecCoexistenceScenario {
    private RcB4CodecCoexistenceScenario() {
    }

    public static void run(ScenarioContext context) {
        context.server().post("/codec/roundtrip").submit(Contracts.CodecScenarioRes.class).toCompletableFuture().join().body();
        ScenarioAssert.waitForEvidence(context.evidence(), "ContentType", "JsonEcho", "application/json");
        ScenarioAssert.waitForEvidence(
            context.evidence(),
            "ContentType",
            "ProtobufEcho",
            "application/x-protobuf");
        ScenarioAssert.waitForEvidence(
            context.evidence(),
            "ContentType",
            "MsgpackEcho",
            "application/x-msgpack");
        System.out.println("scenario RC-B4 passed");
    }
}
