package systems.zlink.e2e.spotservice.client.Scenarios;

import java.time.Duration;
import systems.zlink.e2e.spotservice.shared.Contracts;

final class TimeoutScenario extends SpotServiceScenarioContext {
    private TimeoutScenario(SpotServiceScenarioContext context) {
        super(context);
    }

    static void run(SpotServiceScenarioContext context) {
        new TimeoutScenario(context).execute();
    }

    private void execute() {
        expectFailure(() -> requestSlow("room-a", "late", Duration.ofMillis(100)));
        System.out.println("scenario SM-C1-timeout passed");
    }
}
