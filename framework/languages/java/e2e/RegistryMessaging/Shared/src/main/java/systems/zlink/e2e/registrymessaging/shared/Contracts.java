package systems.zlink.e2e.registrymessaging.shared;

import java.util.List;
import systems.zlink.framework.handlers.ZLinkPacket;

public final class Contracts {
    public static final String API_CHANNEL = "registry.messaging.api";
    public static final String WORKFLOW_CHANNEL = "registry.messaging.workflow";
    public static final String ROUTE_CHANNEL = "registry.messaging.route";
    public static final String HANDLER_GROUP = "registry-messaging";
    public static final String ROUTE_PACKET = "ScenarioRouteReq";

    private Contracts() {
    }

    public record ProfileReq(String value) {
    }

    public record MissingProfileReq(String value) {
    }

    public record ProfileRes(
        String value,
        String providerRid,
        String instanceId) {
    }

    public record ProfileMsg(String commandId) {
    }

    public record MissingProfileMsg(String commandId) {
    }

    public record PayloadReq(String marker, String payload) {
    }

    public record PayloadRes(
        String marker,
        int length,
        String sha256) {
    }

    public record WorkflowReq(String value) {
    }

    public record WorkflowRes(
        String value,
        String providerRid) {
    }

    @ZLinkPacket(ROUTE_PACKET)
    public record RouteReq(String value) {
    }

    public record RouteRes(
        String value,
        String targetRid,
        String sourceRid) {
    }

    public record EvidenceEntry(
        String marker,
        String providerRid,
        String value) {
    }

    public record EvidenceSnapshot(
        String providerRid,
        List<EvidenceEntry> entries) {
    }

    public record EvidenceWaitReq(
        String contains,
        int timeoutMilliseconds) {
        public EvidenceWaitReq(String contains) {
            this(contains, 10000);
        }
    }

    public record RequestFailureRes(
        boolean failed,
        String errorKind) {
    }

    public record BackpressureRes(String outcome) {
    }
}
