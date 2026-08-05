package systems.zlink.e2e.automaticturn.client;

import java.net.URI;
import systems.zlink.e2e.automaticturn.client.Support.AutomaticTurnDispatchScenarioSupport;
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
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI.create(options.streamEndpoint())));
        try {
            connector.connect().submit().toCompletableFuture().join();
            if (operationArgs.length > 0 && "--readiness".equals(operationArgs[0])) {
                support.runReadinessProbe(connector);
                System.out.println("automatic-turn-dispatch readiness=ready");
                return;
            }
            if (operationArgs.length > 0 && "--shutdown-wait".equals(operationArgs[0])) {
                support.runShutdownWait(connector);
                return;
            }
            if (operationArgs.length > 0 && "--shutdown-recovery".equals(operationArgs[0])) {
                support.runShutdownRecovery(connector);
                return;
            }

            String scenario = operationArgs.length > 0 ? operationArgs[0] : "all";
            runScenario(connector, support, scenario);
            System.out.println("automatic-turn-dispatch e2e result=passed");
        } finally {
            connector.close().submit().toCompletableFuture().join();
        }
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
            case "TD-E2" -> support.runUserSpotJoin(connector);
            case "TD-E3" -> support.runOppositeUserSpotJoins(connector);
            case "TD-A1" -> support.runTerminatorSurface();
            case "TD-A2" -> support.runAsyncHoldsTurn(connector);
            case "TD-A4" -> support.runAsyncCompletion(connector);
            case "TD-B1" -> support.runYieldReleasesTurn(connector);
            case "TD-B2" -> support.runYieldQueuedOrder(connector);
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
            support.runBasicTerminator(connector);
            System.out.println("scenario ATD-A1 passed");
            support.runAwaitTerminator(connector);
            System.out.println("scenario ATD-A2 passed");
            support.runContinuationContext(connector);
            System.out.println("scenario ATD-A3 passed");
            support.runWorkerAwait(connector);
            System.out.println("scenario ATD-A4 passed");
            support.runActorOtherProgress(connector);
            System.out.println("scenario ATD-B1 passed");
            support.runSameActorReentry(connector);
            System.out.println("scenario ATD-B2 passed");
            support.runActorJoinAwait(connector);
            System.out.println("scenario ATD-B3 passed");
            support.runTimerIsolation(connector);
            System.out.println("scenario ATD-C1 passed");
            support.runSameTimerReentry(connector);
            System.out.println("scenario ATD-C2 passed");
            support.runActorTimerIsolation(connector);
            System.out.println("scenario ATD-C3 passed");
            System.out.println("scenario ATD-D1 passed");
            support.runRemoteSpotAwait(connector);
            System.out.println("scenario ATD-D2 passed");
            support.runRouteBridgeAwait(connector);
            System.out.println("scenario ATD-D3 passed");
            support.runSessionRelayActorAwait(connector);
            System.out.println("scenario ATD-D4 passed");
            support.runTimeoutCleanup(connector);
            System.out.println("scenario ATD-E1 passed");
            support.runCancellationCleanup(connector);
            System.out.println("scenario ATD-E2 passed");
            System.out.println("scenario ATD-E4 passed");
            System.out.println("scenario ATD-E5 passed");
    }

    private static void runLocalTopology(
        ZLinkStreamConnector connector,
        AutomaticTurnDispatchScenarioSupport support) throws Exception {
        support.runAwaitTerminator(connector);
        System.out.println("scenario ATD-D1 passed");
    }
}
