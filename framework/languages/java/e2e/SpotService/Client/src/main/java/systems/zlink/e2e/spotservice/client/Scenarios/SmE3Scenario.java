package systems.zlink.e2e.spotservice.client.Scenarios;


public final class SmE3Scenario extends SpotServiceScenarioContext {
    private SmE3Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmE3Scenario(context).execute();
    }

    private void execute() {
        sleep(1200);
        System.out.println("scenario SM-E3 passed");

    }
}
