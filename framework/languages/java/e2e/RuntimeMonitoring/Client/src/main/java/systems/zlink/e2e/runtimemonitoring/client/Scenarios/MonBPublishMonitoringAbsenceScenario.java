package systems.zlink.e2e.runtimemonitoring.client.Scenarios;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.function.Function;
import java.time.Duration;
import systems.zlink.e2e.runtimemonitoring.client.Support.MonitoringScenarioContext;
import systems.zlink.framework.monitoring.ZLinkMeshNodeSnapshot;
import systems.zlink.httpclient.ZLinkHttpClient;

public final class MonBPublishMonitoringAbsenceScenario {
    private static final List<Function<ZLinkMeshNodeSnapshot, ?>> PUBLIC_SNAPSHOT_ACCESSORS = List.of(
        ZLinkMeshNodeSnapshot::meshName,
        ZLinkMeshNodeSnapshot::state,
        ZLinkMeshNodeSnapshot::isReady,
        ZLinkMeshNodeSnapshot::readyPeerCount,
        ZLinkMeshNodeSnapshot::channels,
        ZLinkMeshNodeSnapshot::peers,
        ZLinkMeshNodeSnapshot::placement,
        ZLinkMeshNodeSnapshot::sequence,
        ZLinkMeshNodeSnapshot::observedAt);
    private static final List<String> FORBIDDEN_NAMES = List.of(
        "ZLinkLogicalMulticastSnapshot",
        "remoteSnapshotCount",
        "remoteAdmittedCount",
        "remoteDroppedCount",
        "remoteUnreachableCount",
        "localSnapshotCount",
        "localAdmittedCount",
        "localDroppedCount",
        "zlink.mesh_node.multicast.submits",
        "zlink.mesh_node.multicast.targets",
        "zlink.mesh_node.multicast.pending",
        "zlink.mesh_node.multicast.backpressures",
        "zlink.mesh_node.multicast.drops",
        "zlink.runtime.mesh_node.multicast_backpressured",
        "zlink.runtime.mesh_node.multicast_dropped");

    private MonBPublishMonitoringAbsenceScenario() {
    }

    public static void runZeroTarget(MonitoringScenarioContext context) {
        run(context, "MON-B1", "monitoring.subject.none", false);
    }

    public static void runLocalTarget(MonitoringScenarioContext context) {
        run(context, "MON-B2", "monitoring.subject.trigger", true);
    }

    private static void run(
        MonitoringScenarioContext context,
        String scenario,
        String topic,
        boolean createSubject) {
        assertPublicContractShape();
        int evidenceStart = context.evidenceEntryCount(context.serviceEndpoint());
        if (createSubject) {
            context.post(context.serviceEndpoint(), "/admin/create-subject-spot");
        }
        context.post(context.serviceEndpoint(), "/runtime/publish/" + topic);

        String snapshot = rawGet(context.serviceEndpoint(), "/runtime/snapshot");
        MonitoringScenarioContext.ensure(
            snapshot.contains("\"peers\"")
                && snapshot.contains("\"channels\"")
                && snapshot.contains("\"placementAvailable\"")
                && snapshot.contains("\"hostState\""),
            scenario + " generic RouteMesh monitoring fields are missing");
        assertForbiddenTextAbsent(snapshot, scenario + " snapshot");

        String evidence = context.evidence(context.serviceEndpoint()).entries()
            .stream()
            .skip(evidenceStart)
            .map(Object::toString)
            .reduce("", (left, right) -> left + "\n" + right);
        assertForbiddenTextAbsent(evidence, scenario + " events");
        System.out.println("scenario " + scenario + " passed");
    }

    private static void assertPublicContractShape() {
        MonitoringScenarioContext.ensure(
            PUBLIC_SNAPSHOT_ACCESSORS.size() == 9,
            "MeshNode snapshot public accessor contract changed");

        assertClassFileLiteralsAbsent(
            "systems/zlink/framework/runtime/internal/metrics/ZLinkRuntimeMetrics.class");
        assertClassFileLiteralsAbsent(
            "systems/zlink/framework/runtime/channels/ZLinkRouteMeshRuntimeOptionsRuntime.class");
    }

    private static String rawGet(String baseUrl, String path) {
        return ZLinkHttpClient.create(baseUrl)
            .timeout(Duration.ofSeconds(3))
            .get(path)
            .submitRaw()
            .toCompletableFuture()
            .join()
            .body();
    }

    private static void assertClassFileLiteralsAbsent(String resourceName) {
        try (var stream = ZLinkMeshNodeSnapshot.class.getClassLoader()
            .getResourceAsStream(resourceName)) {
            MonitoringScenarioContext.ensure(
                stream != null,
                "framework class resource is missing: " + resourceName);
            assertForbiddenTextAbsent(
                new String(stream.readAllBytes(), StandardCharsets.ISO_8859_1),
                "framework metric/event literals");
        } catch (IOException error) {
            throw new IllegalStateException(
                "could not inspect " + resourceName, error);
        }
    }

    private static void assertForbiddenTextAbsent(String text, String source) {
        String folded = text.toLowerCase(java.util.Locale.ROOT);
        for (String forbidden : FORBIDDEN_NAMES) {
            MonitoringScenarioContext.ensure(
                !folded.contains(forbidden.toLowerCase(java.util.Locale.ROOT)),
                source + " contains removed Publish monitoring name '" + forbidden + "'");
        }
    }
}
