package systems.zlink.e2e.resiliencelifecycle.client;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlA1ProviderRestartScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlA2ProviderEndpointRemapScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlA3ReconnectStormScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlA4DrainAndGreenEndpointScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlA5ProviderFlappingScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlB1CancellationCleanupScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlB2CrashDuringInflightScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlB3GracefulShutdownScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlB4RuntimeDrainScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlB5DrainInflightScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlB6GrayFaultScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlC1ClientHostLifecycleScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlC2TopologyRecoveryScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlC4RegistryOutageScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlD2ObserverFaultScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlD3DispatchErrorEvidenceScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlD4MissingRequestHandlerScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Scenarios.RlD5MixedBurstScenario;
import systems.zlink.e2e.resiliencelifecycle.client.Support.ClientOptions;
import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceProcessManager;

public final class ResilienceLifecycleSuite {
    private final ClientOptions options;
    private final ResilienceProcessManager processes;
    private final List<String> startOrder;
    private ResilienceProcessManager.ManagedProcess providerA;
    private ResilienceProcessManager.ManagedProcess providerB;
    private String currentHttpA;

    public ResilienceLifecycleSuite(
        ClientOptions options,
        ResilienceProcessManager processes,
        List<String> startOrder) {
        this.options = options;
        this.processes = processes;
        this.startOrder = List.copyOf(startOrder);
        this.currentHttpA = options.httpAEndpoint();
    }

    public void run() {
        run("all");
    }

    public void run(String scenario) {
        processes.prepareControlDir();
        for (String role : startOrder) {
            if ("api-a".equals(role)) {
                providerA = processes.startProvider(
                    "api-a", "api-a", options.apiAEndpoint(), options.httpAEndpoint());
            } else {
                providerB = processes.startProvider(
                    "api-b", "api-b", options.apiBEndpoint(), options.httpBEndpoint());
            }
        }
        processes.sleep(2_000);

        switch (scenario) {
            case "all" -> {
                runRestart();
                runReschedule();
                runRollingGreen();
                runFlapping();
                runDefault("all");
                restartProviderB();
                runCrashDuringInflight();
                runTopologyRecovery();
                runStoreOutage();
                runStorm();
                runCleanup("all");
            }
            case "RL-A1", "RL-C3" -> runRestart();
            case "RL-A2" -> runReschedule();
            case "RL-A3", "RL-D1" -> runStorm();
            case "RL-A4" -> runRollingGreen();
            case "RL-A5" -> runFlapping();
            case "RL-B2" -> runCrashDuringInflight();
            case "RL-C2" -> runTopologyRecovery();
            case "RL-C4" -> runStoreOutage();
            case "RL-B1", "RL-B3", "RL-B4", "RL-B5", "RL-B6", "RL-D2", "RL-D3", "RL-D4" ->
                runDefault(scenario);
            case "RL-C1", "RL-D5" -> runCleanup(scenario);
            default -> throw new IllegalArgumentException("unknown ResilienceLifecycle scenario: " + scenario);
        }
    }

    private void runRestart() {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-restart", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            CompletableFuture<Void> scenario = CompletableFuture.runAsync(
                () -> RlA1ProviderRestartScenario.run(consumer));
            processes.waitSignal("a1-ready", scenario);
            providerA.close();
            processes.waitEndpointDown("api-a", currentHttpA);
            processes.touchSignal("a1-down");
            processes.waitSignal("a1-down-observed", scenario);
            providerA = processes.startProvider("api-a", "api-a", options.apiAEndpoint(), options.httpAEndpoint());
            processes.sleep(2_000);
            processes.touchSignal("a1-up");
            scenario.join();
        }
    }

    private void runReschedule() {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-reschedule", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            CompletableFuture<Void> scenario = CompletableFuture.runAsync(
                () -> RlA2ProviderEndpointRemapScenario.run(consumer));
            processes.waitSignal("a2-ready", scenario);
            providerA.killForcibly();
            processes.waitEndpointDown("api-a", currentHttpA);
            processes.touchSignal("a2-down");
            processes.waitSignal("a2-down-observed", scenario);
            providerA = processes.startProvider(
                "api-a", "api-a", options.apiAReplacementEndpoint(), options.httpAReplacementEndpoint());
            currentHttpA = options.httpAReplacementEndpoint();
            processes.sleep(2_000);
            processes.touchSignal("a2-up");
            scenario.join();
        }
    }

    private void runFlapping() {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-flapping", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            CompletableFuture<Void> scenario = CompletableFuture.runAsync(
                () -> RlA5ProviderFlappingScenario.run(consumer));
            processes.waitSignal("a5-ready", scenario);
            for (int i = 0; i < 3; i++) {
                providerA.close();
                processes.waitEndpointDown("api-a", currentHttpA);
                processes.sleep(1_000);
                providerA = processes.startProvider(
                    "api-a", "api-a", options.apiAReplacementEndpoint(), options.httpAReplacementEndpoint());
                processes.sleep(2_000);
            }
            processes.touchSignal("a5-stop");
            scenario.join();
        }
    }

    private void runRollingGreen() {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-rolling-green", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            CompletableFuture<Void> scenario = CompletableFuture.runAsync(
                () -> RlA4DrainAndGreenEndpointScenario.run(consumer));
            processes.waitSignal("a4-drained", scenario);
            providerB.close();
            processes.waitEndpointDown("api-b", options.httpBEndpoint());
            processes.touchSignal("a4-original-down");

            var greenProviderB = processes.startProvider(
                "api-b-green", "api-b", options.apiBGreenEndpoint(), options.httpBGreenEndpoint());
            processes.sleep(2_000);
            processes.touchSignal("a4-green-up");
            processes.waitSignal("a4-green-observed", scenario);
            processes.waitSignal("a4-restore-ready", scenario);

            greenProviderB.close();
            processes.waitEndpointDown("api-b-green", options.httpBGreenEndpoint());
            providerB = processes.startProvider("api-b", "api-b", options.apiBEndpoint(), options.httpBEndpoint());
            processes.sleep(2_000);
            processes.touchSignal("a4-restored");
            scenario.join();
        }
    }

    private void runDefault(String scenario) {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-default", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            if ("all".equals(scenario) || "RL-B1".equals(scenario)) {
                RlB1CancellationCleanupScenario.run(consumer);
            }
            if ("all".equals(scenario) || "RL-B4".equals(scenario)) {
                RlB4RuntimeDrainScenario.run(consumer);
            }
            if ("all".equals(scenario) || "RL-B5".equals(scenario)) {
                RlB5DrainInflightScenario.run(consumer);
            }
            if ("all".equals(scenario) || "RL-D2".equals(scenario)) {
                RlD2ObserverFaultScenario.run(consumer);
            }
            if ("all".equals(scenario) || "RL-D3".equals(scenario)) {
                RlD3DispatchErrorEvidenceScenario.run(consumer);
            }
            if ("all".equals(scenario) || "RL-D4".equals(scenario)) {
                RlD4MissingRequestHandlerScenario.run(consumer);
            }
            if ("all".equals(scenario) || "RL-B6".equals(scenario)) {
                RlB6GrayFaultScenario.run(consumer);
            }
            if ("all".equals(scenario) || "RL-B3".equals(scenario)) {
                RlB3GracefulShutdownScenario.run(consumer);
            }
        }
        if ("all".equals(scenario)) {
            providerB.close();
            processes.waitEndpointDown("api-b", options.httpBEndpoint());
        }
    }

    private void runCrashDuringInflight() {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-crash-inflight", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            CompletableFuture<Void> scenario = CompletableFuture.runAsync(
                () -> RlB2CrashDuringInflightScenario.run(consumer));
            processes.waitSignal("b2-in-flight", scenario);
            providerB.killForcibly();
            processes.waitEndpointDown("api-b", options.httpBEndpoint());
            processes.touchSignal("b2-crashed");
            processes.waitSignal("b2-survivor-observed", scenario);
            providerB = processes.startProvider("api-b", "api-b", options.apiBEndpoint(), options.httpBEndpoint());
            processes.sleep(2_000);
            processes.touchSignal("b2-restored");
            scenario.join();
        }
    }

    private void runTopologyRecovery() {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-topology-recovery", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            CompletableFuture<Void> scenario = CompletableFuture.runAsync(
                () -> RlC2TopologyRecoveryScenario.run(consumer));
            processes.waitSignal("c2-ready", scenario);
            providerB.killForcibly();
            processes.waitEndpointDown("api-b", options.httpBEndpoint());
            processes.touchSignal("c2-crashed");
            processes.waitSignal("c2-survivor-observed", scenario);
            providerB = processes.startProvider("api-b", "api-b", options.apiBEndpoint(), options.httpBEndpoint());
            processes.sleep(2_000);
            processes.touchSignal("c2-restored");
            scenario.join();
        }
    }

    private void runStoreOutage() {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-store-outage", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            CompletableFuture<Void> scenario = CompletableFuture.runAsync(
                () -> RlC4RegistryOutageScenario.run(consumer));
            processes.waitSignal("c4-pause-ready", scenario);
            processes.pauseStore();
            processes.touchSignal("c4-store-paused");
            processes.waitSignal("c4-during-observed", scenario);
            processes.unpauseStore();
            processes.touchSignal("c4-store-resumed");
            scenario.join();
        }
    }

    private void restartProviderB() {
        providerB = processes.startProvider("api-b", "api-b", options.apiBEndpoint(), options.httpBEndpoint());
        processes.sleep(3_000);
    }

    private void runStorm() {
        for (int wave = 1; wave <= 2; wave++) {
            List<CompletableFuture<Void>> scenarios = new ArrayList<>();
            List<ResilienceProcessManager.ManagedProcess> consumers = new ArrayList<>();
            for (int index = 1; index <= 6; index++) {
                Path stormLogDir = Path.of(options.logDir(), "storm-" + wave + "-" + index);
                createDirectory(stormLogDir);
                String consumerHttp = processes.reserveHttpEndpoint();
                consumers.add(processes.startConsumer(
                    "consumer-storm-" + wave + "-" + index,
                    consumerHttp,
                    stormLogDir.toString()));
                ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
                scenarios.add(CompletableFuture.runAsync(() -> RlA3ReconnectStormScenario.run(consumer)));
            }
            scenarios.forEach(CompletableFuture::join);
            for (int i = consumers.size() - 1; i >= 0; i--) {
                consumers.get(i).close();
            }
        }
    }

    private void runCleanup(String scenario) {
        String consumerHttp = processes.reserveHttpEndpoint();
        try (var consumerProcess = processes.startConsumer(
            "consumer-cleanup", consumerHttp, options.logDir())) {
            ResilienceScenarioContext consumer = new ResilienceScenarioContext(consumerHttp, currentHttpA, options);
            if ("all".equals(scenario) || "RL-C1".equals(scenario)) {
                RlC1ClientHostLifecycleScenario.run(consumer);
            }
            if ("all".equals(scenario) || "RL-D5".equals(scenario)) {
                RlD5MixedBurstScenario.run(consumer);
            }
        }
    }

    private static void createDirectory(Path path) {
        try {
            Files.createDirectories(path);
        } catch (Exception error) {
            throw new IllegalStateException("failed to create " + path, error);
        }
    }
}
