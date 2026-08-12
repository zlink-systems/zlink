package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;
import java.util.ArrayList;

import com.fasterxml.jackson.databind.JsonNode;
import java.time.Duration;
import java.util.List;
import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

/**
 * Runs the public evidence that is available before a Config 5 Track E/F
 * scenario is blocked by a missing fixture capability.
 */
public final class CommonScenarioGapScenario {
    private static final List<String> SCENARIOS = List.of(
        "RL-E1", "RL-E2", "RL-E3", "RL-E4", "RL-E5",
        "RL-F1", "RL-F3", "RL-F5", "RL-F6", "RL-F7",
        "RL-F8", "RL-F9", "RL-F10", "RL-F11", "RL-F12", "RL-F13", "RL-F14");

    private CommonScenarioGapScenario() {
    }

    public static boolean supports(String scenario) {
        return SCENARIOS.contains(scenario);
    }

    public static void run(String scenario, ResilienceScenarioContext context) {
        ResilienceScenarioContext.ensure(supports(scenario), "unknown common scenario " + scenario);
        context.waitForTopology(2);
        Contracts.PeerSnapshot topologyBefore = context.topologySnapshot();
        JsonNode capabilities = context.readCapabilities(context.adminA());

        if ("RL-E1".equals(scenario)) {
            runOrderlyDisconnectEvidence(context, topologyBefore, capabilities);
            return;
        }

        List<String> reasons = missingCapabilities(scenario, capabilities);
        if ("RL-E3".equals(scenario) || "RL-E4".equals(scenario)) {
            reasons = append(reasons, "old physical connection loss cannot be isolated by the Java fixture");
        }
        if ("RL-F3".equals(scenario)) {
            reasons = append(reasons, "a cross-language peer fixture is not configured");
        }
        if (reasons.isEmpty()) {
            reasons = List.of("the requested common scenario has no Java public fixture path");
        }

        System.out.println("scenario " + scenario + " blocked: "
            + "topologyBefore.peers=" + topologyBefore.peers().size()
            + " capabilities=" + capabilities
            + " reason=" + String.join(", ", reasons));
    }

    private static void runOrderlyDisconnectEvidence(
        ResilienceScenarioContext context,
        Contracts.PeerSnapshot topologyBefore,
        JsonNode capabilities) {
        context.post(context.adminB() + "/admin/shutdown");
        context.waitForHttpUnavailable(context.adminB());
        context.waitForTopologyWithout("api-b", 10);
        Contracts.PeerSnapshot topologyAfter = context.topologySnapshot();

        String value = "rl-e1-surviving";
        Contracts.WorkRes reply = context.request(value, Duration.ofSeconds(3));
        ResilienceScenarioContext.ensure("api-a".equals(reply.providerRid()),
            "RL-E1 surviving request was not handled by api-a: " + reply.providerRid());
        context.waitForEvidenceValue(context.adminA(), "WorkReq", value);
        ResilienceScenarioContext.ensure(!capabilities.path("routeMeshConfigured").asBoolean(false),
            "RL-E1 unexpectedly has a RouteMesh target fixture");

        System.out.println("scenario RL-E1 blocked: "
            + "topologyBefore.peers=" + topologyBefore.peers().size()
            + " provider-api-b-admin-shutdown=accepted"
            + " provider-api-b-health=unavailable"
            + " topologyAfter.peers=" + topologyAfter.peers().size()
            + " surviving-provider=" + reply.providerRid()
            + " surviving-evidence=api-a"
            + " reason=RouteMesh target fixture is not configured");
    }

    private static List<String> missingCapabilities(String scenario, JsonNode capabilities) {
        ArrayList<String> reasons = new ArrayList<>();
        if (!capabilities.path("routeMeshConfigured").asBoolean(false)) {
            reasons.add("RouteMesh target fixture is not configured");
        }
        if (("RL-E2".equals(scenario) || "RL-E5".equals(scenario))
            && !capabilities.path("directionalFaultProxyConfigured").asBoolean(false)) {
            reasons.add("directional fault proxy is not configured");
        }
        if (scenario.startsWith("RL-F")) {
            if (!capabilities.path("actorConfigured").asBoolean(false)) {
                reasons.add("Actor object server is not configured");
            }
            if (!capabilities.path("spotConfigured").asBoolean(false)) {
                reasons.add("Spot object server is not configured");
            }
            if (!capabilities.path("relocationStoreConfigured").asBoolean(false)) {
                reasons.add("Relocation Store is not configured");
            }
        }
        return reasons;
    }

    private static List<String> append(List<String> values, String value) {
        ArrayList<String> result = new ArrayList<>(values);
        result.add(value);
        return result;
    }
}
