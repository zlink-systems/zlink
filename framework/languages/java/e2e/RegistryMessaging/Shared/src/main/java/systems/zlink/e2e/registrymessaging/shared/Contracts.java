package systems.zlink.e2e.registrymessaging.shared;

import java.util.List;
import systems.zlink.framework.handlers.ZLinkPacket;

public final class Contracts {
    public static final String API_CHANNEL = "registry.messaging.api";
    public static final String WORKFLOW_CHANNEL = "registry.messaging.workflow";
    public static final String ROUTE_CHANNEL = "registry.messaging.route";
    public static final String OBJECT_ACTOR_TYPE = "registry-messaging-identity-actor";
    public static final String OBJECT_SPOT_TYPE = "registry-messaging-identity-spot";
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

    public record IdentityCreateReq(
        String actorId,
        String spotId,
        String meshName,
        String marker) {
    }

    public record IdentityRef(
        String id,
        long objectGeneration,
        String meshName,
        String nodeRid) {
    }

    public record IdentityCreateRes(
        String role,
        String actorState,
        IdentityRef actor,
        IdentityRef actorFound,
        String spotState,
        IdentityRef spot,
        IdentityRef spotFound) {
    }

    @ZLinkPacket("RegistryMessagingIdentityActorPing")
    public record IdentityActorPingReq(String marker) {
    }

    public record IdentityActorPingRes(
        String marker,
        String actorId,
        long objectGeneration,
        int sequence,
        String meshName) {
    }

    @ZLinkPacket("RegistryMessagingIdentitySpotPing")
    public record IdentitySpotPingReq(String marker) {
    }

    public record IdentitySpotPingRes(
        String marker,
        String spotId,
        long objectGeneration,
        int sequence,
        String meshName,
        String nodeRid) {
    }

    public record IdentityPingReq(
        String actorId,
        String spotId,
        String meshName,
        String marker) {
    }

    public record IdentityActorDirectReq(String actorId, String marker) {
    }

    public record IdentitySpotDirectReq(String spotId, String meshName, String marker) {
    }

    public record IdentityPingRes(
        IdentityActorPingRes actor,
        IdentitySpotPingRes spot) {
    }

    public record RequestOutcome(
        String value,
        String providerRid,
        boolean failed,
        String errorKind) {
    }
}
