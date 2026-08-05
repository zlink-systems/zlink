package systems.zlink.e2e.kotlin.automaticturn;

public final class ClientApplication {
    private ClientApplication() {
    }

    public static void run(String... args) {
        String scenario = args.length > 0 ? args[0] : "all";
        if ("d2".equals(scenario) || "ATD-D2".equals(scenario) || "ATD-D3".equals(scenario)) {
            ClientScenario.runD2(scenario);
        } else if ("ATD-E3".equals(scenario)) {
            ClientScenario.runE3();
        } else {
            ClientScenario.run(scenario);
        }
        System.out.println("automatic-turn-dispatch kotlin e2e result=passed");
        System.exit(0);
    }
}
