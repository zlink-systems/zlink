package systems.zlink.e2e.runtimemonitoring.client;

import systems.zlink.e2e.runtimemonitoring.client.Scenarios.MonA1SocketEventsScenario;
import systems.zlink.e2e.runtimemonitoring.client.Scenarios.MonA2LocationEventsScenario;
import systems.zlink.e2e.runtimemonitoring.client.Scenarios.MonA3SpotEventsScenario;
import systems.zlink.e2e.runtimemonitoring.client.Scenarios.MonA4AvailabilityTransitionScenario;
import systems.zlink.e2e.runtimemonitoring.client.Scenarios.MonA5FixedKindsScenario;
import systems.zlink.e2e.runtimemonitoring.client.Scenarios.MonBPublishMonitoringAbsenceScenario;
import systems.zlink.e2e.runtimemonitoring.client.Scenarios.MonD1FailureRecoveryScenario;
import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;

public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        if (args.length != 4 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()) {
            throw new IllegalArgumentException(
                "Usage: runtime-monitoring-client --config <path> --scenario <selector>");
        }
        ClientOptions options = ClientOptions.load(args[1]);
        try (MonitoringScenarioContext context = new MonitoringScenarioContext(options)) {
            String scenario = args[3];
            if (!"all".equals(scenario)) {
                if (!"MON-A1".equals(scenario)) {
                    context.restartServiceB();
                    context.waitForPort(
                        context.serviceBEndpoint(),
                        true,
                        scenario + " service-b did not start");
                }
                runOne(scenario, context);
                System.out.println("monitoring e2e result=passed");
                return;
            }
            MonA1SocketEventsScenario.run(context);
            MonA2LocationEventsScenario.run(context);
            MonA3SpotEventsScenario.run(context);
            MonA4AvailabilityTransitionScenario.runReplacement(context, "MON-A4A");
            MonA4AvailabilityTransitionScenario.runCrashReplacement(context, "MON-A4B");
            MonA5FixedKindsScenario.run(context);
            MonBPublishMonitoringAbsenceScenario.runZeroTarget(context);
            MonBPublishMonitoringAbsenceScenario.runLocalTarget(context);
            MonD1FailureRecoveryScenario.runUnknownMesh(context, "MON-D1A");
            MonD1FailureRecoveryScenario.runRepeated(context, "MON-D1B");
            System.out.println("monitoring e2e result=passed");
        }
    }

    private static void runOne(String scenario, MonitoringScenarioContext context) {
        switch (scenario) {
            case "MON-A1" -> MonA1SocketEventsScenario.run(context);
            case "MON-A2" -> MonA2LocationEventsScenario.run(context);
            case "MON-A3" -> MonA3SpotEventsScenario.run(context);
            case "MON-A4" -> MonA4AvailabilityTransitionScenario.run(context);
            case "MON-A4A" -> MonA4AvailabilityTransitionScenario.runReplacement(context, "MON-A4A");
            case "MON-A4B" -> MonA4AvailabilityTransitionScenario.runCrashReplacement(context, "MON-A4B");
            case "MON-A5" -> MonA5FixedKindsScenario.run(context);
            case "MON-B1" -> MonBPublishMonitoringAbsenceScenario.runZeroTarget(context);
            case "MON-B2" -> MonBPublishMonitoringAbsenceScenario.runLocalTarget(context);
            case "MON-D1" -> MonD1FailureRecoveryScenario.run(context);
            case "MON-D1A" -> MonD1FailureRecoveryScenario.runUnknownMesh(context, "MON-D1A");
            case "MON-D1B" -> MonD1FailureRecoveryScenario.runRepeated(context, "MON-D1B");
            default -> throw new IllegalArgumentException("Unknown RuntimeMonitoring scenario '" + scenario + "'.");
        }
    }
}
