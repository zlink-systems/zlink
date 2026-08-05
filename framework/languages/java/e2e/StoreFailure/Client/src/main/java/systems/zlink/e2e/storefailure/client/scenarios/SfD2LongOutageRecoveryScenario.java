package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;
import systems.zlink.e2e.storefailure.client.support.ScenarioAssert;

public final class SfD2LongOutageRecoveryScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        Duration recoveryWindow = Duration.ofMillis(
            context.options().locationLeaseTtlMillis() * 2
                + context.options().locationHeartbeatMillis() * 4);
        DiscoveryApiResult traffic = context.driveTolerantRequests(
            "sf-d2", recoveryWindow, "SF-D2");
        ScenarioAssert.that(traffic.providers().stream().anyMatch("api-a"::equals),
            "SF-D2 no request was served by the surviving provider");
        context.waitForLivePeerRows();
        context.waitForPeerRowsExcluding(context.options().deadRid(), recoveryWindow);
        return context.requestSurvivorOnly("SF-D2", "sf-d2-after", 8);
    }
}
