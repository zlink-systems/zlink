package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

final class SendScenario extends SpotServiceScenarioContext {
    private SendScenario(SpotServiceScenarioContext context) {
        super(context);
    }

    static void run(SpotServiceScenarioContext context) {
        new SendScenario(context).execute();
    }

    private void execute() {
        sendState("room-a", "cmd-c1");
        System.out.println("scenario SM-C1-send passed");
    }
}
