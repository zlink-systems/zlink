package systems.zlink.e2e.kotlin.automaticturn;

import java.util.ArrayList;
import java.time.Duration;
import java.util.List;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdA1BasicTerminatorScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdA2AwaitTerminatorScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdA3ContinuationContextScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdA4WorkerAwaitScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdB1OtherActorProgressScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdB2SameActorReentryScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdB3ActorJoinAwaitScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdC1TimerIsolationScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdC2TimerReentryScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdC3ActorTimerIsolationScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdD1LocalTopologyScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdD2RemoteSpotAwaitScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdD3RouteBridgeAwaitScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdD4SessionRelayActorAwaitScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdE1TimeoutScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdE2CancellationScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.AtdE3ShutdownRecoveryScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.TdEJoinScenario;
import systems.zlink.e2e.kotlin.automaticturn.scenarios.TdBasicTurnScenario;
import systems.zlink.e2e.kotlin.automaticturn.support.ClientStreamSupport;
import systems.zlink.e2e.kotlin.automaticturn.support.ScenarioAssert;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class ClientScenario {
    private ClientScenario() {
    }

    public static void run(String scenario) {
        if ("ATD-C1".equals(scenario) || "ATD-C2".equals(scenario) || "ATD-E1".equals(scenario)
            || "TD-E2".equals(scenario) || "TD-E3".equals(scenario)) {
            runSingleConnectorScenario(scenario);
            return;
        }
        ZLinkStreamConnector roomA = ClientStreamSupport.createConnector();
        ZLinkStreamConnector roomB = ClientStreamSupport.createConnector();
        try {
            ClientStreamSupport.awaitLifecycle(() -> roomA.connect().submit().toCompletableFuture().join());
            ClientStreamSupport.awaitLifecycle(() -> roomB.connect().submit().toCompletableFuture().join());
            AtdB1OtherActorProgressScenario.JoinedActors actors =
                AtdB1OtherActorProgressScenario.run(roomA, roomB);
            if ("ATD-B1".equals(scenario)) {
                return;
            }
            runScenario(scenario, roomA, roomB, actors);
        } finally {
            ScenarioAssert.lifecycle(() -> roomA.close().submit().toCompletableFuture().join());
            ScenarioAssert.lifecycle(() -> roomB.close().submit().toCompletableFuture().join());
        }
    }

    private static void runSingleConnectorScenario(String scenario) {
        ZLinkStreamConnector connector = ClientStreamSupport.createConnector();
        try {
            ClientStreamSupport.awaitLifecycle(() -> connector.connect().submit().toCompletableFuture().join());
            switch (scenario) {
                case "ATD-C1" -> AtdC1TimerIsolationScenario.run(connector);
                case "ATD-C2" -> AtdC2TimerReentryScenario.run(connector);
                case "ATD-E1" -> AtdE1TimeoutScenario.run(connector);
                case "TD-E2" -> TdEJoinScenario.runUserSpotJoin(connector);
                case "TD-E3" -> TdEJoinScenario.runOppositeUserSpotJoins(connector);
                default -> throw new IllegalArgumentException("unknown AutomaticTurnDispatch single-connector scenario: " + scenario);
            }
        } finally {
            ScenarioAssert.lifecycle(() -> connector.close().submit().toCompletableFuture().join());
        }
    }

    private static void runScenario(
        String scenario,
        ZLinkStreamConnector roomA,
        ZLinkStreamConnector roomB,
        AtdB1OtherActorProgressScenario.JoinedActors actors) {
        switch (scenario) {
            case "all" -> runAll(roomA, roomB, actors);
            case "ATD-A1" -> AtdA1BasicTerminatorScenario.run(roomA, actors.actorA());
            case "ATD-A2" -> {
                AtdA1BasicTerminatorScenario.Result a1 = AtdA1BasicTerminatorScenario.run(roomA, actors.actorA());
                AtdA2AwaitTerminatorScenario.run(roomA, actors.actorA(), a1);
            }
            case "ATD-A3" -> AtdA3ContinuationContextScenario.run(roomA, actors.actorA(), roomB, actors.actorB());
            case "ATD-A4" -> AtdA4WorkerAwaitScenario.run(roomA, actors.actorA());
            case "ATD-B2" -> AtdB2SameActorReentryScenario.run(roomA, actors.actorA());
            case "ATD-B3" -> AtdB3ActorJoinAwaitScenario.run(roomA, actors.actorA(), roomB, actors.actorB());
            case "ATD-C1" -> AtdC1TimerIsolationScenario.run(roomA);
            case "ATD-C2" -> AtdC2TimerReentryScenario.run(roomA);
            case "ATD-C3" -> AtdC3ActorTimerIsolationScenario.run(roomA);
            case "ATD-D1" -> {
                List<String> completed = runLocalTopologyScenarios(roomA, roomB, actors);
                AtdD1LocalTopologyScenario.run(roomA, completed);
            }
            case "ATD-D4" -> runD4(roomA, actors.actorA());
            case "ATD-E1" -> AtdE1TimeoutScenario.run(roomA);
            case "ATD-E2" -> AtdE2CancellationScenario.run(roomA);
            case "ATD-E3" -> runE3Scenario(roomA);
            case "TD-A1" -> TdBasicTurnScenario.runSurface();
            case "TD-A2" -> TdBasicTurnScenario.runAsyncHoldsTurn(roomA);
            case "TD-A4" -> TdBasicTurnScenario.runAsyncCompletion(roomA);
            case "TD-B1" -> TdBasicTurnScenario.runYieldInterleave(roomA);
            case "TD-B2" -> TdBasicTurnScenario.runYieldQueuedOrder(roomA);
            default -> throw new IllegalArgumentException("unknown AutomaticTurnDispatch scenario: " + scenario);
        }
    }

    private static void runAll(
        ZLinkStreamConnector roomA,
        ZLinkStreamConnector roomB,
        AtdB1OtherActorProgressScenario.JoinedActors actors) {
        List<String> completed = runLocalTopologyScenarios(roomA, roomB, actors);
        AtdE2CancellationScenario.run(roomA);
        AtdD1LocalTopologyScenario.run(roomA, completed);
    }

    private static List<String> runLocalTopologyScenarios(
        ZLinkStreamConnector roomA,
        ZLinkStreamConnector roomB,
        AtdB1OtherActorProgressScenario.JoinedActors actors) {
        List<String> completed = new ArrayList<>();
        completed.add("ATD-B1");
        AtdA1BasicTerminatorScenario.Result a1 = AtdA1BasicTerminatorScenario.run(roomA, actors.actorA());
        completed.add("ATD-A1");
        AtdA2AwaitTerminatorScenario.run(roomA, actors.actorA(), a1);
        completed.add("ATD-A2");
        AtdA3ContinuationContextScenario.run(roomA, actors.actorA(), roomB, actors.actorB());
        completed.add("ATD-A3");
        AtdA4WorkerAwaitScenario.run(roomA, actors.actorA());
        completed.add("ATD-A4");
        AtdB2SameActorReentryScenario.run(roomA, actors.actorA());
        completed.add("ATD-B2");
        AtdB3ActorJoinAwaitScenario.run(roomA, actors.actorA(), roomB, actors.actorB());
        completed.add("ATD-B3");
        AtdC1TimerIsolationScenario.run(roomA);
        completed.add("ATD-C1");
        AtdC2TimerReentryScenario.run(roomA);
        completed.add("ATD-C2");
        AtdC3ActorTimerIsolationScenario.run(roomA);
        completed.add("ATD-C3");
        runD4(roomA, actors.actorA());
        completed.add("ATD-D4");
        AtdE1TimeoutScenario.run(roomA);
        completed.add("ATD-E1");
        return completed;
    }

    private static void runD4(
        ZLinkStreamConnector roomA,
        String actorId) {
        ZLinkStreamConnector unbound = ClientStreamSupport.createConnector();
        try {
            ClientStreamSupport.awaitLifecycle(() -> unbound.connect().submit().toCompletableFuture().join());
            AtdD4SessionRelayActorAwaitScenario.run(roomA, actorId, unbound);
        } catch (Exception error) {
            throw new IllegalStateException("ATD-D4 scenario failed", error);
        } finally {
            ScenarioAssert.lifecycle(() -> unbound.close().submit().toCompletableFuture().join());
        }
    }

    public static void runD2(String scenario) {
        ZLinkStreamConnector connector = ClientStreamSupport.createConnector();
        try {
            ClientStreamSupport.awaitLifecycle(() -> connector.connect().submit().toCompletableFuture().join());
            switch (scenario) {
                case "d2" -> {
                    AtdD2RemoteSpotAwaitScenario.run(connector);
                    AtdD3RouteBridgeAwaitScenario.run(connector);
                }
                case "ATD-D2" -> AtdD2RemoteSpotAwaitScenario.run(connector);
                case "ATD-D3" -> AtdD3RouteBridgeAwaitScenario.run(connector);
                default -> throw new IllegalArgumentException("unknown AutomaticTurnDispatch scenario: " + scenario);
            }
        } finally {
            ScenarioAssert.lifecycle(() -> connector.close().submit().toCompletableFuture().join());
        }
    }

    public static void runE3() {
        ZLinkStreamConnector connector = ClientStreamSupport.createConnector();
        ZLinkStreamConnector recovery = ClientStreamSupport.createConnector(Duration.ofSeconds(120));
        try {
            ClientStreamSupport.awaitLifecycle(() -> connector.connect().submit().toCompletableFuture().join());
            ClientStreamSupport.awaitLifecycle(() -> recovery.connect().submit().toCompletableFuture().join());
            AtdE3ShutdownRecoveryScenario.run(connector, recovery);
        } finally {
            ScenarioAssert.lifecycle(() -> recovery.close().submit().toCompletableFuture().join());
            ScenarioAssert.lifecycle(() -> connector.close().submit().toCompletableFuture().join());
        }
    }

    private static void runE3Scenario(ZLinkStreamConnector connector) {
        ZLinkStreamConnector recovery = ClientStreamSupport.createConnector(Duration.ofSeconds(120));
        try {
            ClientStreamSupport.awaitLifecycle(() -> recovery.connect().submit().toCompletableFuture().join());
            AtdE3ShutdownRecoveryScenario.run(connector, recovery);
        } finally {
            ScenarioAssert.lifecycle(() -> recovery.close().submit().toCompletableFuture().join());
        }
    }
}
