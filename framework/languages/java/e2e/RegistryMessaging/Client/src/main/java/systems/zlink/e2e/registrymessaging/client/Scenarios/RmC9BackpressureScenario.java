package systems.zlink.e2e.registrymessaging.client.Scenarios;

import systems.zlink.e2e.registrymessaging.client.Support.ScenarioAssert;
import systems.zlink.e2e.registrymessaging.client.Support.ScenarioWait;
import systems.zlink.e2e.registrymessaging.shared.Contracts;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class RmC9BackpressureScenario {
    private static final int SLOW_SEND_COUNT = 8;

    private RmC9BackpressureScenario() {
    }

    public static void run(
        ZLinkHttpClient backpressureConsumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB) {
        backpressureConsumer.post("/profile/backpressure/reset").submit(Object.class).toCompletableFuture().join().body();
        String marker = "rm-c9-" + java.util.UUID.randomUUID().toString().replace("-", "");
        java.util.List<java.util.concurrent.CompletableFuture<String>> sends = new java.util.ArrayList<>();
        for (int index = 0; index < SLOW_SEND_COUNT; index++) {
            String commandId = "slow-" + marker + "-" + index;
            sends.add(java.util.concurrent.CompletableFuture.supplyAsync(() ->
                backpressureConsumer.post("/profile/backpressure/send")
                    .body(new Contracts.ProfileMsg(commandId))
                    .submit(Contracts.BackpressureRes.class).toCompletableFuture().join().body()
                    .outcome()));
        }
        java.util.List<String> outcomes = sends.stream()
            .map(java.util.concurrent.CompletableFuture::join)
            .toList();
        ScenarioAssert.that(outcomes.stream().allMatch("Submitted"::equals),
            "RM-C9 expected all one-way sends to be submitted without a public bounded-failure oracle");
        ScenarioWait.sleep(10000);

        String recoveredMarker = marker + "-after";
        Contracts.ProfileRes recovered = requestRecovered(backpressureConsumer, recoveredMarker);
        ScenarioAssert.that(("profile:" + recoveredMarker).equals(recovered.value()),
            "RM-C9 connection did not recover after pressure");
        String[] evidence = ScenarioAssert.waitAnyEvidence(providerA, providerB, recoveredMarker);
        ScenarioAssert.that(java.util.Arrays.stream(evidence).anyMatch(line -> line.contains(recoveredMarker)),
            "RM-C9 recovery evidence missing");
        System.out.println("scenario RM-C9 passed");
    }

    private static Contracts.ProfileRes requestRecovered(
        ZLinkHttpClient backpressureConsumer,
        String marker) {
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(30);
        RuntimeException lastFailure = null;
        while (System.nanoTime() < deadline) {
            try {
                return backpressureConsumer.post("/profile/request")
                    .body(new Contracts.ProfileReq(marker))
                    .timeout(java.time.Duration.ofSeconds(10))
                    .submit(Contracts.ProfileRes.class).toCompletableFuture().join().body();
            } catch (RuntimeException error) {
                lastFailure = error;
                ScenarioWait.sleep(500);
            }
        }
        throw lastFailure == null
            ? new IllegalStateException("RM-C9 recovery request did not run")
            : lastFailure;
    }
}
