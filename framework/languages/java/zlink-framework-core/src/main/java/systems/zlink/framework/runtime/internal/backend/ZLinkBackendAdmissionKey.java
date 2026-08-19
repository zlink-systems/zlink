package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;

/** Internal identity of one send-capacity domain. */
public record ZLinkBackendAdmissionKey(
    Kind kind,
    RoutingId nodeRid,
    String spotId,
    String actorId,
    long actorGeneration,
    String channelName,
    long relocationHigh,
    long relocationLow,
    RoutingId sessionRid,
    long bindingGeneration) {

    public enum Kind {
        SOCKET,
        NODE,
        CHANNEL,
        SPOT,
        ACTOR,
        BOUND_SESSION
    }

    public static ZLinkBackendAdmissionKey socket() {
        return new ZLinkBackendAdmissionKey(
            Kind.SOCKET, null, null, null, 0L, null, 0L, 0L, null, 0L);
    }

    public static ZLinkBackendAdmissionKey node(RoutingId nodeRid) {
        return new ZLinkBackendAdmissionKey(
            Kind.NODE, nodeRid, null, null, 0L, null, 0L, 0L, null, 0L);
    }

    public static ZLinkBackendAdmissionKey channel(String channelName) {
        return new ZLinkBackendAdmissionKey(
            Kind.CHANNEL, null, null, null, 0L, channelName,
            0L, 0L, null, 0L);
    }

    public static ZLinkBackendAdmissionKey spot(RoutingId nodeRid, String spotId) {
        return new ZLinkBackendAdmissionKey(
            Kind.SPOT, nodeRid, spotId, null, 0L, null,
            0L, 0L, null, 0L);
    }

    public static ZLinkBackendAdmissionKey actor(
        RoutingId nodeRid,
        String actorId,
        long generation) {
        return new ZLinkBackendAdmissionKey(
            Kind.ACTOR, nodeRid, null, actorId, generation, null,
            0L, 0L, null, 0L);
    }

    public static ZLinkBackendAdmissionKey boundSession(
        RoutingId nodeRid,
        String actorId,
        long generation) {
        return new ZLinkBackendAdmissionKey(
            Kind.BOUND_SESSION, nodeRid, null, actorId, generation, null,
            0L, 0L, null, 0L);
    }

    public static ZLinkBackendAdmissionKey relocatingBoundSession(
        RoutingId nodeRid,
        String actorId,
        long actorGeneration,
        long relocationHigh,
        long relocationLow,
        RoutingId sessionRid,
        long bindingGeneration) {
        if ((relocationHigh == 0L && relocationLow == 0L)
            || sessionRid == null || bindingGeneration == 0L) {
            throw new IllegalArgumentException(
                "relocating bound-Session admission fence is invalid");
        }
        return new ZLinkBackendAdmissionKey(
            Kind.BOUND_SESSION,
            nodeRid,
            null,
            actorId,
            actorGeneration,
            null,
            relocationHigh,
            relocationLow,
            sessionRid,
            bindingGeneration);
    }
}
