package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;

final class DispatchFailureReport {
    final ZLinkDispatchErrorSurface surface;
    final ZLinkDispatchMessageKind messageKind;
    final ZLinkDispatchErrorReason reason;
    final ZLinkDispatchErrorAction action;
    String packetName;
    String channelName;
    String topic;
    String spotId;
    String actorId;
    RoutingId sourceRid;
    String correlationId;
    Throwable error;

    private DispatchFailureReport(
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind messageKind,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action) {
        this.surface = surface;
        this.messageKind = messageKind;
        this.reason = reason;
        this.action = action;
    }

    static DispatchFailureReport of(
        ZLinkDispatchErrorSurface surface,
        ZLinkDispatchMessageKind messageKind,
        ZLinkDispatchErrorReason reason,
        ZLinkDispatchErrorAction action) {
        return new DispatchFailureReport(surface, messageKind, reason, action);
    }

    DispatchFailureReport packetName(String packetName) {
        this.packetName = packetName;
        return this;
    }

    DispatchFailureReport channelName(String channelName) {
        this.channelName = channelName;
        return this;
    }

    DispatchFailureReport topic(String topic) {
        this.topic = topic;
        return this;
    }

    DispatchFailureReport spotId(String spotId) {
        this.spotId = spotId;
        return this;
    }

    DispatchFailureReport actorId(String actorId) {
        this.actorId = actorId;
        return this;
    }

    DispatchFailureReport sourceRid(RoutingId sourceRid) {
        this.sourceRid = sourceRid;
        return this;
    }

    DispatchFailureReport correlationId(String correlationId) {
        this.correlationId = correlationId;
        return this;
    }

    DispatchFailureReport error(Throwable error) {
        this.error = error;
        return this;
    }
}
