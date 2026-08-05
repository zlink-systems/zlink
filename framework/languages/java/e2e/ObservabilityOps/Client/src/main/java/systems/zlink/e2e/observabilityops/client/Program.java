package systems.zlink.e2e.observabilityops.client;

import java.net.URI;
import systems.zlink.e2e.observabilityops.trigger.TriggerOperations;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;

public final class Program {
    private Program() {
    }

    public static void main(String... args) throws Exception {
        if (args.length != 4 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()) {
            throw new IllegalArgumentException(
                "Usage: observability-ops-client --config <path> --scenario <selector>");
        }
        ClientOptions options = ClientOptions.load(args[1]);
        ObservabilityScenarioSupport support = new ObservabilityScenarioSupport(options);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(
                URI.create(options.streamEndpoint())));
        try {
            connector.connect().submit().toCompletableFuture().join();
            run(connector, support, options, args[3]);
            System.out.println("observability-ops client scenario=" + args[3] + " result=passed");
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
    }

    private static void run(
        ZLinkStreamConnector connector,
        ObservabilityScenarioSupport support,
        ClientOptions options,
        String scenario) throws Exception {
        switch (scenario) {
            case "OBS-A1", "OBS-A3" -> support.runSessionRelayActorAwait(connector);
            case "OBS-A2" -> TriggerOperations.runMissingHandler(options.streamEndpoint());
            case "OBS-A4", "OBS-B3" -> support.runTimerIsolation(connector);
            case "OBS-B1" -> TriggerOperations.runMetricsB1(
                options.streamEndpoint(), options.requiredScenarioOutput());
            case "OBS-B2" -> support.runObservabilityTransfer(connector);
            case "OBS-B2-QUEUE" -> support.runObservabilityQueue(connector);
            case "OBS-B4" -> TriggerOperations.runReaderFreeB4(
                options.streamEndpoint(), options.requiredScenarioOutput());
            case "OBS-C1" -> support.runBasicTerminator(connector);
            case "OBS-C2" -> support.runObservabilityDrainHandoff(connector);
            case "OBS-C3-WRITE" -> support.runPersistentRoomWrite(connector);
            case "OBS-C3-READ" -> support.runPersistentRoomRead(connector);
            case "OBS-C4" -> TriggerOperations.runDrainWatch(
                options.streamEndpoint(), options.requiredDrainUrl(), options.requiredScenarioOutput());
            case "OBS-C5-BIND" -> support.runDrainRolloutBind(connector);
            case "OBS-C5-PROBE" -> support.runDrainTargetProbe(connector);
            default -> throw new IllegalArgumentException(
                "unknown ObservabilityOps client scenario: " + scenario);
        }
    }
}
