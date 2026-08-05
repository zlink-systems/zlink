package systems.zlink.framework.runtime.actors;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Base64;
import java.util.ArrayList;
import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

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

    public static List<Message> createCommitRequestParts(
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
        Message transferState,
        List<ZLinkActorHandoffPacket> backlog) {
        return createCommitRequestParts(
            transferId, timeout, actorId, actorType, actorRef,
            sourceEntrySpotNodeRid, sourceEntrySpotId, sourceEntryRouterChannelId,
            sourceNodeRid, sourceSessionRid, adapterKey, transferState, backlog, null, null);
    }

    public static List<Message> createCommitRequestParts(
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
        Message transferState,
        List<ZLinkActorHandoffPacket> backlog,
        CoreTransfer coreTransfer) {
        return createCommitRequestParts(
            transferId, timeout, actorId, actorType, actorRef,
            sourceEntrySpotNodeRid, sourceEntrySpotId, sourceEntryRouterChannelId,
            sourceNodeRid, sourceSessionRid, adapterKey, transferState, backlog,
            coreTransfer, null);
    }

    public static List<Message> createCommitRequestParts(
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
        Message transferState,
        List<ZLinkActorHandoffPacket> backlog,
        CoreTransfer coreTransfer,
        ZLinkDeferredJoinAcceptedRecovery.Manifest completionManifest) {
        return createCommitRequestParts(
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
            transferState,
            backlog,
            coreTransfer,
            completionManifest,
            new byte[0]);
    }

    public static List<Message> createCommitRequestParts(
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
        Message transferState,
        List<ZLinkActorHandoffPacket> backlog,
        CoreTransfer coreTransfer,
        ZLinkDeferredJoinAcceptedRecovery.Manifest completionManifest,
        byte[] sessionRouteCommand44) {
        List<Message> parts = new ArrayList<>();
        parts.add(Message.from(JOIN_SPOT_PACKET_NAME.getBytes(StandardCharsets.UTF_8)));
        parts.add(
            encodeTransferRequest(
                COMMIT_PHASE,
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
                adapterKey == null ? "" : adapterKey,
                backlog.size(),
                coreTransfer,
                completionManifest,
                sessionRouteCommand44));
        parts.add(Message.from(transferState));
        for (ZLinkActorHandoffPacket packet : backlog) {
            parts.add(Message.from(Long.toString(packet.arrivalIndex()).getBytes(StandardCharsets.UTF_8)));
            parts.add(encodeReplyRoute(packet.replyRoute()));
            parts.add(Message.from(ZLinkStreamHeaderCodec.encode(packet.header())));
            parts.add(Message.from(packet.payload()));
            parts.add(Message.from(packet.acceptedJournalRecord()));
        }
        return List.copyOf(parts);
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
        ZLinkDeferredJoinAcceptedRecovery.Manifest completionManifest) {
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
            completionManifest,
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
        ZLinkDeferredJoinAcceptedRecovery.Manifest completionManifest,
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
            completionManifest == null ? "" : completionManifest.reference(),
            completionManifest == null ? "0"
                : Long.toUnsignedString(completionManifest.checksumCrc32c()),
            completionManifest == null ? "0"
                : Integer.toString(completionManifest.cursor()),
            completionManifest == null ? "0"
                : Long.toUnsignedString(completionManifest.operationIdHigh()),
            completionManifest == null ? "0"
                : Long.toUnsignedString(completionManifest.operationIdLow()),
            completionManifest == null ? ""
                : completionManifest.actorId(),
            completionManifest == null ? "0"
                : Long.toUnsignedString(completionManifest.objectGeneration()),
            completionManifest == null ? "0"
                : Long.toUnsignedString(completionManifest.aggregateIdHigh()),
            completionManifest == null ? "0"
                : Long.toUnsignedString(completionManifest.aggregateIdLow()),
            completionManifest == null ? "0"
                : Long.toUnsignedString(completionManifest.aggregateGeneration()),
            sessionRouteCommand44 == null || sessionRouteCommand44.length == 0
                ? ""
                : Base64.getEncoder().encodeToString(sessionRouteCommand44))
            .getBytes(StandardCharsets.UTF_8));
    }

    public static TransferRequest decodeTransferRequest(Message message) {
        String[] fields = message.toUtf8String().split("\n", -1);
        if ((fields.length != 14 && fields.length != 21
                && fields.length != 24 && fields.length != 28
                && fields.length != 31 && fields.length != 32)
            || (!ADMISSION_PHASE.equals(fields[0]) && !COMMIT_PHASE.equals(fields[0]))
            || fields[1].isBlank()
            || fields[3].isBlank()
            || fields[4].isBlank()
            || fields[5].isBlank()
            || fields[7].isBlank()
            || fields[8].isBlank()
            || fields[9].isBlank()
            || (fields[10].isBlank() != fields[11].isBlank())) {
            throw new ZLinkConfigurationException("invalid actor Spot transfer request");
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
            fields.length >= 21 && Boolean.parseBoolean(fields[14]),
            fields.length >= 21 ? Long.parseUnsignedLong(fields[15]) : 0L,
            fields.length >= 21 ? Long.parseUnsignedLong(fields[16]) : 0L,
            fields.length >= 21 ? Long.parseUnsignedLong(fields[17]) : 0L,
            fields.length >= 21 ? Long.parseUnsignedLong(fields[18]) : 0L,
            fields.length >= 21 ? Long.parseUnsignedLong(fields[19]) : 0L,
            fields.length >= 21 ? Long.parseUnsignedLong(fields[20]) : 0L,
            fields.length >= 24 && !fields[21].isBlank()
                ? new ZLinkDeferredJoinAcceptedRecovery.Manifest(
                    fields[21],
                    Long.parseUnsignedLong(fields[22]),
                    Integer.parseInt(fields[23]),
                    fields.length >= 28
                        ? Long.parseUnsignedLong(fields[24]) : 0L,
                    fields.length >= 28
                        ? Long.parseUnsignedLong(fields[25]) : 0L,
                    fields.length >= 28 ? fields[26] : "",
                    fields.length >= 28
                        ? Long.parseUnsignedLong(fields[27]) : 0L,
                    fields.length >= 31
                        ? Long.parseUnsignedLong(fields[28]) : 0L,
                    fields.length >= 31
                        ? Long.parseUnsignedLong(fields[29]) : 0L,
                    fields.length >= 31
                        ? Long.parseUnsignedLong(fields[30]) : 0L)
                : null,
            fields.length == 32 && !fields[31].isBlank()
                ? Base64.getDecoder().decode(fields[31])
                : new byte[0]);
    }

    public static List<WireHandoffPacket> decodeHandoffPackets(
        List<Message> parts,
        int backlogCount) {
        if (backlogCount < 0 || parts.size() != 3 + backlogCount * 5) {
            throw new ZLinkConfigurationException("invalid actor transfer handoff backlog");
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

    public static Message encodeAdmissionReply(boolean accepted, Message reply) {
        String encodedReply = reply == null
            ? ""
            : Base64.getEncoder().encodeToString(reply.toByteArray());
        return Message.from(String.join(
            "\n",
            Boolean.toString(accepted),
            encodedReply).getBytes(StandardCharsets.UTF_8));
    }

    public static AdmissionReply decodeAdmissionReply(Message message) {
        String[] fields = message.toUtf8String().split("\n", -1);
        if (fields.length != 2) {
            throw new ZLinkConfigurationException("invalid actor Spot admission reply");
        }
        return new AdmissionReply(
            Boolean.parseBoolean(fields[0]),
            fields[1].isBlank()
                ? Message.from(new byte[0])
                : Message.from(Base64.getDecoder().decode(fields[1])));
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
            throw new ZLinkConfigurationException("invalid routed actor bound session send packet");
        }
        return new BoundSessionSend(
            decodeActorRef(parts.get(1), "invalid routed actor bound session send metadata"),
            Message.from(parts.get(2)));
    }

    public static ActorPacket decodeActorPacket(List<Message> parts) {
        if (parts.size() < 5
            || !ACTOR_PACKET_NAME.equals(parts.get(0).toUtf8String())) {
            throw new ZLinkConfigurationException("invalid routed actor packet");
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
            throw new ZLinkConfigurationException("invalid actor transfer reply route");
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
            throw new ZLinkConfigurationException(errorMessage);
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

    public static JoinReply decodeJoinReply(Message message) {
        String[] fields = message.toUtf8String().split("\n", -1);
        if (fields.length != 5
            || fields[1].isBlank()
            || fields[2].isBlank()) {
            throw new ZLinkConfigurationException("invalid actor Spot route join reply");
        }
        Message reply = fields[4].isBlank()
            ? Message.from(new byte[0])
            : Message.from(Base64.getDecoder().decode(fields[4]));
        return new JoinReply(
            Boolean.parseBoolean(fields[0]),
            new ZLinkBackendActorRef(
                RoutingId.from(fields[1]),
                fields[2],
                Long.parseUnsignedLong(fields[3])),
            reply);
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
        ZLinkDeferredJoinAcceptedRecovery.Manifest completionManifest,
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
            ZLinkDeferredJoinAcceptedRecovery.Manifest completionManifest) {
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
                completionManifest,
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

    public record AdmissionReply(boolean accepted, Message reply) implements AutoCloseable {
        @Override
        public void close() {
            reply.close();
        }
    }

    public record JoinReply(
        boolean accepted,
        ZLinkBackendActorRef actorRef,
        Message reply) {
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
