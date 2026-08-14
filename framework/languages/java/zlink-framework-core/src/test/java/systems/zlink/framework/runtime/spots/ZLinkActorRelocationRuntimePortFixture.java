package systems.zlink.framework.runtime.spots;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;

/**
 * Test-side transport endpoint that drives the production target admission
 * port without opening a socket. Assertions remain on public callbacks,
 * Location authority and Actor mailbox effects.
 */
public final class ZLinkActorRelocationRuntimePortFixture {
    private final ZLinkActorSpotAdmission admission =
        new ZLinkActorSpotAdmission();
    private final ZLinkInternalSpotNode node;
    private final String spotId;
    private final ZLinkSpot<?> spot;
    private final Function<ZLinkActor, CompletionStage<Void>> joined;
    private final Function<ZLinkBackendActorRef,
        CompletionStage<List<Message>>> replay;

    public ZLinkActorRelocationRuntimePortFixture(
        ZLinkActorRuntime runtime,
        ZLinkInternalSpotNode node,
        String spotId,
        ZLinkSpot<?> spot,
        Function<ZLinkActor, CompletionStage<Void>> joined,
        Function<ZLinkBackendActorRef, CompletionStage<List<Message>>> replay,
        ZLinkSessionRelocationPeerClient sessionRoutes) {
        this.node = Objects.requireNonNull(node, "node");
        this.spotId = Objects.requireNonNull(spotId, "spotId");
        this.spot = Objects.requireNonNull(spot, "spot");
        this.joined = Objects.requireNonNull(joined, "joined");
        this.replay = Objects.requireNonNull(replay, "replay");
        admission.attach(
            Objects.requireNonNull(runtime, "runtime"),
            () -> false,
            sessionRoutes);
    }

    public CompletionStage<List<Message>> request(List<Message> parts) {
        Objects.requireNonNull(parts, "parts");
        ZLinkActorSpotRoutePackets.TransferRequest request =
            ZLinkActorSpotRoutePackets.decodeTransferRequest(parts.get(1));
        if (request.admission()) {
            return admission.prepareRoutedActor(
                    request,
                    null,
                    request.actorNodeRid(),
                    spotId,
                    spot,
                    joined,
                    ignored -> java.util.concurrent.CompletableFuture
                        .completedFuture(ZLinkSpotActorJoinResult.accept()))
                .thenApply(response -> List.of(
                    ZLinkActorSpotRoutePackets.encodeAdmissionReply(
                        response.accepted(), Message.from(new byte[0]))));
        }
        ZLinkMessage transferState = ZLinkMessage.fromEncoded(
            systems.zlink.framework.ZLinkEncodedPayload.from(
                parts.get(2).toByteArray()),
            new systems.zlink.framework.runtime.messaging
                .ZLinkJsonMessageSerializer());
        return admission.commitRoutedActor(
                request,
                transferState,
                node,
                spotId,
                spot,
                joined,
                replay)
            .thenApply(result -> {
                List<Message> replies = new ArrayList<>();
                replies.add(ZLinkActorSpotRoutePackets.encodeJoinReply(
                    result.response().accepted(),
                    result.actorRef(),
                    Message.from(new byte[0])));
                replies.addAll(result.handoffReplies().stream()
                    .map(Message::from)
                    .toList());
                result.handoffReplies().forEach(Message::close);
                return List.copyOf(replies);
            });
    }
}
