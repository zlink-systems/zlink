package systems.zlink.e2e.storefailure.client.scenarios;

import java.time.Duration;
import systems.zlink.e2e.storefailure.client.support.ClientContext;
import systems.zlink.e2e.storefailure.client.support.ClientScenario;
import systems.zlink.e2e.storefailure.client.support.DiscoveryApiResult;

public final class SfB2RecoveredScenario implements ClientScenario {
    @Override
    public DiscoveryApiResult run(ClientContext context) {
        context.waitForRecoveredStatus("SF-B2");
        context.waitForReadyPeerRow(
            context.options().deadRid(),
            Duration.ofMillis(context.options().locationHeartbeatMillis() * 10));
        return context.requestUntilAnyProvider();
    }
}
