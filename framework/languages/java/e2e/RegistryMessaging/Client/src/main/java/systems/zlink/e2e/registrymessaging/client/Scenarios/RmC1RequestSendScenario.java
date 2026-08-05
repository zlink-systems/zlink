package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmC1RequestSendScenario {
    private RmC1RequestSendScenario() {
    }

    public static void run(ZLinkHttpClient providerA, ZLinkHttpClient providerB) {
        String requestValue = "rm-c1-request";
        Contracts.ProfileRes reply = providerA.post("/profile/request")
            .body(new Contracts.ProfileReq(requestValue))
            .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
        ScenarioAssert.that(("profile:" + requestValue).equals(reply.value()), "RM-C1 reply mismatch");
        String commandId = "cmd-c1-" + java.util.UUID.randomUUID();
        providerA.post("/profile/command")
            .body(new Contracts.ProfileMsg(commandId))
            .submit(Object.class).toCompletableFuture().join().body();
        String[] evidence = ScenarioAssert.waitAnyEvidence(providerA, providerB, commandId);
        ScenarioAssert.that(java.util.Arrays.stream(evidence)
                .anyMatch(line -> line.contains("ProfileReq") && line.contains(requestValue)),
            "RM-C1 request evidence missing");
        ScenarioAssert.that(java.util.Arrays.stream(evidence)
                .anyMatch(line -> line.contains("ProfileMsg") && line.contains(commandId)),
            "RM-C1 send evidence missing");
        System.out.println("scenario RM-C1 passed");
    }
}
