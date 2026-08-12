package systems.zlink.e2e.registrationcodec.client.Scenarios;

import java.util.Arrays;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrationcodec.client.Support.ScenarioContext;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class RcB6JsonGoldenScenario {
    private RcB6JsonGoldenScenario() {
    }

    public static void run(ScenarioContext context) {
        Contracts.JsonGoldenRes result = context.server()
            .post("/codec/json-golden")
            .submit(Contracts.JsonGoldenRes.class)
            .toCompletableFuture()
            .join()
            .body();
        ScenarioAssert.ensure("Ada Lovelace".equals(result.displayName()),
            "RC-B6 display name changed");
        ScenarioAssert.ensure("ready".equals(result.status()), "RC-B6 status changed");
        ScenarioAssert.ensure(result.balance() == -9_223_372_036_854_775_000L,
            "RC-B6 int64 value changed");
        ScenarioAssert.ensure(Arrays.equals(result.payload(),
                new byte[] {0x00, 0x7f, (byte) 0x80, (byte) 0xff}),
            "RC-B6 bytes value changed");
        ScenarioAssert.ensure(result.score() == 2_147_000_001, "RC-B6 int32 value changed");
        ScenarioAssert.ensure(Double.compare(result.ratio(), 0.125) == 0,
            "RC-B6 floating-point value changed");
        ScenarioAssert.ensure(result.optionalNote() == null, "RC-B6 nullable value changed");
        ScenarioAssert.ensure("application/json".equals(result.contentType()),
            "RC-B6 did not use default JSON: " + result.contentType());
        ScenarioAssert.waitForEvidence(context.evidence(), "ContentType", "JsonGoldenReq", "application/json");
        System.out.println("scenario RC-B6 passed");
    }
}
