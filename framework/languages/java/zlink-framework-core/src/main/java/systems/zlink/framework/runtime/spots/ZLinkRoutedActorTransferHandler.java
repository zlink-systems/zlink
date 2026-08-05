package systems.zlink.framework.runtime.spots;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.function.BiFunction;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorEntryTransferEnvelope;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;

/** Shared routed transfer protocol for user and Entry Spot activations. */
final class ZLinkRoutedActorTransferHandler {
    private final ZLinkSpotRuntime host;
    private final ZLinkInternalSpotNode node;
    private final String targetSpotId;
    private final Object targetSurface;
    private final BiFunction<String, ZLinkMessage, CompletionStage<ZLinkSpotActorJoinResult>> admission;
    private final Function<ZLinkActor, CompletionStage<Void>> joined;

    ZLinkRoutedActorTransferHandler(
        ZLinkSpotRuntime host,
        ZLinkInternalSpotNode node,
        String targetSpotId,
        Object targetSurface,
        BiFunction<String, ZLinkMessage, CompletionStage<ZLinkSpotActorJoinResult>> admission,
        Function<ZLinkActor, CompletionStage<Void>> joined) {
        this.host = host;
        this.node = node;
        this.targetSpotId = targetSpotId;
        this.targetSurface = targetSurface;
        this.admission = admission;
        this.joined = joined;
    }

    CompletionStage<List<Message>> handle(List<Message> parts) {
        return handle(parts, null);
    }

    CompletionStage<List<Message>> handle(
        List<Message> parts,
        RoutingId sourcePeerRid) {
        List<Message> transferParts = parts.size() == 2
            ? ZLinkActorEntryTransferEnvelope.decode(parts.get(1))
            : parts;
        boolean ownsTransferParts = transferParts != parts;
        ParsedPacket packet = ZLinkSpotRuntime.parsePacket(transferParts);
        ZLinkActorSpotRoutePackets.TransferRequest request =
            ZLinkActorSpotRoutePackets.decodeTransferRequest(packet.payload());
        Message phasePayload = transferParts.size() > 2
            ? Message.from(transferParts.get(2).toByteArray())
            : Message.from(new byte[0]);
        List<ZLinkActorSpotRoutePackets.WireHandoffPacket> backlog = request.commit()
            ? ZLinkActorSpotRoutePackets.decodeHandoffPackets(
                transferParts, request.backlogCount())
            : List.of();
        CompletionStage<List<Message>> result = request.admission()
            ? host.actorAdmissions().prepareRoutedActor(
                request,
                null,
                sourcePeerRid,
                actorId -> admission.apply(
                    actorId,
                    ZLinkMessage.fromEncoded(
                        ZLinkMessagePayloads.encoded(phasePayload),
                        host.serializerForSpot())))
                .thenApply(response -> List.of(encodeAdmission(response)))
            : host.actorAdmissions().commitRoutedActor(
                request,
                ZLinkMessage.fromEncoded(
                    ZLinkMessagePayloads.encoded(phasePayload),
                    host.serializerForSpot()),
                node,
                targetSpotId,
                targetSurface,
                joined,
                actorRef -> replay(actorRef, backlog))
                .thenApply(join -> {
                    List<Message> replies = new ArrayList<>();
                    replies.add(encodeJoin(join.actorRef(), join.response()));
                    replies.addAll(join.handoffReplies());
                    return List.copyOf(replies);
                });
        return result.handle((replies, error) -> {
            try {
                if (error != null) {
                    throw new CompletionException(error);
                }
                List<Message> copies = replies.stream().map(Message::from).toList();
                replies.forEach(Message::close);
                return copies;
            } finally {
                phasePayload.close();
                backlog.forEach(ZLinkActorSpotRoutePackets.WireHandoffPacket::close);
                if (ownsTransferParts) {
                    transferParts.forEach(Message::close);
                }
            }
        });
    }

    private CompletionStage<List<Message>> replay(
        ZLinkBackendActorRef actorRef,
        List<ZLinkActorSpotRoutePackets.WireHandoffPacket> backlog) {
        CompletionStage<List<Message>> tail =
            CompletableFuture.completedFuture(new ArrayList<>());
        for (ZLinkActorSpotRoutePackets.WireHandoffPacket packet : backlog) {
            tail = tail.thenCompose(replies -> host.dispatchTransferBacklog(
                    actorRef, packet.header(), packet.payload(),
                    packet.acceptedJournalRecord())
                .thenApply(reply -> appendReply(replies, actorRef, packet, reply)));
        }
        return tail.thenApply(List::copyOf);
    }

    private List<Message> appendReply(
        List<Message> replies,
        ZLinkBackendActorRef actorRef,
        ZLinkActorSpotRoutePackets.WireHandoffPacket packet,
        java.util.Optional<Message> reply) {
        try {
            if (packet.replyRoute() == null || reply.isEmpty()) {
                replies.add(reply.map(Message::from)
                    .orElseGet(() -> Message.from(new byte[0])));
                return replies;
            }
            host.replyTransferredRequestDirect(
                actorRef, packet.header(), packet.replyRoute(), reply);
            replies.add(Message.from(new byte[0]));
            return replies;
        } finally {
            if (packet.replyRoute() == null) {
                reply.ifPresent(Message::close);
            }
        }
    }

    private Message encodeAdmission(ZLinkSpotActorJoinResult response) {
        Message reply = response.reply() == null
            ? Message.from(new byte[0])
            : ZLinkMessagePayloads.message(response.reply(), host.serializerForSpot());
        try {
            return ZLinkActorSpotRoutePackets.encodeAdmissionReply(response.accepted(), reply);
        } finally {
            reply.close();
        }
    }

    private Message encodeJoin(
        ZLinkBackendActorRef actorRef,
        ZLinkSpotActorJoinResult response) {
        Message reply = response.reply() == null
            ? Message.from(new byte[0])
            : ZLinkMessagePayloads.message(response.reply(), host.serializerForSpot());
        try {
            return ZLinkActorSpotRoutePackets.encodeJoinReply(
                response.accepted(), actorRef, reply);
        } finally {
            reply.close();
        }
    }
}
