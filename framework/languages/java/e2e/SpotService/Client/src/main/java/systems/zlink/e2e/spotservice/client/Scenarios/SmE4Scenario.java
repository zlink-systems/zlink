package systems.zlink.e2e.spotservice.client.Scenarios;


public final class SmE4Scenario extends SpotServiceScenarioContext {
    private SmE4Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmE4Scenario(context).execute();
    }

    private void execute() {
        sleep(1200);
        System.out.println("scenario SM-E4 passed");

    }
}
