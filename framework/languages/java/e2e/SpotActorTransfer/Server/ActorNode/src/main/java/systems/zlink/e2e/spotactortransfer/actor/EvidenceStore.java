package systems.zlink.e2e.spotactortransfer.actor;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import systems.zlink.e2e.spotactortransfer.shared.Contracts;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;

public final class EvidenceStore {
    private static final Set<String> TRANSFER_MARKERS = Set.of(
        "commit_request",
        "location_committed",
        "message_follow_registered",
        "handoff_backlog",
        "backlog_enqueued",
        "target_backlog_replayed");
    private static final Set<String> TRANSFER_ID_MARKERS = Set.of(
        "commit_request",
        "target_admission_received",
        "target_admission_accepted",
        "target_commit_received",
        "target_materialized",
        "target_session_bound",
        "target_joined_callback",
        "target_backlog_replayed",
        "location_committed");

    private final String nodeRid;
    private final Path file;
    private final CopyOnWriteArrayList<Contracts.Evidence> entries = new CopyOnWriteArrayList<>();
    private final ConcurrentHashMap<String, String> transferIds = new ConcurrentHashMap<>();

    public EvidenceStore(String nodeRid, String file) {
        this.nodeRid = nodeRid;
        this.file = Path.of(file);
        try {
            Files.createDirectories(this.file.getParent());
        } catch (IOException error) {
            throw new IllegalStateException("failed to prepare evidence directory", error);
        }
    }

    public String nodeRid() {
        return nodeRid;
    }

    public void add(String scenario, String actorId, String kind, String value) {
        add(
            scenario,
            actorId,
            kind,
            value,
            actorId == null ? null : transferIds.get(actorId),
            null,
            null);
    }

    public void addFlow(ZLinkMessageFlowEvent flow) {
        if (flow.actorId() == null || flow.packetName() == null) {
            return;
        }
        if (TRANSFER_ID_MARKERS.contains(flow.packetName()) && flow.correlationId() != null) {
            transferIds.put(flow.actorId(), flow.correlationId());
        }
        if (!TRANSFER_MARKERS.contains(flow.packetName())) {
            return;
        }
        add(
            "message_flow",
            flow.actorId(),
            flow.packetName(),
            flow.correlationId(),
            transferIds.get(flow.actorId()),
            flow.correlationId(),
            flow.flowId());
    }

    private void add(
        String scenario,
        String actorId,
        String kind,
        String value,
        String transferId,
        String correlationId,
        String flowId) {
        Contracts.Evidence evidence = new Contracts.Evidence(
            scenario == null ? "" : scenario,
            actorId == null ? "" : actorId,
            kind,
            value == null ? "" : value,
            nodeRid,
            transferId == null ? "" : transferId,
            correlationId == null ? "" : correlationId,
            flowId == null ? "" : flowId);
        entries.add(evidence);
        try {
            Files.writeString(
                file,
                evidence.text() + System.lineSeparator(),
                StandardOpenOption.CREATE,
                StandardOpenOption.APPEND);
        } catch (IOException error) {
            throw new IllegalStateException("failed to write transfer evidence", error);
        }
    }

    public Contracts.EvidenceSnapshot snapshot() {
        List<Contracts.Evidence> snapshot = entries.stream()
            .map(entry -> entry.transferId().isBlank() && transferIds.containsKey(entry.actorId())
                ? new Contracts.Evidence(
                    entry.scenario(),
                    entry.actorId(),
                    entry.kind(),
                    entry.value(),
                    entry.nodeRid(),
                    transferIds.get(entry.actorId()),
                    entry.correlationId(),
                    entry.flowId())
                : entry)
            .toList();
        return new Contracts.EvidenceSnapshot(snapshot);
    }
}
