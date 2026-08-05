package systems.zlink.e2e.registrationcodec.client.Scenarios;

import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class RcB2ProtobufCodecScenario {
    private RcB2ProtobufCodecScenario() {
    }

    public static void run(ScenarioContext context) {
        String protobufValue = context.server()
            .post("/codec/roundtrip")
            .submit(Contracts.CodecScenarioRes.class).toCompletableFuture().join().body()
            .protobufValue();
        ScenarioAssert.ensure("echo:protobuf-request".equals(protobufValue),
            "RC-B2 request mismatch");
        ScenarioAssert.waitForEvidence(
            context.evidence(),
            "ContentType",
            "ProtobufEcho",
            "application/x-protobuf");
        ScenarioAssert.waitForEvidence(context.evidence(), "Send", "ProtobufEcho", "protobuf-send");
        System.out.println("scenario RC-B2 passed");
    }
}
