package systems.zlink.e2e.channelegress.shared;

import java.util.List;

public final class Contracts {
    public static final String GAME_MESH = "channel-egress.game";
    public static final String AUDIT_MESH = "channel-egress.audit";
    public static final String SESSION_CHANNEL = "game.session";
    public static final String PLAY_CHANNEL = "game.play";
    public static final String API_CHANNEL = "game.api";
    public static final String AUDIT_CHANNEL = "audit.record";
    public static final String FANOUT_CHANNEL = "game.fanout";
    public static final String STREAM_NODE = "game.stream";
    public static final String WORKFLOW_CHANNEL = "workflow.command";
    public static final String INSTANCE_SPOT_TYPE = "config12.workflow-spot";
    public static final String ACTOR_TYPE = "config12.workflow-actor";
    public static final String HANDLER_GROUP = "channel-egress";
    public static final String FANOUT_HANDLER_GROUP = "channel-egress-fanout";

    private Contracts() {
    }

    public record ChannelProbeReq(String id, String mode) {
        public ChannelProbeReq(String id) {
            this(id, "echo");
        }
    }

    public record ChannelProbeRes(
        String id,
        String role,
        String lifecycle,
        String channel,
        List<String> downstream) {
        public ChannelProbeRes {
            downstream = List.copyOf(downstream);
        }
    }

    public record ChannelProbeMsg(String id) {
    }

    public record FanoutProbeEvent(String id) {
    }

    public record StreamProbeMsg(String id) {
    }

    public record ListenerStatus(
        String kind,
        String name,
        boolean isReady,
        String advertisedEndpoint,
        String detail) {
    }

    public record SpotWorkflowReq(String id, String timerName) {
        public SpotWorkflowReq(String id) {
            this(id, id + "-timer");
        }
    }

    public record SpotWorkflowRes(String id, List<String> sequence) {
        public SpotWorkflowRes {
            sequence = List.copyOf(sequence);
        }
    }

    public record SpotCreateReq(String spotId) {
    }

    public record SpotCreateRes(String spotId, String nodeRid) {
    }

    public record ActorCreateReq(String actorId) {
    }

    public record ActorCreateRes(String actorId, String nodeRid) {
    }

    public record ObjectProbeReq(String id) {
    }

    public record ObjectProbeRes(String id, String kind, String objectId, String role) {
    }

    public record StateAddressReq(String id, String spotId, String actorId) {
    }

    public record StateAddressRes(String id, List<String> downstream) {
        public StateAddressRes {
            downstream = List.copyOf(downstream);
        }
    }

    public record WorkflowStatus(
        String state,
        boolean isReady,
        int readyTargetCount,
        String localRole,
        List<WorkflowTarget> targets) {
        public WorkflowStatus {
            targets = List.copyOf(targets);
        }
    }

    public record WorkflowTarget(String rid, int weight, String state) {
    }

    public record LocationEntry(String meshName, String rid, String endpoint, String state) {
    }

    public record InvokeReq(String channel, String id, String mode) {
        public InvokeReq(String channel, String id) {
            this(channel, id, "echo");
        }
    }

    public record InvokeRes(
        boolean succeeded,
        String error,
        ChannelProbeRes reply,
        long elapsedMilliseconds) {
    }

    public record SendRes(
        boolean succeeded,
        String error,
        long elapsedMilliseconds) {
    }

    public record EvidenceEntry(String marker, String role, String rid, String value) {
    }

    public record EvidenceSnapshot(String role, String rid, List<EvidenceEntry> entries) {
        public EvidenceSnapshot {
            entries = List.copyOf(entries);
        }
    }

    public record EvidenceWaitReq(String contains, int timeoutMilliseconds) {
    }
}
