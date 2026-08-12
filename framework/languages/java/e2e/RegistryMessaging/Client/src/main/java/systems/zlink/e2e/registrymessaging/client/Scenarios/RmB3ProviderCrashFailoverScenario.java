package systems.zlink.e2e.registrymessaging.client.Scenarios;

import java.util.Arrays;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.registrymessaging.client.Support.ClientOptions;
import systems.zlink.e2e.registrymessaging.client.Support.DynamicClusterLauncher;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmB3ProviderCrashFailoverScenario {
    private RmB3ProviderCrashFailoverScenario() {
    }

    public static void run(ClientOptions options) {
        try (DynamicClusterLauncher cluster = DynamicClusterLauncher.start(options)) {
            DynamicClusterLauncher.DynamicProvider providerA =
                cluster.startApiProvider("api-a", "api-a");
            DynamicClusterLauncher.DynamicConsumer consumer = cluster.startConsumer("consumer");
            try (ZLinkHttpClient requester = ZLinkHttpClient.create(consumer.httpUrl()).build();
                 ZLinkHttpClient providerAHttp = ZLinkHttpClient.create(providerA.httpUrl()).build()) {
                cluster.waitPeerCount(requester, 1);
                providerAHttp.post("/profile/gate/reset").submitRaw().toCompletableFuture().join();

                String inFlightValue = "rm-b3-inflight-" + Long.toUnsignedString(System.nanoTime());
                CompletionStage<Contracts.ProfileCallRes> inFlight = requester
                    .post("/profile/request/outcome")
                    .body(new Contracts.ProfileReq(inFlightValue))
                    .submit(Contracts.ProfileCallRes.class)
                    .thenApply(response -> response.body());
                ScenarioAssert.waitEvidence(providerAHttp, "ProfileReqStarted|rid=api-a|value=" + inFlightValue);

                DynamicClusterLauncher.DynamicProvider providerB =
                    cluster.startApiProvider("api-b", "api-b");
                cluster.waitPeerCount(requester, 2);
                providerA.process().crash();

                Contracts.ProfileCallRes transition = inFlight.toCompletableFuture().join();
                ScenarioAssert.that(transition.failed()
                        && isFinitePublicFailure(transition.errorKind()),
                    "RM-B3 in-flight request did not finish with one bounded public terminal: "
                        + transition.errorKind());
                try (ZLinkHttpClient providerBClient = ZLinkHttpClient.create(providerB.httpUrl()).build()) {
                    String[] providerBEvidenceBefore = ScenarioAssert.evidence(providerBClient);
                    ScenarioAssert.that(Arrays.stream(providerBEvidenceBefore)
                            .noneMatch(line -> line.contains(inFlightValue)),
                        "RM-B3 in-flight request was replayed on provider B");
                }

                cluster.waitPeerCount(requester, 1);
                for (int index = 0; index < 20; index++) {
                    String value = "rm-b3-after-" + index;
                    Contracts.ProfileCallRes reply = requester
                        .post("/profile/request/outcome")
                        .body(new Contracts.ProfileReq(value))
                        .submit(Contracts.ProfileCallRes.class)
                        .toCompletableFuture().join().body();
                    ScenarioAssert.that(!reply.failed() && "api-b".equals(reply.providerRid()),
                        "RM-B3 new request did not use provider B: " + value + " -> " + reply);
                }
                try (ZLinkHttpClient providerBClient = ZLinkHttpClient.create(providerB.httpUrl()).build()) {
                    String[] providerBEvidence = ScenarioAssert.evidence(providerBClient);
                    long handled = Arrays.stream(providerBEvidence)
                        .filter(line -> line.contains("ProfileReq|rid=api-b|value=rm-b3-after-"))
                        .count();
                    ScenarioAssert.that(handled == 20,
                        "RM-B3 provider B evidence count was " + handled + ", expected 20");
                    ScenarioAssert.that(Arrays.stream(providerBEvidence)
                            .noneMatch(line -> line.contains(inFlightValue)),
                        "RM-B3 provider B contains the crashed request marker");
                }
            }
        }
        System.out.println("scenario RM-B3 passed");
    }

    private static boolean isFinitePublicFailure(String errorKind) {
        return errorKind != null && (errorKind.equals("UNAVAILABLE")
            || errorKind.equals("DEADLINE_EXCEEDED")
            || errorKind.equals("INTERNAL_FAILURE")
            || errorKind.equals("TimeoutException")
            || errorKind.equals("Timeout"));
    }
}
