package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;

public interface ZLinkBackendSpot extends ZLinkBackendObject {
    String spotId();

    default long lifecycleGeneration() {
        return 0L;
    }

    /** Removes a binding-owned Instance Spot activation when supported. */
    default boolean closeInstanceSpot() {
        return false;
    }

    void setRoutingId(String spotId);

    void setSubscription(String topic);

    ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode);

    ZLinkBackendReceived recvRoute(ZLinkBackendRecvMode mode);

    default void rememberSpotAuthority(
        RoutingId targetNodeRid,
        String spotId,
        long objectGeneration,
        long authorityOwnerGeneration) {
    }

    default void rememberSpotAuthority(
        RoutingId targetNodeRid,
        String spotId,
        long objectGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        rememberSpotAuthority(
            targetNodeRid, spotId, objectGeneration,
            authorityOwnerGeneration);
    }

    boolean publish(
        String channelName,
        String topic,
        List<Message> parts,
        SendFlags flags);

    CompletionStage<Void> publishAsync(
        String channelName,
        String topic,
        List<Message> parts,
        SendFlags flags);

    default boolean publish(
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        if (metadata == null || metadata.length == 0) {
            return publish(channelName, topic, parts, flags);
        }
        throw new UnsupportedOperationException("Spot publish metadata is unavailable");
    }

    default CompletionStage<Void> publishAsync(
        String channelName,
        String topic,
        byte[] metadata,
        List<Message> parts,
        SendFlags flags) {
        if (metadata == null || metadata.length == 0) {
            return publishAsync(channelName, topic, parts, flags);
        }
        throw new UnsupportedOperationException(
            "Spot publish metadata is unavailable");
    }

    CompletionStage<Void> sendToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        List<Message> parts);

    default CompletionStage<Void> sendToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        byte[] metadata,
        List<Message> parts) {
        if (metadata == null || metadata.length == 0) {
            return sendToSpot(
                targetNodeRid, spotId, spotGeneration, parts);
        }
        throw new UnsupportedOperationException("Spot send metadata is unavailable");
    }

    CompletionStage<ZLinkBackendReceived> requestToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        List<Message> parts,
        Duration timeout);

    default CompletionStage<ZLinkBackendReceived> requestToSpot(
        RoutingId targetNodeRid,
        String spotId,
        long spotGeneration,
        byte[] metadata,
        List<Message> parts,
        Duration timeout) {
        if (metadata == null || metadata.length == 0) {
            return requestToSpot(
                targetNodeRid,
                spotId,
                spotGeneration,
                parts,
                timeout);
        }
        throw new UnsupportedOperationException("Spot request metadata is unavailable");
    }
    void onDispatchEvent(ZLinkBackendSpotDispatchHandler handler);

    ZLinkBackendActorJoinRequest recvActorJoin(ZLinkBackendRecvMode mode);

    void replyActorJoin(
        ZLinkBackendActorJoinRequest request,
        int joinResultCode,
        List<Message> parts);

    ZLinkBackendActorLifecycleEvent recvActorLifecycle(ZLinkBackendRecvMode mode);
}
