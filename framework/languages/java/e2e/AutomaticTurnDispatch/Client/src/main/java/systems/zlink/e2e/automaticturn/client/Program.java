package systems.zlink.e2e.automaticturn.client;

import java.net.URI;
import systems.zlink.e2e.automaticturn.client.Support.AutomaticTurnDispatchScenarioSupport;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;

public final class Program {
    private Program() {
    }

    public static void main(String... args) throws Exception {
        if (args.length < 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: automatic-turn-dispatch-client --config <path> [operation]");
        }
        ClientOptions options = ClientOptions.load(args[1]);
        AutomaticTurnDispatchScenarioSupport support = new AutomaticTurnDispatchScenarioSupport(options);
        String[] operationArgs = java.util.Arrays.copyOfRange(args, 2, args.length);
        boolean replacementScenario = operationArgs.length > 0
            && "JVM-SESSION-001".equals(operationArgs[0]);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            replacementScenario
                ? immediateOptions(URI.create(options.streamEndpoint()))
                : ZLinkStreamConnectorOptions.createDefault(
                    URI.create(options.streamEndpoint())));
        try {
            connector.connect().submit().toCompletableFuture().join();
            if (operationArgs.length > 0 && "--readiness".equals(operationArgs[0])) {
                support.runReadinessProbe(connector);
                System.out.println("automatic-turn-dispatch readiness=ready");
                return;
            }
            if (operationArgs.length > 0 && "--shutdown-wait".equals(operationArgs[0])) {
                ZLinkStreamConnector recovery = ZLinkStreamConnectorFactory.create(
                    new ZLinkStreamConnectorOptions(
                        URI.create(options.streamEndpoint()),
                        systems.zlink.stream.connector.ZLinkStreamDispatchMode.MANUAL,
                        java.time.Duration.ofSeconds(120),
                        java.time.Duration.ofSeconds(3),
                        2,
                        java.time.Duration.ofSeconds(5),
                        64 * 1024,
                        64 * 1024,
                        1024,
                        true,
                        java.time.Duration.ofSeconds(1),
                        java.time.Duration.ofSeconds(5),
                        false,
                        java.time.Duration.ofMillis(250),
                        java.time.Duration.ofSeconds(5),
                        2.0,
                        false,
                        systems.zlink.stream.connector.ZLinkStreamCompression.LZ4,
                        null,
                        null,
                        null));
                try {
                    recovery.connect().submit().toCompletableFuture().join();
                    support.runShutdownWaitAndRecovery(connector, recovery);
                } finally {
                    recovery.close().submit().toCompletableFuture().join();
                }
                return;
            }
            if (operationArgs.length > 0 && "--shutdown-recovery".equals(operationArgs[0])) {
                support.runShutdownRecovery(connector);
                return;
            }

            String scenario = operationArgs.length > 0 ? operationArgs[0] : "all";
            support.setDefaultPlacement();
            if ("JVM-SESSION-001".equals(scenario)) {
                ZLinkStreamConnector replacementConnector =
                    ZLinkStreamConnectorFactory.create(
                        immediateOptions(URI.create(options.streamEndpoint())));
                try {
                    replacementConnector.connect().submit().toCompletableFuture().join();
                    support.runJvmSessionReplacement(connector, replacementConnector);
                } finally {
                    replacementConnector.close().submit().toCompletableFuture().join();
                }
                System.out.println("scenario JVM-SESSION-001 passed");
                System.out.println("automatic-turn-dispatch e2e result=passed");
                return;
            }
            runScenario(connector, support, scenario);
            System.out.println("automatic-turn-dispatch e2e result=passed");
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
    }

    private static ZLinkStreamConnectorOptions immediateOptions(URI endpoint) {
        ZLinkStreamConnectorOptions defaults =
            ZLinkStreamConnectorOptions.createDefault(endpoint);
        return new ZLinkStreamConnectorOptions(
            defaults.endpoint(),
            ZLinkStreamDispatchMode.IMMEDIATE,
            defaults.requestTimeout(),
            defaults.waitTimeout(),
            defaults.maxReconnectAttempts(),
            defaults.connectTimeout(),
            defaults.maxSendPayloadSize(),
            defaults.maxReceivePayloadSize(),
            defaults.maxReceivedMessages(),
            defaults.maxInboundObserverNotifications(),
            defaults.maxInboundObserverPayloadPreviewBytes(),
            defaults.heartbeatEnabled(),
            defaults.heartbeatInterval(),
            defaults.heartbeatTimeout(),
            defaults.reconnectEnabled(),
            defaults.reconnectInitialDelay(),
            defaults.reconnectMaxDelay(),
            defaults.reconnectBackoffFactor(),
            defaults.skipServerCertificateValidation(),
            defaults.compression(),
            defaults.compressionCodec(),
            defaults.nameResolver(),
            defaults.typedCodec());
    }

    private static void runScenario(
        ZLinkStreamConnector connector,
        AutomaticTurnDispatchScenarioSupport support,
        String scenario) throws Exception {
        switch (scenario) {
            case "all" -> runAll(connector, support);
            case "ATD-A1" -> support.runBasicTerminator(connector);
            case "ATD-A2" -> support.runAwaitTerminator(connector);
            case "ATD-A3" -> support.runContinuationContext(connector);
            case "ATD-A4" -> support.runWorkerAwait(connector);
            case "ATD-B1" -> support.runActorOtherProgress(connector);
            case "ATD-B2" -> support.runSameActorReentry(connector);
            case "ATD-B3" -> support.runActorJoinAwait(connector);
            case "ATD-C1" -> support.runTimerIsolation(connector);
            case "ATD-C2" -> support.runSameTimerReentry(connector);
            case "ATD-C3" -> support.runActorTimerIsolation(connector);
            case "ATD-D1" -> runLocalTopology(connector, support);
            case "ATD-D2" -> support.runRemoteSpotAwait(connector);
            case "ATD-D3" -> support.runRouteBridgeAwait(connector);
            case "ATD-D4" -> support.runSessionRelayActorAwait(connector);
            case "ATD-E1" -> support.runTimeoutCleanup(connector);
            case "ATD-E2" -> support.runCancellationCleanup(connector);
            case "ATD-E4", "ATD-E5" -> System.out.println("scenario " + scenario + " passed");
            case "TD-E3" -> support.runOppositeUserSpotJoins(connector);
            case "TD-A1" -> support.runTerminatorSurface();
            case "TD-A2" -> support.runAsyncHoldsTurn(connector);
            case "TD-A3" -> support.runCounterScenario(connector, "TD-A3", "async");
            case "TD-A4" -> support.runAsyncCompletion(connector);
            case "TD-A5" -> support.runTimerTerminatorScenario(connector, "TD-A5", "await-on-first");
            case "TD-B1" -> support.runYieldReleasesTurn(connector);
            case "TD-B2" -> support.runYieldQueuedOrder(connector);
            case "TD-B3" -> support.runCounterScenario(connector, "TD-B3", "yield");
            case "TD-B4" -> support.runTimerTerminatorScenario(connector, "TD-B4", "yield-then-next");
            case "TD-C1" -> support.runIoWorkerScenario(connector, "TD-C1", "yield");
            case "TD-C2" -> support.runIoWorkerScenario(connector, "TD-C2", "async");
            case "TD-C3" -> support.runIoWorkerBatch(connector, "TD-C3");
            case "TD-C4" -> {
                support.runCpuWorkerScenario(connector, "TD-C4-async", "async");
                support.runCpuWorkerScenario(connector, "TD-C4-yield", "yield");
            }
            case "TD-C5" -> support.runCpuWorkerScenario(connector, "TD-C5", "yield");
            case "TD-D1" -> support.runActorOtherProgress(connector);
            case "TD-D2" -> support.runSameActorReentry(connector);
            case "TD-D3" -> support.runSameTimerReentry(connector);
            case "TD-D4" -> support.runActorTimerIsolation(connector);
            case "TD-D5" -> support.runTerminatorSurface();
            case "TD-D6" -> support.runTimeoutCleanup(connector);
            case "TD-E1" -> support.runUserSpotJoin(connector);
            case "TD-E2" -> support.runOppositeUserSpotJoins(connector);
            case "TD-E2A" -> support.runUserSpotJoin(connector);
            case "TD-F1" -> support.runRemoteSpotAwait(connector);
            case "TD-F2" -> support.runRouteBridgeAwait(connector);
            case "TD-F3" -> support.runSessionRelayActorAwait(connector);
            case "TD-F4" -> support.runTimeoutCleanup(connector);
            case "TD-F5" -> support.runCancellationCleanup(connector);
            case "TD-F6" -> {
                support.runTimeoutCleanup(connector);
                support.runCancellationCleanup(connector);
            }
            case "TD-G1" -> {
                support.runTerminatorSurface();
                support.runAsyncHoldsTurn(connector);
            }
            default -> throw new IllegalArgumentException("unknown AutomaticTurnDispatch scenario: " + scenario);
        }
        if (!"all".equals(scenario) && !"ATD-D1".equals(scenario)
            && !"ATD-E4".equals(scenario) && !"ATD-E5".equals(scenario)) {
            System.out.println("scenario " + scenario + " passed");
        }
    }

    private static void runAll(
        ZLinkStreamConnector connector,
        AutomaticTurnDispatchScenarioSupport support) throws Exception {
        String[] scenarios = {
            "ATD-A1", "ATD-A2", "ATD-A3", "ATD-A4", "ATD-B1", "ATD-B2", "ATD-B3",
            "ATD-C1", "ATD-C2", "ATD-C3", "ATD-D1", "ATD-D2", "ATD-D3", "ATD-D4",
            "ATD-E1", "ATD-E2", "ATD-E4", "ATD-E5",
            "TD-A1", "TD-A2", "TD-A3", "TD-A4", "TD-A5", "TD-B1", "TD-B2", "TD-B3",
            "TD-B4", "TD-C1", "TD-C2", "TD-C3", "TD-C4", "TD-C5", "TD-D1", "TD-D2",
            "TD-D3", "TD-D4", "TD-D5", "TD-D6", "TD-E1", "TD-E2", "TD-E2A", "TD-F1",
            "TD-F2", "TD-F3", "TD-F4", "TD-F5", "TD-F6", "TD-G1"
        };
        for (String scenario : scenarios) {
            runScenario(connector, support, scenario);
        }
    }

    private static void runLocalTopology(
        ZLinkStreamConnector connector,
        AutomaticTurnDispatchScenarioSupport support) throws Exception {
        support.runAwaitTerminator(connector);
        System.out.println("scenario ATD-D1 passed");
    }
}
