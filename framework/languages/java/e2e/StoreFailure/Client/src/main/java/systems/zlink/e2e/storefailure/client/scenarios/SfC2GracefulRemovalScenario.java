package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import java.time.Instant;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;
import systems.zlink.e2e.storefailure.client.support.ScenarioAssert;
import systems.zlink.e2e.storefailure.shared.Wait;

public final class SfC2GracefulRemovalScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        Instant started = Instant.now();
        context.waitForPeerRowsExcluding(
            context.options().deadRid(),
            Duration.ofMillis(context.options().locationLeaseTtlMillis()));
        Duration elapsed = Duration.between(started, Instant.now());
        ScenarioAssert.that(
            elapsed.compareTo(Duration.ofMillis(context.options().locationLeaseTtlMillis())) < 0,
            "SF-C2 row removal did not beat owner lease TTL: " + elapsed);
        Wait.sleep(Duration.ofMillis(context.options().locationPollingMillis() * 4));
        return context.requestSurvivorOnly();
    }
}
