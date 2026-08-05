package systems.zlink.e2e.spotservice.client;

import systems.zlink.e2e.spotservice.client.Scenarios.ScenarioSuite;
import systems.zlink.e2e.spotservice.client.Scenarios.SpotServiceScenarioContext;

public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        Inputs inputs = parseInputs(args);
        ClientOptions options = ClientOptions.load(inputs.configPath());
        ScenarioSuite.run(inputs.scenario(), new SpotServiceScenarioContext(options));
        System.out.println("spot-service e2e mode=" + inputs.scenario() + " result=passed");
    }

    private static Inputs parseInputs(String[] args) {
        if (args.length != 4 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()) {
            throw new IllegalArgumentException("Usage: spot-service-client --config <path> --scenario <selector>");
        }
        return new Inputs(args[1], args[3]);
    }

    private record Inputs(String configPath, String scenario) { }
}
