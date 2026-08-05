package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

public record ZLinkBackendTopicMessage(
    Optional<RoutingId> routingId,
    String channelName,
    String topic,
    byte[] applicationMetadata,
    List<Message> parts,
    String contentType,
    ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
    public ZLinkBackendTopicMessage {
        applicationMetadata =
            applicationMetadata == null ? new byte[0] : applicationMetadata.clone();
    }

    public int applicationMetadataSize() {
        return applicationMetadata.length;
    }

    /** Backward-compatible constructor without an inbound content type. */
    public ZLinkBackendTopicMessage(
        Optional<RoutingId> routingId,
        String channelName,
        String topic,
        byte[] applicationMetadata,
        List<Message> parts,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        this(
            routingId,
            channelName,
            topic,
            applicationMetadata,
            parts,
            null,
            inboundDispatchLease);
    }

    public ZLinkBackendTopicMessage(
        Optional<RoutingId> routingId,
        String channelName,
        String topic,
        byte[] applicationMetadata,
        List<Message> parts) {
        this(routingId, channelName, topic, applicationMetadata, parts, null, null);
    }

    public ZLinkBackendTopicMessage(
        Optional<RoutingId> routingId,
        String topic,
        List<Message> parts) {
        this(routingId, null, topic, new byte[0], parts, null, null);
    }

    public ZLinkBackendTopicMessage(
        Optional<RoutingId> routingId,
        String topic,
        byte[] applicationMetadata,
        List<Message> parts) {
        this(routingId, null, topic, applicationMetadata, parts, null, null);
    }

    @Override
    public byte[] applicationMetadata() {
        return applicationMetadata.clone();
    }

    public void closeAdmission() {
        if (inboundDispatchLease != null) {
            inboundDispatchLease.close();
        }
    }
}
