package systems.zlink.e2e.registrationcodec.client.Scenarios;

import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class RcB3MessagePackCodecScenario {
    private RcB3MessagePackCodecScenario() {
    }

    public static void run(ScenarioContext context) {
        String packedValue = context.server()
            .post("/codec/roundtrip")
            .submit(Contracts.CodecScenarioRes.class).toCompletableFuture().join().body()
            .messagePackValue();
        ScenarioAssert.ensure("echo:msgpack-request".equals(packedValue),
            "RC-B3 request mismatch");
        ScenarioAssert.waitForEvidence(
            context.evidence(),
            "ContentType",
            "MsgpackEcho",
            "application/x-msgpack");
        ScenarioAssert.waitForEvidence(context.evidence(), "Send", "MsgpackEcho", "msgpack-send");
        System.out.println("scenario RC-B3 passed");
    }
}
