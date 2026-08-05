package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;
import systems.zlink.e2e.storefailure.client.support.ScenarioAssert;
import systems.zlink.e2e.storefailure.shared.Wait;

public final class SfE1StoreDelayNonBlockingScenario implements ClientScenario {
    private static final int STORE_DELAY_MILLISECONDS = 1200;

    @Override
    public DiscoveryApiResult run(ClientContext context) {
        context.waitForLivePeerRows();
        List<Long> baseline = context.measureRequests("sf-e1-baseline", 10);
        long baselineP99 = ClientContext.percentileMillis(baseline, 0.99);
        context.setStoreDelay(STORE_DELAY_MILLISECONDS);
        try {
            CompletableFuture<Long> delayedStoreRead =
                CompletableFuture.supplyAsync(context::measureStoreRead);
            Wait.sleep(Duration.ofMillis(150));
            List<Long> concurrent = context.measureRequests("sf-e1-concurrent", 12);
            long delayedStoreReadMs = delayedStoreRead.join();
            long concurrentP99 = ClientContext.percentileMillis(concurrent, 0.99);
            long budget = Math.max(baselineP99 * 8, 750);

            ScenarioAssert.that(delayedStoreReadMs >= STORE_DELAY_MILLISECONDS * 0.75,
                "SF-E1 delayed store read finished too quickly: " + delayedStoreReadMs + " ms");
            ScenarioAssert.that(concurrentP99 <= budget,
                "SF-E1 unrelated request p99 grew too much during store delay. baseline="
                    + baselineP99 + " ms concurrent=" + concurrentP99 + " ms budget=" + budget + " ms");

            return context.requestUntilAnyProvider("SF-E1", "sf-e1-recovery", 1);
        } finally {
            context.setStoreDelay(0);
        }
    }
}
