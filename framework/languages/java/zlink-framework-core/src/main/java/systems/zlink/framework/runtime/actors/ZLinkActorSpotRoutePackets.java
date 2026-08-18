package systems.zlink.framework.runtime.actors;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Base64;
import java.util.ArrayList;
import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinAdmissionProfileCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkCanonicalActorJoinReplyCodec;

public final class ZLinkActorSpotRoutePackets {
    public static final String JOIN_SPOT_PACKET_NAME = "__zlink.actor.joinSpot";
    public static final String ADMISSION_PHASE = "admission";
    public static final String COMMIT_PHASE = "commit";
    public static final String BOUND_SESSION_SEND_PACKET_NAME = "__zlink.actor.boundSession.send";
    public static final String SESSION_DISCONNECTED_PACKET_NAME =
        "__zlink.actor.sessionDisconnected";
    public static final String ACTOR_PACKET_NAME = "__zlink.actor.packet";
    public static final String HANDOFF_DIRECT_REPLY_ACK = "__zlink.actor.handoff.directAck";

    private ZLinkActorSpotRoutePackets() {
    }

    public static List<Message> createAdmissionRequestParts(
        String transferId,
        Duration timeout,
        String actorId,
        String actorType,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceEntrySpotNodeRid,
        String sourceEntrySpotId,
        String sourceEntryRouterChannelId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Message joinPayload) {
        return List.of(
            Message.from(JOIN_SPOT_PACKET_NAME.getBytes(StandardCharsets.UTF_8)),
            encodeTransferRequest(
                ADMISSION_PHASE,
                transferId,
                timeout,
                actorId,
                actorType,
                actorRef,
                sourceEntrySpotNodeRid,
                sourceEntrySpotId,
                sourceEntryRouterChannelId,
                sourceNodeRid,
                sourceSessionRid,
                ""),
            Message.from(joinPayload));
    }

    static List<Message> createCanonicalAdmissionRequestParts(
        String transferId,
        Duration timeout,
        String actorId,
        String actorType,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceEntrySpotNodeRid,
        String sourceEntrySpotId,
        String sourceEntryRouterChannelId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Message joinPayload,
        ZLinkActorJoinOperationId operationId) {
        return List.of(
            Message.from(JOIN_SPOT_PACKET_NAME.getBytes(StandardCharsets.UTF_8)),
            encodeTransferRequest(
                ADMISSION_PHASE,
                transferId,
                timeout,
                actorId,
                actorType,
                actorRef,
                sourceEntrySpotNodeRid,
                sourceEntrySpotId,
                sourceEntryRouterChannelId,
                sourceNodeRid,
                sourceSessionRid,
                ZLinkActorJoinAdmissionProfileCodec.encode(operationId)),
            Message.from(joinPayload));
    }

    private static Message encodeTransferRequest(
        String phase,
        String transferId,
        Duration timeout,
        String actorId,
        String actorType,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceEntrySpotNodeRid,
        String sourceEntrySpotId,
        String sourceEntryRouterChannelId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        String adapterKey) {
        return encodeTransferRequest(
            phase, transferId, timeout, actorId, actorType, actorRef,
            sourceEntrySpotNodeRid, sourceEntrySpotId,
            sourceEntryRouterChannelId, sourceNodeRid, sourceSessionRid, adapterKey, 0, null);
    }

    private static Message encodeTransferRequest(
        String phase,
        String transferId,
        Duration timeout,
        String actorId,
        String actorType,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceEntrySpotNodeRid,
        String sourceEntrySpotId,
        String sourceEntryRouterChannelId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        String adapterKey,
        int backlogCount,
        CoreTransfer coreTransfer) {
        return encodeTransferRequest(
            phase, transferId, timeout, actorId, actorType, actorRef,
            sourceEntrySpotNodeRid, sourceEntrySpotId, sourceEntryRouterChannelId,
            sourceNodeRid, sourceSessionRid, adapterKey, backlogCount, coreTransfer,
            null, new byte[0]);
    }

    private static Message encodeTransferRequest(
        String phase,
        String transferId,
        Duration timeout,
        String actorId,
        String actorType,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceEntrySpotNodeRid,
        String sourceEntrySpotId,
        String sourceEntryRouterChannelId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        String adapterKey,
        int backlogCount,
        CoreTransfer coreTransfer,
        ZLinkDirectJoinRelocation.Manifest relocationManifest) {
        return encodeTransferRequest(
            phase,
            transferId,
            timeout,
            actorId,
            actorType,
            actorRef,
            sourceEntrySpotNodeRid,
            sourceEntrySpotId,
            sourceEntryRouterChannelId,
            sourceNodeRid,
            sourceSessionRid,
            adapterKey,
            backlogCount,
            coreTransfer,
            relocationManifest,
            new byte[0]);
    }

    private static Message encodeTransferRequest(
        String phase,
        String transferId,
        Duration timeout,
        String actorId,
        String actorType,
        ZLinkBackendActorRef actorRef,
        RoutingId sourceEntrySpotNodeRid,
        String sourceEntrySpotId,
        String sourceEntryRouterChannelId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        String adapterKey,
        int backlogCount,
        CoreTransfer coreTransfer,
        ZLinkDirectJoinRelocation.Manifest relocationManifest,
        byte[] sessionRouteCommand44) {
        long timeoutMillis = timeout == null ? 30_000L : Math.max(1L, timeout.toMillis());
        CoreTransfer core = coreTransfer == null
            ? new CoreTransfer(false, 0L, 0L, 0L, 0L, 0L, 0L)
            : coreTransfer;
        return Message.from(String.join(
            "\n",
            phase,
            transferId,
            Long.toString(timeoutMillis),
            actorId,
            actorType,
            actorRef.nodeRid().toString(),
            Long.toUnsignedString(actorRef.generation()),
            sourceEntrySpotNodeRid.toString(),
            sourceEntrySpotId.toString(),
            sourceEntryRouterChannelId,
            sourceNodeRid == null ? "" : sourceNodeRid.toString(),
            sourceSessionRid == null ? "" : sourceSessionRid.toString(),
            adapterKey,
            Integer.toString(backlogCount),
            Boolean.toString(core.enabled()),
            Long.toUnsignedString(core.transferIdHigh()),
            Long.toUnsignedString(core.transferIdLow()),
            Long.toUnsignedString(core.membershipEpoch()),
            Long.toUnsignedString(core.finalSequence()),
            Long.toUnsignedString(core.reserveMessageCount()),
            Long.toUnsignedString(core.reserveByteCount()),
            relocationManifest == null ? ""
                : Base64.getEncoder().encodeToString(relocationManifest.root()),
            relocationManifest == null ? "0"
                : Long.toUnsignedString(relocationManifest.checksumCrc32c()),
            relocationManifest == null || relocationManifest.rawReply().length == 0
                ? ""
                : Base64.getEncoder().encodeToString(
                    relocationManifest.rawReply()),
            relocationManifest == null ? "0"
                : Long.toUnsignedString(relocationManifest.operationIdHigh()),
            relocationManifest == null ? "0"
                : Long.toUnsignedString(relocationManifest.operationIdLow()),
            relocationManifest == null ? "0"
                : Long.toUnsignedString(relocationManifest.aggregateIdHigh()),
            relocationManifest == null ? "0"
                : Long.toUnsignedString(relocationManifest.aggregateIdLow()),
            relocationManifest == null ? "0"
                : Long.toUnsignedString(relocationManifest.aggregateGeneration()),
            sessionRouteCommand44 == null || sessionRouteCommand44.length == 0
                ? ""
                : Base64.getEncoder().encodeToString(sessionRouteCommand44))
            .getBytes(StandardCharsets.UTF_8));
    }

    public static TransferRequest decodeTransferRequest(Message message) {
        String[] fields = message.toUtf8String().split("\n", -1);
        if (fields.length != 30
            || (!ADMISSION_PHASE.equals(fields[0]) && !COMMIT_PHASE.equals(fields[0]))
            || fields[1].isBlank()
            || fields[3].isBlank()
            || fields[4].isBlank()
            || fields[5].isBlank()
            || fields[7].isBlank()
            || fields[8].isBlank()
            || fields[9].isBlank()
            || (fields[10].isBlank() != fields[11].isBlank())) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,"invalid actor Spot transfer request");
        }
        return new TransferRequest(
            fields[0],
            fields[1],
            Long.parseLong(fields[2]),
            fields[3],
            fields[4],
            RoutingId.from(fields[5]),
            Long.parseUnsignedLong(fields[6]),
            RoutingId.from(fields[7]),
            fields[8],
            fields[9],
            fields[10].isBlank() ? null : RoutingId.from(fields[10]),
            fields[11].isBlank() ? null : RoutingId.from(fields[11]),
            fields[12].isBlank() ? null : fields[12],
            Integer.parseInt(fields[13]),
            Boolean.parseBoolean(fields[14]),
            Long.parseUnsignedLong(fields[15]),
            Long.parseUnsignedLong(fields[16]),
            Long.parseUnsignedLong(fields[17]),
            Long.parseUnsignedLong(fields[18]),
            Long.parseUnsignedLong(fields[19]),
            Long.parseUnsignedLong(fields[20]),
            !fields[21].isBlank()
                ? new ZLinkDirectJoinRelocation.Manifest(
                    Base64.getDecoder().decode(fields[21]),
                    Long.parseUnsignedLong(fields[22]),
                    Long.parseUnsignedLong(fields[24]),
                    Long.parseUnsignedLong(fields[25]),
                    Long.parseUnsignedLong(fields[26]),
                    Long.parseUnsignedLong(fields[27]),
                    Long.parseUnsignedLong(fields[28]),
                    fields[23].isBlank()
                        ? new byte[0]
                        : Base64.getDecoder().decode(fields[23]))
                : null,
            !fields[29].isBlank()
                ? Base64.getDecoder().decode(fields[29])
                : new byte[0]);
    }

    public static List<WireHandoffPacket> decodeHandoffPackets(
        List<Message> parts,
        int backlogCount) {
        if (backlogCount < 0 || parts.size() != 3 + backlogCount * 5) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,"invalid actor transfer handoff backlog");
        }
        List<WireHandoffPacket> backlog = new ArrayList<>(backlogCount);
        for (int index = 0; index < backlogCount; index++) {
            int offset = 3 + index * 5;
            backlog.add(new WireHandoffPacket(
                Long.parseLong(parts.get(offset).toUtf8String()),
                decodeReplyRoute(parts.get(offset + 1)),
                ZLinkStreamHeaderCodec.decodeOrPlain(parts.get(offset + 2).toByteArray()),
                Message.from(parts.get(offset + 3)),
                parts.get(offset + 4).toByteArray()));
        }
        return List.copyOf(backlog);
    }

    /**
     * Encodes the actor join admission outcome as the schema's canonical
     * actor-join-reply-tail (service-wire-v1.schema.json:2251-2294) instead
     * of a delimited-text dialect. Returns two parts: the binary tail
     * ({@code trailingBytes: forbidden} on the tail forbids appending the
     * reply payload after it) and the admission reply payload as its own
     * part. {@code spotId}/{@code spotGeneration}/{@code membershipEpoch}
     * are only meaningful (and required nonzero) when {@code accepted}.
     */
    public static List<Message> encodeAdmissionReply(
        boolean accepted,
        String spotId,
        long spotGeneration,
        long membershipEpoch,
        long receiveChunkLimitBytes,
        Message reply) {
        ZLinkCanonicalActorJoinReplyCodec.Tail tail = accepted
            ? new ZLinkCanonicalActorJoinReplyCodec.Tail(
                ZLinkCanonicalActorJoinReplyCodec.JOIN_RESULT_ACCEPTED,
                new ZLinkCanonicalActorJoinReplyCodec.SpotRef(spotId, spotGeneration),
                membershipEpoch,
                receiveChunkLimitBytes)
            : new ZLinkCanonicalActorJoinReplyCodec.Tail(
                ZLinkCanonicalActorJoinReplyCodec.JOIN_RESULT_REJECTED, null, null, null);
        byte[] tailBytes;
        try {
            tailBytes = new ZLinkCanonicalActorJoinReplyCodec().encodeTail(tail);
        } catch (IllegalArgumentException failure) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "invalid actor Spot admission reply", failure);
        }
        return List.of(
            Message.from(tailBytes),
            Message.from(reply == null ? new byte[0] : reply.toByteArray()));
    }

    /**
     * Decodes an admission reply encoded by {@link #encodeAdmissionReply}:
     * a two-part binary tail plus its reply payload. Strict binary decode -
     * the earlier delimited-text dialect (including its two-field mixed-
     * version tolerance) no longer exists on the wire.
     */
    public static AdmissionReply decodeAdmissionReply(List<Message> parts) {
        if (parts == null || parts.size() != 2) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "invalid actor Spot admission reply");
        }
        ZLinkCanonicalActorJoinReplyCodec.Tail tail;
        try {
            tail = new ZLinkCanonicalActorJoinReplyCodec().decodeTail(parts.get(0).toByteArray());
        } catch (IllegalArgumentException failure) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "invalid actor Spot admission reply", failure);
        }
        byte[] replyBytes = parts.get(1).toByteArray();
        return new AdmissionReply(
            tail.accepted(),
            tail.spot() == null ? null : tail.spot().spotId(),
            tail.spot() == null ? 0L : tail.spot().objectGeneration(),
            tail.membershipEpoch() == null ? 0L : tail.membershipEpoch(),
            tail.receiveChunkLimitBytes() == null ? 0L : tail.receiveChunkLimitBytes(),
            replyBytes.length == 0 ? Message.from(new byte[0]) : Message.from(replyBytes));
    }

    public static List<Message> createBoundSessionSendParts(
        ZLinkBackendActorRef actorRef,
        Message frame) {
        return List.of(
            Message.from(BOUND_SESSION_SEND_PACKET_NAME.getBytes(StandardCharsets.UTF_8)),
            encodeActorRef(actorRef),
            Message.from(frame));
    }

    public static List<Message> createActorPacketParts(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute) {
        return createActorPacketParts(actorRef, header, payload, replyRoute, null);
    }

    public static List<Message> createActorPacketParts(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute,
        Long handoffArrivalIndex) {
        return createActorPacketParts(
            actorRef, header, payload, replyRoute, handoffArrivalIndex,
            new byte[0]);
    }

    public static List<Message> createActorPacketParts(
        ZLinkBackendActorRef actorRef,
        ZLinkStreamHeader header,
        Message payload,
        ZLinkActorReplyRoute replyRoute,
        Long handoffArrivalIndex,
        byte[] acceptedJournalRecord) {
        return List.of(
            Message.from(ACTOR_PACKET_NAME.getBytes(StandardCharsets.UTF_8)),
            encodeActorRef(actorRef),
            encodeReplyRoute(replyRoute),
            Message.from(ZLinkStreamHeaderCodec.encode(header)),
            Message.from(payload),
            Message.from((handoffArrivalIndex == null
                ? ""
                : Long.toString(handoffArrivalIndex)).getBytes(StandardCharsets.UTF_8)),
            Message.from(acceptedJournalRecord == null
                ? new byte[0]
                : acceptedJournalRecord));
    }

    public static BoundSessionSend decodeBoundSessionSend(List<Message> parts) {
        if (parts.size() < 3
            || !BOUND_SESSION_SEND_PACKET_NAME.equals(parts.get(0).toUtf8String())) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,"invalid routed actor bound session send packet");
        }
        return new BoundSessionSend(
            decodeActorRef(parts.get(1), "invalid routed actor bound session send metadata"),
            Message.from(parts.get(2)));
    }

    public static ActorPacket decodeActorPacket(List<Message> parts) {
        if (parts.size() < 5
            || !ACTOR_PACKET_NAME.equals(parts.get(0).toUtf8String())) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,"invalid routed actor packet");
        }
        return new ActorPacket(
            decodeActorRef(parts.get(1), "invalid routed actor packet metadata"),
            decodeReplyRoute(parts.get(2)),
            ZLinkStreamHeaderCodec.decodeOrPlain(parts.get(3).toByteArray()),
            Message.from(parts.get(4)),
            parts.size() < 6 || parts.get(5).toUtf8String().isBlank()
                ? null
                : Long.parseLong(parts.get(5).toUtf8String()),
            parts.size() < 7 ? new byte[0] : parts.get(6).toByteArray());
    }

    public static Message createHandoffDirectReplyAck() {
        return Message.from(HANDOFF_DIRECT_REPLY_ACK.getBytes(StandardCharsets.UTF_8));
    }

    public static boolean isHandoffDirectReplyAck(Message message) {
        return message != null && HANDOFF_DIRECT_REPLY_ACK.equals(message.toUtf8String());
    }

    private static Message encodeActorRef(ZLinkBackendActorRef actorRef) {
        return Message.from(String.join(
            "\n",
            actorRef.nodeRid().toString(),
            actorRef.actorId(),
            Long.toUnsignedString(actorRef.generation())).getBytes(StandardCharsets.UTF_8));
    }

    private static Message encodeReplyRoute(ZLinkActorReplyRoute route) {
        if (route == null) {
            return Message.from(new byte[0]);
        }
        return Message.from(String.join(
            "\n",
            route.actorRef().nodeRid().toString(),
            route.actorRef().actorId(),
            Long.toUnsignedString(route.actorRef().generation()),
            route.sourceNodeRid().toString(),
            route.sourceSessionRid().toString(),
            Long.toUnsignedString(route.requestId()),
            Integer.toUnsignedString(route.flags())).getBytes(StandardCharsets.UTF_8));
    }

    private static ZLinkActorReplyRoute decodeReplyRoute(Message message) {
        if (message.toByteArray().length == 0) {
            return null;
        }
        String[] fields = message.toUtf8String().split("\n", -1);
        if (fields.length != 7
            || fields[0].isBlank()
            || fields[1].isBlank()
            || fields[3].isBlank()) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,"invalid actor transfer reply route");
        }
        return new ZLinkActorReplyRoute(
            new ZLinkBackendActorRef(
                RoutingId.from(fields[0]),
                fields[1],
                Long.parseUnsignedLong(fields[2])),
            RoutingId.from(fields[3]),
            RoutingId.from(fields[4]),
            Long.parseUnsignedLong(fields[5]),
            Integer.parseUnsignedInt(fields[6]));
    }

    private static ZLinkBackendActorRef decodeActorRef(Message metadata, String errorMessage) {
        String[] fields = metadata.toUtf8String().split("\n", -1);
        if (fields.length != 3
            || fields[0].isBlank()
            || fields[1].isBlank()) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,errorMessage);
        }
        return new ZLinkBackendActorRef(
            RoutingId.from(fields[0]),
            fields[1],
            Long.parseUnsignedLong(fields[2]));
    }

    public static Message encodeJoinReply(
        boolean accepted,
        ZLinkBackendActorRef actorRef,
        Message reply) {
        String encodedReply = reply == null
            ? ""
            : Base64.getEncoder().encodeToString(reply.toByteArray());
        return Message.from(String.join(
            "\n",
            Boolean.toString(accepted),
            actorRef.nodeRid().toString(),
            actorRef.actorId(),
            Long.toUnsignedString(actorRef.generation()),
            encodedReply).getBytes(StandardCharsets.UTF_8));
    }

    public record TransferRequest(
        String phase,
        String transferId,
        long timeoutMillis,
        String actorId,
        String actorType,
        RoutingId actorNodeRid,
        long actorGeneration,
        RoutingId sourceEntrySpotNodeRid,
        String sourceEntrySpotId,
        String sourceEntryRouterChannelId,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        String adapterKey,
        int backlogCount,
        boolean coreTransfer,
        long coreTransferIdHigh,
        long coreTransferIdLow,
        long coreMembershipEpoch,
        long coreFinalSequence,
        long coreReserveMessageCount,
        long coreReserveByteCount,
        ZLinkDirectJoinRelocation.Manifest relocationManifest,
        byte[] sessionRouteCommand44) {
        public TransferRequest(
            String phase,
            String transferId,
            long timeoutMillis,
            String actorId,
            String actorType,
            RoutingId actorNodeRid,
            long actorGeneration,
            RoutingId sourceEntrySpotNodeRid,
            String sourceEntrySpotId,
            String sourceEntryRouterChannelId,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            String adapterKey,
            int backlogCount,
            boolean coreTransfer,
            long coreTransferIdHigh,
            long coreTransferIdLow,
            long coreMembershipEpoch,
            long coreFinalSequence,
            long coreReserveMessageCount,
            long coreReserveByteCount,
            ZLinkDirectJoinRelocation.Manifest relocationManifest) {
            this(
                phase,
                transferId,
                timeoutMillis,
                actorId,
                actorType,
                actorNodeRid,
                actorGeneration,
                sourceEntrySpotNodeRid,
                sourceEntrySpotId,
                sourceEntryRouterChannelId,
                sourceNodeRid,
                sourceSessionRid,
                adapterKey,
                backlogCount,
                coreTransfer,
                coreTransferIdHigh,
                coreTransferIdLow,
                coreMembershipEpoch,
                coreFinalSequence,
                coreReserveMessageCount,
                coreReserveByteCount,
                relocationManifest,
                new byte[0]);
        }

        public TransferRequest {
            sessionRouteCommand44 = sessionRouteCommand44 == null
                ? new byte[0]
                : sessionRouteCommand44.clone();
        }

        @Override public byte[] sessionRouteCommand44() {
            return sessionRouteCommand44.clone();
        }

        public ZLinkBackendActorRef actorRef() {
            return new ZLinkBackendActorRef(actorNodeRid, actorId, actorGeneration);
        }

        public boolean admission() {
            return ADMISSION_PHASE.equals(phase);
        }

        public boolean commit() {
            return COMMIT_PHASE.equals(phase);
        }

        public boolean hasSourceSessionRoute() {
            return sourceNodeRid != null && sourceSessionRid != null;
        }
    }

    public record CoreTransfer(
        boolean enabled,
        long transferIdHigh,
        long transferIdLow,
        long membershipEpoch,
        long finalSequence,
        long reserveMessageCount,
        long reserveByteCount) {
    }

    public record WireHandoffPacket(
        long arrivalIndex,
        ZLinkActorReplyRoute replyRoute,
        ZLinkStreamHeader header,
        Message payload,
        byte[] acceptedJournalRecord) implements AutoCloseable {
        public WireHandoffPacket {
            acceptedJournalRecord = acceptedJournalRecord.clone();
        }

        @Override
        public byte[] acceptedJournalRecord() {
            return acceptedJournalRecord.clone();
        }

        @Override
        public void close() {
            payload.close();
        }
    }

    public record AdmissionReply(
        boolean accepted,
        String spotId,
        long spotGeneration,
        long membershipEpoch,
        long receiveChunkLimitBytes,
        Message reply) implements AutoCloseable {
        @Override
        public void close() {
            reply.close();
        }
    }

    public record BoundSessionSend(
        ZLinkBackendActorRef actorRef,
        Message frame) implements AutoCloseable {
        @Override
        public void close() {
            frame.close();
        }
    }

    public record ActorPacket(
        ZLinkBackendActorRef actorRef,
        ZLinkActorReplyRoute replyRoute,
        ZLinkStreamHeader header,
        Message payload,
        Long handoffArrivalIndex,
        byte[] acceptedJournalRecord) implements AutoCloseable {
        public ActorPacket {
            acceptedJournalRecord = acceptedJournalRecord.clone();
        }

        @Override
        public byte[] acceptedJournalRecord() {
            return acceptedJournalRecord.clone();
        }

        @Override
        public void close() {
            payload.close();
        }
    }
}
