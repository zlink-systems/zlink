package systems.zlink.framework.perf.client;

import com.fasterxml.jackson.databind.JsonNode;

import systems.zlink.framework.perf.EndpointManifest;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PerfJson;
import systems.zlink.framework.perf.PerfTriggerRequest;
import systems.zlink.framework.perf.ResetRequest;
import systems.zlink.framework.perf.RoleConfig;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;

/** Shared standalone client control pipe; each language supplies only its CS workload implementation. */
public final class ClientControl {
    public interface Workload extends AutoCloseable {
        CompletionStage<Void> prepare();
        CompletionStage<Void> run();
        @Override void close();
    }

    @FunctionalInterface
    public interface ScenarioFactory {
        Workload create(EndpointManifest manifest, Measurement measurement, int index, String scenarioName);
    }

    private ClientControl() {}

    public static void run(EndpointManifest manifest, int index, ScenarioFactory scenarioFactory) throws Exception {
        PrintStream control = System.out;
        System.setOut(System.err); // stdout carries typed control JSON only
        boolean cs = manifest.roles().stream().anyMatch(role -> role.streamEndpoint() != null);
        String scenarioName = manifest.cellId().split("/")[0];
        RoleConfig config = new RoleConfig(manifest.runId(), manifest.cellId(), manifest.configHash(), manifest.language(), "client", index,
                scenarioName, "request", "ordinary", null, null, null, Map.of(), null, "", "", false, "None", true, null,
                List.of(), List.of(), null, null, null, "Immediate", manifest.workload(), null, manifest.provenance());
        Measurement measurement = new Measurement(config, cs);
        try (Workload scenario = cs ? scenarioFactory.create(manifest, measurement, index, scenarioName) : null) {
            if (scenario != null) {
                scenario.prepare().toCompletableFuture().join();
            }
            Map<String, Object> prepared = new LinkedHashMap<>();
            prepared.put("type", "prepared");
            prepared.put("ok", !measurement.hasErrors());
            prepared.put("snapshot", measurement.snapshot(null).toMap());
            emit(control, prepared);
            BufferedReader input = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.UTF_8));
            String line;
            while ((line = input.readLine()) != null) {
                try {
                    JsonNode root = PerfJson.mapper().readTree(line);
                    String command = root.get("command").asText();
                    Object response;
                    switch (command) {
                        case "start" -> response = measurement.start(request(root, PerfTriggerRequest.class),
                                scenario == null ? null : scenario::run);
                        case "reset" -> response = measurement.reset(request(root, ResetRequest.class), null);
                        case "wait" -> {
                            measurement.phaseTask().join();
                            Map<String, Object> waited = new LinkedHashMap<>();
                            waited.put("ok", !measurement.hasErrors());
                            waited.put("phase", measurement.phase());
                            response = waited;
                        }
                        case "stats" -> response = measurement.snapshot(null).toMap();
                        case "stop" -> {
                            return;
                        }
                        default -> throw new IllegalArgumentException("Unknown control command.");
                    }
                    Map<String, Object> reply = new LinkedHashMap<>();
                    reply.put("ok", true);
                    reply.put("response", response);
                    emit(control, reply);
                } catch (Exception error) {
                    measurement.recordDiagnostic(error);
                    Map<String, Object> failure = new LinkedHashMap<>();
                    failure.put("ok", false);
                    failure.put("errorType", error.getClass().getName());
                    failure.put("message", error.getMessage());
                    emit(control, failure);
                }
            }
        }
    }

    private static <T> T request(JsonNode root, Class<T> type) throws IOException {
        return PerfJson.mapper().treeToValue(root.get("request"), type);
    }

    private static void emit(PrintStream control, Object value) {
        control.println(PerfJson.write(value));
        control.flush();
    }
}
