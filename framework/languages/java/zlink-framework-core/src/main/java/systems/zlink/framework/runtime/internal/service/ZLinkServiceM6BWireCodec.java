package systems.zlink.framework.runtime.internal.service;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/**
 * Closed codec for the M6B Spot route fence. The codec validates the complete
 * header before the record can enter a Framework-owned Spot mailbox.
 */
public final class ZLinkServiceM6BWireCodec {
    private static final int PREFIX_BYTES = 5;
    private static final int SESSION_ROUTE_INTENT_MARKER = 0xD1;

    public byte[] encodeSpotHeader(
        boolean request,
        int flags,
        Long correlation,
        long operationHigh,
        long operationLow,
        int messageFollowHopCount,
        String sourceSpotId,
        SpotRouteFence target) {
        if ((flags & ~ServiceWireConstants.FLAG_METADATA) != 0
            || request != (correlation != null)
            || (correlation != null && correlation <= 0)
            || operationHigh == 0 && operationLow == 0
            || messageFollowHopCount < 0 || messageFollowHopCount > 8) {
            throw protocol("invalid Spot message header");
        }
        Objects.requireNonNull(sourceSpotId, "sourceSpotId");
        Objects.requireNonNull(target, "target");
        Writer writer = prefix(
            request
                ? ServiceWireConstants.COMMAND_SPOT_REQUEST
                : ServiceWireConstants.COMMAND_SPOT_SEND,
            flags);
        if (correlation != null) {
            writer.u64(correlation);
        }
        writer.bits64(operationHigh);
        writer.bits64(operationLow);
        writer.u8(messageFollowHopCount);
        writer.text8(sourceSpotId, "sourceSpotId");
        writer.text8(target.spotId(), "targetSpotId");
        writer.nonzero(target.spotGeneration(), "targetSpotGeneration");
        writer.rid(target.targetNodeRid(), "targetNodeRid");
        writer.nonzero(
            target.targetNodeGeneration(), "targetNodeGeneration");
        writer.nonzero(
            target.authorityOwnerGeneration(),
            "authorityOwnerGeneration");
        writer.nonzero(
            target.ownerLeaseGeneration(),
            "expectedOwnerLeaseGeneration");
        return writer.toByteArray();
    }

    public SpotMessage decodeSpotHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        boolean request;
        if (header.command() == ServiceWireConstants.COMMAND_SPOT_SEND) {
            request = false;
        } else if (
            header.command() == ServiceWireConstants.COMMAND_SPOT_REQUEST) {
            request = true;
        } else {
            throw protocol("command is not a Spot message");
        }
        if ((header.flags() & ~ServiceWireConstants.FLAG_METADATA) != 0) {
            throw protocol("Spot message contains an unknown flag");
        }
        Long correlation = request
            ? reader.nonzeroU64("correlation")
            : null;
        long operationHigh = reader.bits64("operation.high");
        long operationLow = reader.bits64("operation.low");
        if (operationHigh == 0 && operationLow == 0) {
            throw protocol("Spot operation id is zero");
        }
        int messageFollowHopCount = reader.u8("messageFollowHopCount");
        if (messageFollowHopCount > 8) {
            throw protocol("Spot Message Follow hop count exceeds its bound");
        }
        String sourceSpotId = reader.text8("sourceSpotId");
        SpotRouteFence target = new SpotRouteFence(
            reader.text8("targetSpotId"),
            reader.nonzeroU64("targetSpotGeneration"),
            reader.rid("targetNodeRid"),
            reader.nonzeroU64("targetNodeGeneration"),
            reader.nonzeroU64("authorityOwnerGeneration"),
            reader.nonzeroU64("expectedOwnerLeaseGeneration"));
        reader.end();
        return new SpotMessage(
            request,
            header.flags(),
            correlation,
            operationHigh,
            operationLow,
            messageFollowHopCount,
            sourceSpotId,
            target);
    }

    public byte[] encodeActorHeader(
        boolean request,
        int flags,
        Long correlation,
        long operationHigh,
        long operationLow,
        int messageFollowHopCount,
        ZLinkBackendActorRef sourceActor,
        ActorRouteFence target) {
        return encodeActorHeader(
            request,
            flags,
            correlation,
            operationHigh,
            operationLow,
            messageFollowHopCount,
            sourceActor,
            target,
            null);
    }

    public byte[] encodeActorHeader(
        boolean request,
        int flags,
        Long correlation,
        long operationHigh,
        long operationLow,
        int messageFollowHopCount,
        ZLinkBackendActorRef sourceActor,
        ActorRouteFence target,
        BoundSessionTail boundSession) {
        int boundFlags =
            ServiceWireConstants.FLAG_BOUND_SESSION
                | ServiceWireConstants.FLAG_SOURCE_SPOT_ID;
        if ((flags & ~(ServiceWireConstants.FLAG_METADATA | boundFlags)) != 0
            || request != (correlation != null)
            || (correlation != null && correlation <= 0)
            || ((flags & boundFlags) != 0
                && (flags & boundFlags) != boundFlags)
            || ((flags & boundFlags) == boundFlags)
                != (boundSession != null)
            || operationHigh == 0 && operationLow == 0
            || messageFollowHopCount < 0 || messageFollowHopCount > 8) {
            throw protocol("invalid Actor message header");
        }
        Objects.requireNonNull(target, "target");
        Writer writer = prefix(
            request
                ? ServiceWireConstants.COMMAND_ACTOR_REQUEST
                : ServiceWireConstants.COMMAND_ACTOR_SEND,
            flags);
        if (correlation != null) {
            writer.u64(correlation);
        }
        writer.bits64(operationHigh);
        writer.bits64(operationLow);
        writer.u8(messageFollowHopCount);
        if (sourceActor == null) {
            writer.u8(0);
        } else {
            writer.text8(sourceActor.actorId(), "sourceActorId");
            writer.nonzero(sourceActor.generation(), "sourceActorGeneration");
        }
        writer.text8(target.actor().actorId(), "targetActorId");
        writer.nonzero(
            target.actor().generation(), "targetActorGeneration");
        writer.rid(target.actor().nodeRid(), "targetNodeRid");
        writer.nonzero(
            target.targetNodeGeneration(), "targetNodeGeneration");
        writer.nonzero(
            target.authorityOwnerGeneration(),
            "authorityOwnerGeneration");
        writer.nonzero(
            target.ownerLeaseGeneration(),
            "expectedOwnerLeaseGeneration");
        if (boundSession != null) {
            writer.rid(
                boundSession.sourceSessionRid(), "sourceSessionRid");
            writer.nonzero(
                boundSession.sourceBindingGeneration(),
                "sourceBindingGeneration");
            writer.nonzero(
                boundSession.sourceSessionSequence(),
                "sourceSessionSequence");
        }
        return writer.toByteArray();
    }

    public ActorMessage decodeActorHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        boolean request;
        if (header.command() == ServiceWireConstants.COMMAND_ACTOR_SEND) {
            request = false;
        } else if (
            header.command() == ServiceWireConstants.COMMAND_ACTOR_REQUEST) {
            request = true;
        } else {
            throw protocol("command is not an Actor message");
        }
        int boundFlags =
            ServiceWireConstants.FLAG_BOUND_SESSION
                | ServiceWireConstants.FLAG_SOURCE_SPOT_ID;
        if ((header.flags()
                & ~(ServiceWireConstants.FLAG_METADATA | boundFlags)) != 0
            || ((header.flags() & boundFlags) != 0
                && (header.flags() & boundFlags) != boundFlags)) {
            throw protocol("unsupported Actor message flags");
        }
        Long correlation = request
            ? reader.nonzeroU64("correlation")
            : null;
        long operationHigh = reader.bits64("operation.high");
        long operationLow = reader.bits64("operation.low");
        if (operationHigh == 0 && operationLow == 0) {
            throw protocol("Actor operation id is zero");
        }
        int messageFollowHopCount = reader.u8("messageFollowHopCount");
        if (messageFollowHopCount > 8) {
            throw protocol("Actor Message Follow hop count exceeds its bound");
        }
        String sourceActorId = reader.optionalText8("sourceActorId");
        ActorIdentity sourceActor = sourceActorId == null
            ? null
            : new ActorIdentity(
                sourceActorId,
                reader.nonzeroU64("sourceActorGeneration"));
        String targetActorId = reader.text8("targetActorId");
        long targetActorGeneration =
            reader.nonzeroU64("targetActorGeneration");
        RoutingId targetNodeRid = reader.rid("targetNodeRid");
        ActorRouteFence target = new ActorRouteFence(
            new ZLinkBackendActorRef(
                targetNodeRid,
                targetActorId,
                targetActorGeneration),
            reader.nonzeroU64("targetNodeGeneration"),
            reader.nonzeroU64("authorityOwnerGeneration"),
            reader.nonzeroU64("expectedOwnerLeaseGeneration"));
        BoundSessionTail boundSession =
            (header.flags() & boundFlags) == boundFlags
                ? new BoundSessionTail(
                    reader.rid("sourceSessionRid"),
                    reader.nonzeroU64("sourceBindingGeneration"),
                    reader.nonzeroU64("sourceSessionSequence"))
                : null;
        reader.end();
        return new ActorMessage(
            request,
            header.flags(),
            correlation,
            operationHigh,
            operationLow,
            messageFollowHopCount,
            sourceActor,
            target,
            boundSession);
    }

    public byte[] encodeBoundSessionSendHeader(
        ActorRouteFence actor,
        long expectedBindingGeneration) {
        Writer writer = prefix(
            ServiceWireConstants.COMMAND_BOUND_SESSION_SEND,
            0);
        writeActorRoute(writer, actor);
        writer.nonzero(
            expectedBindingGeneration, "expectedBindingGeneration");
        return writer.toByteArray();
    }

    public BoundSessionSend decodeBoundSessionSendHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command()
                != ServiceWireConstants.COMMAND_BOUND_SESSION_SEND
            || header.flags() != 0) {
            throw protocol("command is not boundSessionSend");
        }
        BoundSessionSend result = new BoundSessionSend(
            readActorRoute(reader),
            reader.nonzeroU64("expectedBindingGeneration"));
        reader.end();
        return result;
    }

    public byte[] encodeBoundSessionBindHeader(BoundSessionBind binding) {
        Objects.requireNonNull(binding, "binding");
        Writer writer = prefix(
            ServiceWireConstants.COMMAND_BOUND_SESSION_BIND,
            0);
        writer.nonzero(binding.correlation(), "correlation");
        writeActorRoute(writer, binding.actor());
        writer.rid(binding.sessionRid(), "sessionRid");
        writer.u8(binding.active() ? 1 : 2);
        writer.u16(Long.BYTES);
        writer.nonzero(
            binding.bindingGeneration(), binding.active()
                ? "bindingGeneration"
                : "retiredBindingGeneration");
        return writer.toByteArray();
    }

    public BoundSessionBind decodeBoundSessionBindHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command()
                != ServiceWireConstants.COMMAND_BOUND_SESSION_BIND
            || header.flags() != 0) {
            throw protocol("command is not boundSessionBind");
        }
        long correlation = reader.nonzeroU64("correlation");
        ActorRouteFence actor = readActorRoute(reader);
        RoutingId sessionRid = reader.rid("sessionRid");
        int state = reader.u8("bindingState");
        if (state != 1 && state != 2) {
            throw protocol("unknown bound session binding state");
        }
        int bodyLength = reader.u16("binding.length");
        int bodyEnd = reader.position() + bodyLength;
        long generation = reader.nonzeroU64(
            state == 1
                ? "bindingGeneration"
                : "retiredBindingGeneration");
        if (bodyLength != Long.BYTES || reader.position() != bodyEnd) {
            throw protocol("invalid bound session binding body length");
        }
        reader.end();
        return new BoundSessionBind(
            correlation,
            actor,
            sessionRid,
            state == 1,
            generation);
    }

    public byte[] encodeLogicalMulticastHeader(
        int flags,
        String channelName,
        String topic,
        String sourceSpotId) {
        if ((flags & ~ServiceWireConstants.FLAG_METADATA) != 0) {
            throw protocol("logical multicast contains an unknown flag");
        }
        Writer writer = prefix(
            ServiceWireConstants.COMMAND_LOGICAL_MULTICAST,
            flags);
        writer.text8(channelName, "channelName");
        writer.text8(topic, "topic");
        writer.text8(sourceSpotId, "sourceSpotId");
        return writer.toByteArray();
    }

    public LogicalMulticast decodeLogicalMulticastHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command()
                != ServiceWireConstants.COMMAND_LOGICAL_MULTICAST
            || (header.flags() & ~ServiceWireConstants.FLAG_METADATA) != 0) {
            throw protocol("command is not logical multicast");
        }
        LogicalMulticast result = new LogicalMulticast(
            header.flags(),
            reader.text8("channelName"),
            reader.text8("topic"),
            reader.text8("sourceSpotId"));
        reader.end();
        return result;
    }

    public byte[] encodeInstanceSpotHeader(InstanceSpotMessage message) {
        Objects.requireNonNull(message, "message");
        if ((message.flags() & ~ServiceWireConstants.FLAG_METADATA) != 0
            || message.sourceNodeGeneration() <= 0
            || (message.request()
                != (message.operationHigh() != 0
                    || message.operationLow() != 0))
            || message.request() != (message.replyRouteId() != null)
            || (message.replyRouteId() != null
                && message.replyRouteId() <= 0)) {
            throw protocol("invalid Instance Spot message header");
        }
        Writer route = new Writer();
        route.rid(message.route().targetNodeRid(), "targetNodeRid");
        route.nonzero(
            message.route().targetNodeGeneration(),
            "targetNodeGeneration");
        route.text8(message.route().targetSpotId(), "targetSpotId");
        route.nonzero(
            message.route().objectGeneration(), "objectGeneration");
        route.text8(message.route().ownerId(), "ownerId");
        route.nonzero(
            message.route().authorityOwnerGeneration(),
            "authorityOwnerGeneration");
        route.nonzero(
            message.route().leaseGeneration(), "leaseGeneration");
        route.text16(message.route().storeVersion(), "storeVersion");
        route.text8(message.stableType(), "stableType");
        byte[] routeBody = route.toByteArray();

        Writer writer = prefix(
            ServiceWireConstants.COMMAND_INSTANCE_SPOT,
            message.flags());
        writer.u8(1);
        writer.u16(routeBody.length);
        writer.bytes(routeBody);
        writer.nonzero(
            message.sourceNodeGeneration(), "sourceNodeGeneration");
        writer.rid(message.sourceNodeRid(), "sourceNodeRid");
        writer.optionalText8(message.sourceSpotId(), "sourceSpotId");
        writer.u8(message.request() ? 2 : 1);
        writer.bits64(message.operationHigh());
        writer.bits64(message.operationLow());
        if (message.replyRouteId() != null) {
            writer.nonzero(message.replyRouteId(), "replyRouteId");
        }
        return writer.toByteArray();
    }

    public InstanceSpotMessage decodeInstanceSpotHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command() != ServiceWireConstants.COMMAND_INSTANCE_SPOT
            || (header.flags() & ~ServiceWireConstants.FLAG_METADATA) != 0
            || reader.u8("instanceRoute.version") != 1) {
            throw protocol("command is not Instance Spot");
        }
        int routeLength = reader.u16("instanceRoute.length");
        int routeEnd = reader.position() + routeLength;
        InstanceRouteFence route = new InstanceRouteFence(
            reader.rid("targetNodeRid"),
            reader.nonzeroU64("targetNodeGeneration"),
            reader.text8("targetSpotId"),
            reader.nonzeroU64("objectGeneration"),
            reader.text8("ownerId"),
            reader.nonzeroU64("authorityOwnerGeneration"),
            reader.nonzeroU64("leaseGeneration"),
            reader.text16("storeVersion"));
        String stableType = reader.text8("stableType");
        if (reader.position() != routeEnd) {
            throw protocol("invalid Instance route body length");
        }
        long sourceNodeGeneration =
            reader.nonzeroU64("sourceNodeGeneration");
        RoutingId sourceNodeRid = reader.rid("sourceNodeRid");
        String sourceSpotId = reader.optionalText8("sourceSpotId");
        int operationKind = reader.u8("operationKind");
        if (operationKind != 1 && operationKind != 2) {
            throw protocol("unknown Instance operation kind");
        }
        long operationHigh = reader.bits64("operation.high");
        long operationLow = reader.bits64("operation.low");
        boolean request = operationKind == 2;
        if ((!request && (operationHigh != 0 || operationLow != 0))
            || (request && operationHigh == 0 && operationLow == 0)) {
            throw protocol("invalid Instance operation identity");
        }
        Long replyRouteId = request
            ? reader.nonzeroU64("replyRouteId")
            : null;
        reader.end();
        return new InstanceSpotMessage(
            header.flags(),
            route,
            stableType,
            sourceNodeGeneration,
            sourceNodeRid,
            sourceSpotId,
            request,
            operationHigh,
            operationLow,
            replyRouteId);
    }

    public byte[] encodeUserSpotCreateHeader(UserSpotCreate message) {
        Objects.requireNonNull(message, "message");
        validateTerminalOperation(
            message.correlation(),
            message.operationHigh(),
            message.operationLow(),
            message.sourceNodeGeneration(),
            message.deadlineUnixMs());
        Writer writer = prefix(
            ServiceWireConstants.COMMAND_USER_SPOT_CREATE,
            0);
        writer.nonzero(message.correlation(), "correlation");
        writer.u64(message.operationHigh());
        writer.u64(message.operationLow());
        writer.rid(message.sourceNodeRid(), "sourceNodeRid");
        writer.nonzero(
            message.sourceNodeGeneration(), "sourceNodeGeneration");
        writer.text8(message.spotId(), "spotId");
        writer.text8(message.stableType(), "stableType");
        writeReservation(writer, message.reservation());
        writer.nonzero(message.deadlineUnixMs(), "deadlineUnixMs");
        return writer.toByteArray();
    }

    public UserSpotCreate decodeUserSpotCreateHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command()
                != ServiceWireConstants.COMMAND_USER_SPOT_CREATE
            || header.flags() != 0) {
            throw protocol("command is not userSpotCreate");
        }
        long correlation = reader.nonzeroU64("correlation");
        long operationHigh = reader.u64("operation.high");
        long operationLow = reader.u64("operation.low");
        RoutingId sourceNodeRid = reader.rid("sourceNodeRid");
        long sourceNodeGeneration =
            reader.nonzeroU64("sourceNodeGeneration");
        String spotId = reader.text8("spotId");
        String stableType = reader.text8("stableType");
        ReservationFence reservation = readReservation(reader);
        long deadlineUnixMs = reader.nonzeroU64("deadlineUnixMs");
        reader.end();
        validateTerminalOperation(
            correlation,
            operationHigh,
            operationLow,
            sourceNodeGeneration,
            deadlineUnixMs);
        return new UserSpotCreate(
            correlation,
            operationHigh,
            operationLow,
            sourceNodeRid,
            sourceNodeGeneration,
            spotId,
            stableType,
            reservation,
            deadlineUnixMs);
    }

    public byte[] encodeActorCreateHeader(ActorCreate message) {
        Objects.requireNonNull(message, "message");
        validateTerminalOperation(
            message.correlation(),
            message.operationHigh(),
            message.operationLow(),
            message.sourceNodeGeneration(),
            message.deadlineUnixMs());
        Writer writer = prefix(
            ServiceWireConstants.COMMAND_ACTOR_CREATE, 0);
        writer.nonzero(message.correlation(), "correlation");
        writer.u64(message.operationHigh());
        writer.u64(message.operationLow());
        writer.rid(message.sourceNodeRid(), "sourceNodeRid");
        writer.nonzero(
            message.sourceNodeGeneration(), "sourceNodeGeneration");
        writer.text8(message.actorId(), "actorId");
        writer.text8(message.stableType(), "stableType");
        writeReservation(writer, message.reservation());
        writer.nonzero(message.deadlineUnixMs(), "deadlineUnixMs");
        return writer.toByteArray();
    }

    public ActorCreate decodeActorCreateHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command()
                != ServiceWireConstants.COMMAND_ACTOR_CREATE
            || header.flags() != 0) {
            throw protocol("command is not actorCreate");
        }
        long correlation = reader.nonzeroU64("correlation");
        long operationHigh = reader.u64("operation.high");
        long operationLow = reader.u64("operation.low");
        RoutingId sourceNodeRid = reader.rid("sourceNodeRid");
        long sourceNodeGeneration =
            reader.nonzeroU64("sourceNodeGeneration");
        String actorId = reader.text8("actorId");
        String stableType = reader.text8("stableType");
        ReservationFence reservation = readReservation(reader);
        long deadlineUnixMs = reader.nonzeroU64("deadlineUnixMs");
        reader.end();
        validateTerminalOperation(
            correlation,
            operationHigh,
            operationLow,
            sourceNodeGeneration,
            deadlineUnixMs);
        return new ActorCreate(
            correlation,
            operationHigh,
            operationLow,
            sourceNodeRid,
            sourceNodeGeneration,
            actorId,
            stableType,
            reservation,
            deadlineUnixMs);
    }

    public byte[] encodeUserSpotCloseHeader(UserSpotClose message) {
        Objects.requireNonNull(message, "message");
        validateTerminalOperation(
            message.correlation(),
            message.operationHigh(),
            message.operationLow(),
            message.sourceNodeGeneration(),
            message.deadlineUnixMs());
        Writer target = new Writer();
        target.text8(message.target().spotId(), "spotId");
        target.nonzero(
            message.target().objectGeneration(), "objectGeneration");
        target.rid(
            message.target().targetNodeRid(), "targetNodeRid");
        target.nonzero(
            message.target().targetNodeGeneration(),
            "targetNodeGeneration");
        target.nonzero(
            message.target().authorityOwnerGeneration(),
            "expectedAuthorityOwnerGeneration");
        target.text16(
            message.target().storeVersion(), "expectedStoreVersion");
        byte[] targetBody = target.toByteArray();

        Writer writer = prefix(
            ServiceWireConstants.COMMAND_USER_SPOT_CLOSE,
            0);
        writer.nonzero(message.correlation(), "correlation");
        writer.u64(message.operationHigh());
        writer.u64(message.operationLow());
        writer.rid(message.sourceNodeRid(), "sourceNodeRid");
        writer.nonzero(
            message.sourceNodeGeneration(), "sourceNodeGeneration");
        writer.u8(1);
        writer.u16(targetBody.length);
        writer.bytes(targetBody);
        writer.nonzero(message.deadlineUnixMs(), "deadlineUnixMs");
        return writer.toByteArray();
    }

    public UserSpotClose decodeUserSpotCloseHeader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command()
                != ServiceWireConstants.COMMAND_USER_SPOT_CLOSE
            || header.flags() != 0) {
            throw protocol("command is not userSpotClose");
        }
        long correlation = reader.nonzeroU64("correlation");
        long operationHigh = reader.u64("operation.high");
        long operationLow = reader.u64("operation.low");
        RoutingId sourceNodeRid = reader.rid("sourceNodeRid");
        long sourceNodeGeneration =
            reader.nonzeroU64("sourceNodeGeneration");
        if (reader.u8("userSpotCloseFence.version") != 1) {
            throw protocol("unsupported User Spot close fence version");
        }
        int targetLength = reader.u16("userSpotCloseFence.length");
        int targetEnd = reader.position() + targetLength;
        UserSpotCloseFence target = new UserSpotCloseFence(
            reader.text8("spotId"),
            reader.nonzeroU64("objectGeneration"),
            reader.rid("targetNodeRid"),
            reader.nonzeroU64("targetNodeGeneration"),
            reader.nonzeroU64("expectedAuthorityOwnerGeneration"),
            reader.text16("expectedStoreVersion"));
        if (reader.position() != targetEnd) {
            throw protocol("invalid User Spot close fence length");
        }
        long deadlineUnixMs = reader.nonzeroU64("deadlineUnixMs");
        reader.end();
        validateTerminalOperation(
            correlation,
            operationHigh,
            operationLow,
            sourceNodeGeneration,
            deadlineUnixMs);
        return new UserSpotClose(
            correlation,
            operationHigh,
            operationLow,
            sourceNodeRid,
            sourceNodeGeneration,
            target,
            deadlineUnixMs);
    }

    public byte[] encodeUserSpotCreateReply(
        long correlation,
        int terminalResult,
        int failureCode,
        UserSpotCreateTerminal success) {
        requireReplyTail(terminalResult, failureCode, success != null);
        Writer writer = replyPrefix(
            correlation, terminalResult, failureCode);
        if (success != null) {
            writer.u8(success.result().wireValue);
            writer.text8(success.spotId(), "spotId");
            writer.nonzero(
                success.objectGeneration(), "objectGeneration");
        }
        return writer.toByteArray();
    }

    public UserSpotCreateReply decodeUserSpotCreateReply(byte[] frame) {
        Reader reader = replyReader(frame);
        long correlation = reader.nonzeroU64("correlation");
        int terminalResult = reader.u32("terminalResult");
        int failureCode = reader.u32("failureCode");
        UserSpotCreateTerminal success = null;
        if (terminalResult == 0) {
            success = new UserSpotCreateTerminal(
                UserSpotCreateResult.fromWire(
                    reader.u8("createResult")),
                reader.text8("spotId"),
                reader.nonzeroU64("objectGeneration"));
        }
        reader.end();
        requireReplyTail(
            terminalResult, failureCode, success != null);
        return new UserSpotCreateReply(
            correlation, terminalResult, failureCode, success);
    }

    public byte[] encodeActorCreateReply(
        long correlation,
        ActorCreationTerminal terminal) {
        Objects.requireNonNull(terminal, "terminal");
        Writer writer = replyPrefix(
            correlation,
            terminal.terminalResult(),
            terminal.failureCode());
        writeActorCreateTerminal(writer, terminal.creation());
        return writer.toByteArray();
    }

    public ActorCreateReply decodeActorCreateReply(
        byte[] frame,
        String meshName) {
        Reader reader = replyReader(frame);
        long correlation = reader.nonzeroU64("correlation");
        int terminalResult = reader.u32("terminalResult");
        int failureCode = reader.u32("failureCode");
        ActorCreateTerminal creation = terminalResult == 0
            ? readActorCreateReplyTerminal(reader, meshName)
            : null;
        reader.end();
        requireActorTerminalShape(
            terminalResult, failureCode, creation, null);
        return new ActorCreateReply(
            correlation,
            new ActorCreationTerminal(
                terminalResult,
                failureCode,
                creation,
                null));
    }

    public byte[] encodeCreationOperationTerminal(
        ActorCreationTerminal terminal) {
        Objects.requireNonNull(terminal, "terminal");
        requireActorTerminalShape(
            terminal.terminalResult(),
            terminal.failureCode(),
            terminal.creation(),
            terminal.applicationPayloadFrame());
        Writer body = new Writer();
        body.u32(terminal.terminalResult(), "terminalResult");
        body.u32(terminal.failureCode(), "failureCode");
        body.u8(terminal.creation() == null ? 0 : 1);
        if (terminal.creation() != null) {
            writeActorCreationOperationTerminal(body, terminal.creation());
        }
        body.u8(terminal.applicationPayloadFrame() == null ? 0 : 1);
        if (terminal.applicationPayloadFrame() != null) {
            body.bytes(terminal.applicationPayloadFrame());
        }
        Writer envelope = new Writer();
        envelope.u8(1);
        envelope.u32(body.toByteArray().length, "bodyLength");
        envelope.bytes(body.toByteArray());
        byte[] result = envelope.toByteArray();
        if (result.length > 1024 * 1024) {
            throw protocol(
                "creation operation terminal exceeds 1 MiB");
        }
        return result;
    }

    public ActorCreationTerminal decodeCreationOperationTerminal(
        byte[] envelope) {
        Reader reader = new Reader(envelope);
        if (reader.u8("version") != 1) {
            throw protocol(
                "unknown creation operation terminal version");
        }
        Reader body = reader.reader(reader.u32("bodyLength"));
        reader.end();
        int terminalResult = body.u32("terminalResult");
        int failureCode = body.u32("failureCode");
        int hasCreation = body.u8("hasCreation");
        if (hasCreation != 0 && hasCreation != 1) {
            throw protocol("hasCreation must be bool8");
        }
        ActorCreateTerminal creation = hasCreation == 1
            ? readActorCreationOperationTerminal(body)
            : null;
        int hasPayload = body.u8("hasApplicationPayload");
        if (hasPayload != 0 && hasPayload != 1) {
            throw protocol("hasApplicationPayload must be bool8");
        }
        byte[] applicationPayload = hasPayload == 1
            ? body.remainingBytes()
            : null;
        body.end();
        requireActorTerminalShape(
            terminalResult,
            failureCode,
            creation,
            applicationPayload);
        return new ActorCreationTerminal(
            terminalResult,
            failureCode,
            creation,
            applicationPayload);
    }

    private static void writeActorCreateTerminal(
        Writer writer,
        ActorCreateTerminal terminal) {
        writer.u8(terminal.result().wireValue);
        Writer selected = new Writer();
        if (terminal.result() != ActorCreateResult.REJECTED) {
            ActorRef actor = Objects.requireNonNull(
                terminal.actor(), "actor");
            selected.rid(actor.nodeRid(), "actor.nodeRid");
            selected.text8(actor.actorId(), "actor.actorId");
            selected.nonzero(
                actor.objectGeneration(), "actor.objectGeneration");
        }
        writer.u16(selected.toByteArray().length);
        writer.bytes(selected.toByteArray());
    }

    private static ActorCreateTerminal readActorCreateReplyTerminal(
        Reader reader,
        String meshName) {
        ActorCreateResult result = ActorCreateResult.fromWire(
            reader.u8("createResult"));
        Reader selected = reader.reader(reader.u16("creationLength"));
        ActorRef actor = null;
        if (result != ActorCreateResult.REJECTED) {
            RoutingId nodeRid = selected.rid("actor.nodeRid");
            String actorId = selected.text8("actor.actorId");
            long objectGeneration = selected.nonzeroU64(
                "actor.objectGeneration");
            actor = new ActorRef(
                actorId,
                objectGeneration,
                meshName,
                nodeRid);
        }
        selected.end();
        return new ActorCreateTerminal(result, actor);
    }

    private static void writeActorCreationOperationTerminal(
        Writer writer,
        ActorCreateTerminal terminal) {
        writer.u8(terminal.result().wireValue);
        Writer selected = new Writer();
        if (terminal.result() != ActorCreateResult.REJECTED) {
            ActorRef actor = Objects.requireNonNull(
                terminal.actor(), "actor");
            selected.text8(actor.actorId(), "actor.actorId");
            selected.nonzero(
                actor.objectGeneration(), "actor.objectGeneration");
            selected.text8(actor.meshName(), "actor.meshName");
            selected.rid(actor.nodeRid(), "actor.nodeRid");
        }
        writer.u16(selected.toByteArray().length);
        writer.bytes(selected.toByteArray());
    }

    private static ActorCreateTerminal readActorCreationOperationTerminal(
        Reader reader) {
        ActorCreateResult result = ActorCreateResult.fromWire(
            reader.u8("createResult"));
        Reader selected = reader.reader(reader.u16("creationLength"));
        ActorRef actor = null;
        if (result != ActorCreateResult.REJECTED) {
            actor = new ActorRef(
                selected.text8("actor.actorId"),
                selected.nonzeroU64("actor.objectGeneration"),
                selected.text8("actor.meshName"),
                selected.rid("actor.nodeRid"));
        }
        selected.end();
        return new ActorCreateTerminal(result, actor);
    }

    private static void requireActorTerminalShape(
        int terminalResult,
        int failureCode,
        ActorCreateTerminal creation,
        byte[] applicationPayload) {
        requireReplyTail(
            terminalResult, failureCode, creation != null);
        if (creation != null
            && creation.result() == ActorCreateResult.EXISTING
            && applicationPayload != null) {
            throw protocol(
                "Existing Actor terminal cannot carry application payload");
        }
        if (terminalResult != 0 && applicationPayload != null) {
            throw protocol(
                "failed Actor terminal cannot carry application payload");
        }
    }

    public byte[] encodeUserSpotCloseReply(
        long correlation,
        int terminalResult,
        int failureCode,
        Boolean closed) {
        requireReplyTail(terminalResult, failureCode, closed != null);
        Writer writer = replyPrefix(
            correlation, terminalResult, failureCode);
        if (closed != null) {
            writer.u8(closed ? 1 : 0);
        }
        return writer.toByteArray();
    }

    public UserSpotCloseReply decodeUserSpotCloseReply(byte[] frame) {
        Reader reader = replyReader(frame);
        long correlation = reader.nonzeroU64("correlation");
        int terminalResult = reader.u32("terminalResult");
        int failureCode = reader.u32("failureCode");
        Boolean closed = null;
        if (terminalResult == 0) {
            int value = reader.u8("closed");
            if (value != 0 && value != 1) {
                throw protocol("closed must be bool8");
            }
            closed = value == 1;
        }
        reader.end();
        requireReplyTail(
            terminalResult, failureCode, closed != null);
        return new UserSpotCloseReply(
            correlation, terminalResult, failureCode, closed);
    }

    public byte[] encodeSessionRelocationRoute(SessionRelocationRoute route) {
        Objects.requireNonNull(route, "route");
        Writer writer = prefix(
            ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTE, 0);
        writeSessionRelocationRoutePrefix(
            writer,
            route.relocation(),
            route.coordinator(),
            route.senderRole(),
            route.actor(),
            route.session(),
            route.action());
        Writer selected = new Writer();
        if (route.action() == SessionRelocationRouteAction.COMMIT) {
            selected.nonzero(route.previousAuthorityOwnerGeneration(),
                "previousAuthorityOwnerGeneration");
            selected.nonzero(route.currentAuthorityOwnerGeneration(),
                "targetAuthorityOwnerGeneration");
            selected.rid(route.targetNodeRid(), "targetNodeRid");
            selected.nonzero(route.targetNodeGeneration(), "targetNodeGeneration");
            selected.u64(route.lastAcceptedSessionSequence());
        } else {
            selected.nonzero(route.currentAuthorityOwnerGeneration(),
                "currentAuthorityOwnerGeneration");
        }
        byte[] body = selected.toByteArray();
        writer.u16(body.length);
        writer.bytes(body);
        return writer.toByteArray();
    }

    /**
     * Encodes the direct-Join route intent before the target owner generation
     * has been assigned by the Location authority.
     */
    public byte[] encodeSessionRelocationRouteIntent(
        SessionRelocationRouteIntent intent) {
        Objects.requireNonNull(intent, "intent");
        Writer writer = prefix(
            ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTE, 0);
        writeSessionRelocationRoutePrefix(
            writer,
            intent.relocation(),
            intent.coordinator(),
            intent.senderRole(),
            intent.actor(),
            intent.session(),
            intent.action());
        Writer selected = new Writer();
        selected.u8(SESSION_ROUTE_INTENT_MARKER);
        selected.nonzero(intent.previousAuthorityOwnerGeneration(),
            "previousAuthorityOwnerGeneration");
        selected.rid(intent.targetNodeRid(), "targetNodeRid");
        selected.nonzero(intent.targetNodeGeneration(), "targetNodeGeneration");
        selected.u64(intent.lastAcceptedSessionSequence());
        byte[] body = selected.toByteArray();
        writer.u16(body.length);
        writer.bytes(body);
        return writer.toByteArray();
    }

    public SessionRelocationRoute decodeSessionRelocationRoute(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command() != ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTE
            || header.flags() != 0) {
            throw protocol("command is not sessionRelocationRoute");
        }
        RelocationIdentity relocation = readRelocationIdentity(reader);
        RelocationCoordinatorFence coordinator = readCoordinatorFence(reader);
        RelocationRole senderRole = RelocationRole.fromWire(reader.u8("senderRole"));
        ActorIdentity actor = readActorIdentity(reader);
        SessionOwnerFence session = readSessionOwner(reader);
        SessionRelocationRouteAction action =
            SessionRelocationRouteAction.fromWire(reader.u8("action"));
        Reader selected = reader.reader(reader.u16("routeBodyLength"));
        long previous = 0;
        long current;
        RoutingId targetNodeRid = null;
        long targetNodeGeneration = 0;
        long highWater = 0;
        if (action == SessionRelocationRouteAction.COMMIT) {
            previous = selected.nonzeroU64("previousAuthorityOwnerGeneration");
            current = selected.nonzeroU64("targetAuthorityOwnerGeneration");
            targetNodeRid = selected.rid("targetNodeRid");
            targetNodeGeneration = selected.nonzeroU64("targetNodeGeneration");
            highWater = selected.u64("replayedHighWater");
        } else {
            current = selected.nonzeroU64("currentAuthorityOwnerGeneration");
        }
        selected.end();
        reader.end();
        return new SessionRelocationRoute(relocation, coordinator, senderRole,
            actor, session, action, previous, current, targetNodeRid,
            targetNodeGeneration, highWater);
    }

    /** Decodes the direct-Join route intent stored in the transfer root. */
    public SessionRelocationRouteIntent decodeSessionRelocationRouteIntent(
        byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command() != ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTE
            || header.flags() != 0) {
            throw protocol("frame is not a Session relocation route intent");
        }
        RelocationIdentity relocation = readRelocationIdentity(reader);
        RelocationCoordinatorFence coordinator = readCoordinatorFence(reader);
        RelocationRole senderRole = RelocationRole.fromWire(reader.u8("senderRole"));
        ActorIdentity actor = readActorIdentity(reader);
        SessionOwnerFence session = readSessionOwner(reader);
        SessionRelocationRouteAction action =
            SessionRelocationRouteAction.fromWire(reader.u8("action"));
        if (action != SessionRelocationRouteAction.COMMIT) {
            throw protocol("Session relocation route intent must commit");
        }
        Reader selected = reader.reader(reader.u16("routeBodyLength"));
        if (selected.u8("intentMarker") != SESSION_ROUTE_INTENT_MARKER) {
            throw protocol("invalid Session relocation route intent marker");
        }
        long previous = selected.nonzeroU64("previousAuthorityOwnerGeneration");
        RoutingId targetNodeRid = selected.rid("targetNodeRid");
        long targetNodeGeneration = selected.nonzeroU64("targetNodeGeneration");
        long highWater = selected.u64("replayedHighWater");
        selected.end();
        reader.end();
        return new SessionRelocationRouteIntent(
            relocation,
            coordinator,
            senderRole,
            actor,
            session,
            action,
            previous,
            targetNodeRid,
            targetNodeGeneration,
            highWater);
    }

    private static void writeSessionRelocationRoutePrefix(
        Writer writer,
        RelocationIdentity relocation,
        RelocationCoordinatorFence coordinator,
        RelocationRole senderRole,
        ActorIdentity actor,
        SessionOwnerFence session,
        SessionRelocationRouteAction action) {
        writeRelocationIdentity(writer, relocation);
        writeCoordinatorFence(writer, coordinator);
        writer.u8(senderRole.wireValue);
        writeActorIdentity(writer, actor);
        writeSessionOwner(writer, session);
        writer.u8(action.wireValue);
    }

    public byte[] encodeSessionRelocationRouted(SessionRelocationRouted routed) {
        Objects.requireNonNull(routed, "routed");
        Writer writer = prefix(
            ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTED, 0);
        writeRelocationIdentity(writer, routed.relocation());
        writeCoordinatorFence(writer, routed.coordinator());
        writeActorIdentity(writer, routed.actor());
        writeSessionOwner(writer, routed.session());
        writer.u8(routed.action().wireValue);
        writer.nonzero(routed.currentAuthorityOwnerGeneration(),
            "currentAuthorityOwnerGeneration");
        writer.u64(routed.lastAcceptedSessionSequence());
        return writer.toByteArray();
    }

    public SessionRelocationRouted decodeSessionRelocationRouted(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command() != ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTED
            || header.flags() != 0) {
            throw protocol("command is not sessionRelocationRouted");
        }
        SessionRelocationRouted routed = new SessionRelocationRouted(
            readRelocationIdentity(reader), readCoordinatorFence(reader),
            readActorIdentity(reader), readSessionOwner(reader),
            SessionRelocationRouteAction.fromWire(reader.u8("action")),
            reader.nonzeroU64("currentAuthorityOwnerGeneration"),
            reader.u64("lastAcceptedSessionSequence"));
        reader.end();
        return routed;
    }

    public enum RelocationRole {
        SOURCE(1), TARGET(2), COORDINATOR(3);
        private final int wireValue;
        RelocationRole(int wireValue) { this.wireValue = wireValue; }
        private static RelocationRole fromWire(int value) {
            return switch (value) {
                case 1 -> SOURCE;
                case 2 -> TARGET;
                case 3 -> COORDINATOR;
                default -> throw protocol("unknown relocation role");
            };
        }
    }

    public enum SessionRelocationRouteAction {
        COMMIT(1), ABORT(2);
        private final int wireValue;
        SessionRelocationRouteAction(int wireValue) { this.wireValue = wireValue; }
        private static SessionRelocationRouteAction fromWire(int value) {
            return switch (value) {
                case 1 -> COMMIT;
                case 2 -> ABORT;
                default -> throw protocol("unknown Session relocation route action");
            };
        }
    }

    public record RelocationIdentity(long high, long low) {
        public RelocationIdentity {
            if (high == 0 && low == 0) throw protocol("relocation id is zero");
        }
    }

    public record RelocationCoordinatorFence(
        String ownerId, long leaseGeneration, RoutingId nodeRid,
        long nodeGeneration, String expectedAuthorityStoreVersion) {
        public RelocationCoordinatorFence {
            Objects.requireNonNull(ownerId, "ownerId");
            Objects.requireNonNull(nodeRid, "nodeRid");
            Objects.requireNonNull(expectedAuthorityStoreVersion,
                "expectedAuthorityStoreVersion");
            if (leaseGeneration <= 0 || nodeGeneration <= 0) {
                throw protocol("coordinator generations must be nonzero");
            }
        }
    }

    public record SessionOwnerFence(
        RoutingId nodeRid, long nodeGeneration, String ownerId,
        long ownerLeaseGeneration, RoutingId sessionRid,
        long bindingGeneration) {
        public SessionOwnerFence {
            Objects.requireNonNull(nodeRid, "nodeRid");
            Objects.requireNonNull(ownerId, "ownerId");
            Objects.requireNonNull(sessionRid, "sessionRid");
            if (nodeGeneration <= 0 || ownerLeaseGeneration <= 0
                || bindingGeneration <= 0) {
                throw protocol("Session owner generations must be nonzero");
            }
        }
    }

    public record SessionRelocationRoute(
        RelocationIdentity relocation, RelocationCoordinatorFence coordinator,
        RelocationRole senderRole, ActorIdentity actor,
        SessionOwnerFence session, SessionRelocationRouteAction action,
        long previousAuthorityOwnerGeneration,
        long currentAuthorityOwnerGeneration, RoutingId targetNodeRid,
        long targetNodeGeneration, long lastAcceptedSessionSequence) {
        public SessionRelocationRoute {
            Objects.requireNonNull(relocation, "relocation");
            Objects.requireNonNull(coordinator, "coordinator");
            Objects.requireNonNull(senderRole, "senderRole");
            Objects.requireNonNull(actor, "actor");
            Objects.requireNonNull(session, "session");
            Objects.requireNonNull(action, "action");
            if (senderRole != RelocationRole.TARGET) {
                throw protocol("route update sender must be target");
            }
            if (currentAuthorityOwnerGeneration <= 0
                || lastAcceptedSessionSequence < 0) {
                throw protocol("route update generations are invalid");
            }
            if (action == SessionRelocationRouteAction.COMMIT) {
                Objects.requireNonNull(targetNodeRid, "targetNodeRid");
                if (previousAuthorityOwnerGeneration <= 0
                    || currentAuthorityOwnerGeneration
                        <= previousAuthorityOwnerGeneration
                    || targetNodeGeneration <= 0) {
                    throw protocol("commit route update is invalid");
                }
            } else if (previousAuthorityOwnerGeneration != 0
                || targetNodeRid != null || targetNodeGeneration != 0
                || lastAcceptedSessionSequence != 0) {
                throw protocol("abort route update contains commit fields");
            }
        }
    }

    public record SessionRelocationRouteIntent(
        RelocationIdentity relocation, RelocationCoordinatorFence coordinator,
        RelocationRole senderRole, ActorIdentity actor,
        SessionOwnerFence session, SessionRelocationRouteAction action,
        long previousAuthorityOwnerGeneration, RoutingId targetNodeRid,
        long targetNodeGeneration, long lastAcceptedSessionSequence) {
        public SessionRelocationRouteIntent {
            Objects.requireNonNull(relocation, "relocation");
            Objects.requireNonNull(coordinator, "coordinator");
            Objects.requireNonNull(senderRole, "senderRole");
            Objects.requireNonNull(actor, "actor");
            Objects.requireNonNull(session, "session");
            Objects.requireNonNull(action, "action");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            if (senderRole != RelocationRole.TARGET
                || action != SessionRelocationRouteAction.COMMIT
                || previousAuthorityOwnerGeneration <= 0
                || targetNodeGeneration <= 0
                || lastAcceptedSessionSequence < 0) {
                throw protocol("Session relocation route intent is invalid");
            }
        }

        public SessionRelocationRoute materialize(
            long currentAuthorityOwnerGeneration) {
            return new SessionRelocationRoute(
                relocation,
                coordinator,
                senderRole,
                actor,
                session,
                action,
                previousAuthorityOwnerGeneration,
                currentAuthorityOwnerGeneration,
                targetNodeRid,
                targetNodeGeneration,
                lastAcceptedSessionSequence);
        }
    }

    public record SessionRelocationRouted(
        RelocationIdentity relocation, RelocationCoordinatorFence coordinator,
        ActorIdentity actor, SessionOwnerFence session,
        SessionRelocationRouteAction action,
        long currentAuthorityOwnerGeneration,
        long lastAcceptedSessionSequence) {
        public SessionRelocationRouted {
            Objects.requireNonNull(relocation, "relocation");
            Objects.requireNonNull(coordinator, "coordinator");
            Objects.requireNonNull(actor, "actor");
            Objects.requireNonNull(session, "session");
            Objects.requireNonNull(action, "action");
            if (currentAuthorityOwnerGeneration <= 0
                || lastAcceptedSessionSequence < 0) {
                throw protocol("route ACK generations are invalid");
            }
        }
    }

    public record SpotRouteFence(
        String spotId,
        long spotGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        public SpotRouteFence {
            Objects.requireNonNull(spotId, "spotId");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            if (spotGeneration <= 0
                || targetNodeGeneration <= 0
                || authorityOwnerGeneration <= 0
                || ownerLeaseGeneration <= 0) {
                throw protocol("Spot route fence generations must be nonzero");
            }
        }
    }

    public record SpotMessage(
        boolean request,
        int flags,
        Long correlation,
        long operationHigh,
        long operationLow,
        int messageFollowHopCount,
        String sourceSpotId,
        SpotRouteFence target) {
        public SpotMessage(
            boolean request,
            int flags,
            Long correlation,
            String sourceSpotId,
            SpotRouteFence target) {
            this(request, flags, correlation, 1, 1, 0, sourceSpotId, target);
        }
    }

    public record ActorRouteFence(
        ZLinkBackendActorRef actor,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        public ActorRouteFence {
            Objects.requireNonNull(actor, "actor");
            if (actor.generation() <= 0
                || targetNodeGeneration <= 0
                || authorityOwnerGeneration <= 0
                || ownerLeaseGeneration <= 0) {
                throw protocol("Actor route fence generations must be nonzero");
            }
        }
    }

    public record ActorMessage(
        boolean request,
        int flags,
        Long correlation,
        long operationHigh,
        long operationLow,
        int messageFollowHopCount,
        ActorIdentity sourceActor,
        ActorRouteFence target,
        BoundSessionTail boundSession) {
        public ActorMessage(
            boolean request,
            int flags,
            Long correlation,
            long operationHigh,
            long operationLow,
            int messageFollowHopCount,
            ActorIdentity sourceActor,
            ActorRouteFence target) {
            this(
                request,
                flags,
                correlation,
                operationHigh,
                operationLow,
                messageFollowHopCount,
                sourceActor,
                target,
                null);
        }

        public ActorMessage(
            boolean request,
            int flags,
            Long correlation,
            ActorIdentity sourceActor,
            ActorRouteFence target) {
            this(request, flags, correlation, 1, 1, 0, sourceActor, target, null);
        }

        public ActorMessage(
            boolean request,
            int flags,
            Long correlation,
            ActorIdentity sourceActor,
            ActorRouteFence target,
            BoundSessionTail boundSession) {
            this(
                request,
                flags,
                correlation,
                1,
                1,
                0,
                sourceActor,
                target,
                boundSession);
        }
    }

    public record BoundSessionTail(
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence) {
        public BoundSessionTail {
            Objects.requireNonNull(sourceSessionRid, "sourceSessionRid");
            if (sourceBindingGeneration <= 0
                || sourceSessionSequence <= 0) {
                throw protocol(
                    "bound session tail generations must be nonzero");
            }
        }
    }

    public record ActorIdentity(String actorId, long generation) {
        public ActorIdentity {
            if (actorId == null || actorId.isBlank() || generation <= 0) {
                throw protocol("invalid Actor identity");
            }
        }
    }

    public record BoundSessionSend(
        ActorRouteFence actor,
        long expectedBindingGeneration) {
        public BoundSessionSend {
            Objects.requireNonNull(actor, "actor");
            if (expectedBindingGeneration <= 0) {
                throw protocol(
                    "expectedBindingGeneration must be nonzero");
            }
        }
    }

    public record BoundSessionBind(
        long correlation,
        ActorRouteFence actor,
        RoutingId sessionRid,
        boolean active,
        long bindingGeneration) {
        public BoundSessionBind {
            Objects.requireNonNull(actor, "actor");
            Objects.requireNonNull(sessionRid, "sessionRid");
            if (correlation <= 0 || bindingGeneration <= 0) {
                throw protocol(
                    "bound session generations must be nonzero");
            }
        }
    }

    public record LogicalMulticast(
        int flags,
        String channelName,
        String topic,
        String sourceSpotId) {
    }

    public record InstanceRouteFence(
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        String targetSpotId,
        long objectGeneration,
        String ownerId,
        long authorityOwnerGeneration,
        long leaseGeneration,
        String storeVersion) {
        public InstanceRouteFence {
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            Objects.requireNonNull(targetSpotId, "targetSpotId");
            if (targetNodeGeneration <= 0
                || objectGeneration <= 0
                || authorityOwnerGeneration <= 0
                || leaseGeneration <= 0) {
                throw protocol(
                    "Instance route generations must be nonzero");
            }
            if (ownerId == null || ownerId.isBlank()
                || storeVersion == null || storeVersion.isBlank()) {
                throw protocol(
                    "Instance route owner and store version are required");
            }
        }
    }

    public record InstanceSpotMessage(
        int flags,
        InstanceRouteFence route,
        String stableType,
        long sourceNodeGeneration,
        RoutingId sourceNodeRid,
        String sourceSpotId,
        boolean request,
        long operationHigh,
        long operationLow,
        Long replyRouteId) {
        public InstanceSpotMessage {
            Objects.requireNonNull(route, "route");
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            if (stableType == null || stableType.isBlank()) {
                throw protocol("Instance Spot stable type is required");
            }
        }
    }

    public record ReservationFence(
        String reservationId,
        String storeVersion,
        long objectGeneration,
        long authorityOwnerGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        String targetOwnerId,
        long targetOwnerLeaseGeneration,
        long pendingCapacityDelta) {
        public ReservationFence {
            if (reservationId == null || reservationId.isBlank()
                || storeVersion == null || storeVersion.isBlank()
                || targetOwnerId == null || targetOwnerId.isBlank()) {
                throw protocol("reservation text fields are required");
            }
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            if (objectGeneration <= 0
                || authorityOwnerGeneration <= 0
                || targetNodeGeneration <= 0
                || targetOwnerLeaseGeneration <= 0
                || pendingCapacityDelta <= 0
                || pendingCapacityDelta > 0xffff_ffffL) {
                throw protocol("reservation generations must be nonzero");
            }
        }
    }

    public record UserSpotCreate(
        long correlation,
        long operationHigh,
        long operationLow,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String spotId,
        String stableType,
        ReservationFence reservation,
        long deadlineUnixMs) {
        public UserSpotCreate {
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            Objects.requireNonNull(spotId, "spotId");
            Objects.requireNonNull(reservation, "reservation");
            if (stableType == null || stableType.isBlank()) {
                throw protocol("stableType is required");
            }
        }
    }

    public record ActorCreate(
        long correlation,
        long operationHigh,
        long operationLow,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String actorId,
        String stableType,
        ReservationFence reservation,
        long deadlineUnixMs) {
        public ActorCreate {
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            if (actorId == null || actorId.isBlank()
                || stableType == null || stableType.isBlank()) {
                throw protocol("Actor create identity is required");
            }
            Objects.requireNonNull(reservation, "reservation");
        }
    }

    public enum ActorCreateResult {
        EXISTING(1),
        CREATED(2),
        REJECTED(3);

        private final int wireValue;

        ActorCreateResult(int wireValue) {
            this.wireValue = wireValue;
        }

        static ActorCreateResult fromWire(int value) {
            for (ActorCreateResult result : values()) {
                if (result.wireValue == value) {
                    return result;
                }
            }
            throw protocol("unknown Actor create result");
        }
    }

    public record ActorCreateTerminal(
        ActorCreateResult result,
        ActorRef actor) {
        public ActorCreateTerminal {
            Objects.requireNonNull(result, "result");
            if ((result == ActorCreateResult.REJECTED) != (actor == null)) {
                throw protocol(
                    "Rejected has no ActorRef and other results require it");
            }
        }
    }

    public record ActorCreationTerminal(
        int terminalResult,
        int failureCode,
        ActorCreateTerminal creation,
        byte[] applicationPayloadFrame) {
        public ActorCreationTerminal {
            applicationPayloadFrame = applicationPayloadFrame == null
                ? null : applicationPayloadFrame.clone();
        }

        @Override
        public byte[] applicationPayloadFrame() {
            return applicationPayloadFrame == null
                ? null : applicationPayloadFrame.clone();
        }
    }

    public record ActorCreateReply(
        long correlation,
        ActorCreationTerminal terminal) {
    }

    public record UserSpotCloseFence(
        String spotId,
        long objectGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        String storeVersion) {
        public UserSpotCloseFence {
            Objects.requireNonNull(spotId, "spotId");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            if (objectGeneration <= 0
                || targetNodeGeneration <= 0
                || authorityOwnerGeneration <= 0
                || storeVersion == null
                || storeVersion.isBlank()) {
                throw protocol("invalid User Spot close fence");
            }
        }
    }

    public record UserSpotClose(
        long correlation,
        long operationHigh,
        long operationLow,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        UserSpotCloseFence target,
        long deadlineUnixMs) {
        public UserSpotClose {
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            Objects.requireNonNull(target, "target");
        }
    }

    public enum UserSpotCreateResult {
        EXISTING(1),
        CREATED(2),
        REJECTED(3);

        private final int wireValue;

        UserSpotCreateResult(int wireValue) {
            this.wireValue = wireValue;
        }

        static UserSpotCreateResult fromWire(int value) {
            for (UserSpotCreateResult result : values()) {
                if (result.wireValue == value) {
                    return result;
                }
            }
            throw protocol("unknown User Spot create result");
        }
    }

    public record UserSpotCreateTerminal(
        UserSpotCreateResult result,
        String spotId,
        long objectGeneration) {
        public UserSpotCreateTerminal {
            Objects.requireNonNull(result, "result");
            Objects.requireNonNull(spotId, "spotId");
            if (objectGeneration <= 0) {
                throw protocol("objectGeneration must be nonzero");
            }
        }
    }

    public record UserSpotCreateReply(
        long correlation,
        int terminalResult,
        int failureCode,
        UserSpotCreateTerminal success) {
    }

    public record UserSpotCloseReply(
        long correlation,
        int terminalResult,
        int failureCode,
        Boolean closed) {
    }

    private static Writer prefix(int command, int flags) {
        Writer result = new Writer();
        result.u8(ServiceWireConstants.MAGIC_0);
        result.u8(ServiceWireConstants.MAGIC_1);
        result.u8(ServiceWireConstants.WIRE_MAJOR);
        result.u8(command);
        result.u8(flags);
        return result;
    }

    private static void validateTerminalOperation(
        long correlation,
        long operationHigh,
        long operationLow,
        long sourceNodeGeneration,
        long deadlineUnixMs) {
        if (correlation <= 0
            || (operationHigh == 0 && operationLow == 0)
            || operationHigh < 0
            || operationLow < 0
            || sourceNodeGeneration <= 0
            || deadlineUnixMs <= 0) {
            throw protocol("invalid terminal operation identity");
        }
    }

    private static void writeReservation(
        Writer writer,
        ReservationFence reservation) {
        Objects.requireNonNull(reservation, "reservation");
        writer.text8(reservation.reservationId(), "reservationId");
        writer.text16(reservation.storeVersion(), "expectedStoreVersion");
        writer.nonzero(
            reservation.objectGeneration(), "objectGeneration");
        writer.nonzero(
            reservation.authorityOwnerGeneration(),
            "authorityOwnerGeneration");
        writer.rid(
            reservation.targetNodeRid(), "targetNodeRid");
        writer.nonzero(
            reservation.targetNodeGeneration(),
            "targetNodeGeneration");
        writer.text8(
            reservation.targetOwnerId(), "targetOwnerId");
        writer.nonzero(
            reservation.targetOwnerLeaseGeneration(),
            "targetOwnerLeaseGeneration");
        writer.u32(
            reservation.pendingCapacityDelta(),
            "pendingCapacityDelta");
    }

    private static ReservationFence readReservation(Reader reader) {
        return new ReservationFence(
            reader.text8("reservationId"),
            reader.text16("expectedStoreVersion"),
            reader.nonzeroU64("objectGeneration"),
            reader.nonzeroU64("authorityOwnerGeneration"),
            reader.rid("targetNodeRid"),
            reader.nonzeroU64("targetNodeGeneration"),
            reader.text8("targetOwnerId"),
            reader.nonzeroU64("targetOwnerLeaseGeneration"),
            reader.nonzeroU32("pendingCapacityDelta"));
    }

    private static Writer replyPrefix(
        long correlation,
        int terminalResult,
        int failureCode) {
        if (correlation <= 0
            || terminalResult < 0
            || failureCode < 0
            || !validTerminalFailurePair(terminalResult, failureCode)) {
            throw protocol("invalid terminal reply");
        }
        Writer writer = prefix(ServiceWireConstants.COMMAND_REPLY, 0);
        writer.nonzero(correlation, "correlation");
        writer.u32(terminalResult, "terminalResult");
        writer.u32(failureCode, "failureCode");
        return writer;
    }

    private static Reader replyReader(byte[] frame) {
        Reader reader = new Reader(frame);
        Header header = reader.prefix();
        if (header.command() != ServiceWireConstants.COMMAND_REPLY
            || header.flags() != 0) {
            throw protocol("command is not reply");
        }
        return reader;
    }

    private static void requireReplyTail(
        int terminalResult,
        int failureCode,
        boolean hasSuccessTail) {
        if (terminalResult < 0
            || failureCode < 0
            || !validTerminalFailurePair(terminalResult, failureCode)
            || (terminalResult == 0) != hasSuccessTail) {
            throw protocol("reply tail does not match terminal result");
        }
    }

    private static boolean validTerminalFailurePair(
        int terminalResult,
        int failureCode) {
        if (terminalResult == 0) {
            return failureCode == 0;
        }
        boolean typedFailure = terminalResult == 102
            || terminalResult >= 104 && terminalResult <= 107;
        return typedFailure ? failureCode != 0 : failureCode == 0;
    }

    private static void writeRelocationIdentity(
        Writer writer,
        RelocationIdentity relocation) {
        writer.bits64(relocation.high());
        writer.bits64(relocation.low());
    }

    private static RelocationIdentity readRelocationIdentity(Reader reader) {
        return new RelocationIdentity(
            reader.bits64("relocation.high"),
            reader.bits64("relocation.low"));
    }

    private static void writeCoordinatorFence(
        Writer writer,
        RelocationCoordinatorFence coordinator) {
        writer.text8(coordinator.ownerId(), "coordinatorOwnerId");
        writer.nonzero(coordinator.leaseGeneration(),
            "coordinatorLeaseGeneration");
        writer.rid(coordinator.nodeRid(), "coordinatorNodeRid");
        writer.nonzero(coordinator.nodeGeneration(),
            "coordinatorNodeGeneration");
        writer.text16(coordinator.expectedAuthorityStoreVersion(),
            "expectedAuthorityStoreVersion");
    }

    private static RelocationCoordinatorFence readCoordinatorFence(
        Reader reader) {
        return new RelocationCoordinatorFence(
            reader.text8("coordinatorOwnerId"),
            reader.nonzeroU64("coordinatorLeaseGeneration"),
            reader.rid("coordinatorNodeRid"),
            reader.nonzeroU64("coordinatorNodeGeneration"),
            reader.text16("expectedAuthorityStoreVersion"));
    }

    private static void writeActorIdentity(
        Writer writer,
        ActorIdentity actor) {
        writer.text8(actor.actorId(), "actorId");
        writer.nonzero(actor.generation(), "objectGeneration");
    }

    private static ActorIdentity readActorIdentity(Reader reader) {
        return new ActorIdentity(
            reader.text8("actorId"),
            reader.nonzeroU64("objectGeneration"));
    }

    private static void writeSessionOwner(
        Writer writer,
        SessionOwnerFence session) {
        writer.rid(session.nodeRid(), "sessionOwnerNodeRid");
        writer.nonzero(session.nodeGeneration(),
            "sessionOwnerNodeGeneration");
        writer.text8(session.ownerId(), "sessionOwnerId");
        writer.nonzero(session.ownerLeaseGeneration(),
            "sessionOwnerLeaseGeneration");
        writer.rid(session.sessionRid(), "sessionRid");
        writer.nonzero(session.bindingGeneration(), "bindingGeneration");
    }

    private static SessionOwnerFence readSessionOwner(Reader reader) {
        return new SessionOwnerFence(
            reader.rid("sessionOwnerNodeRid"),
            reader.nonzeroU64("sessionOwnerNodeGeneration"),
            reader.text8("sessionOwnerId"),
            reader.nonzeroU64("sessionOwnerLeaseGeneration"),
            reader.rid("sessionRid"),
            reader.nonzeroU64("bindingGeneration"));
    }

    private static void writeActorRoute(
        Writer writer,
        ActorRouteFence target) {
        Objects.requireNonNull(target, "target");
        writer.text8(target.actor().actorId(), "actorId");
        writer.nonzero(target.actor().generation(), "actorGeneration");
        writer.rid(target.actor().nodeRid(), "targetNodeRid");
        writer.nonzero(
            target.targetNodeGeneration(), "targetNodeGeneration");
        writer.nonzero(
            target.authorityOwnerGeneration(),
            "expectedAuthorityOwnerGeneration");
        writer.nonzero(
            target.ownerLeaseGeneration(),
            "expectedOwnerLeaseGeneration");
    }

    private static ActorRouteFence readActorRoute(Reader reader) {
        String actorId = reader.text8("actorId");
        long actorGeneration = reader.nonzeroU64("actorGeneration");
        RoutingId targetNodeRid = reader.rid("targetNodeRid");
        return new ActorRouteFence(
            new ZLinkBackendActorRef(
                targetNodeRid, actorId, actorGeneration),
            reader.nonzeroU64("targetNodeGeneration"),
            reader.nonzeroU64("expectedAuthorityOwnerGeneration"),
            reader.nonzeroU64("expectedOwnerLeaseGeneration"));
    }

    private static ZLinkServiceWireException protocol(String message) {
        return new ZLinkServiceWireException(message);
    }

    private record Header(int command, int flags) {
    }

    private static final class Writer {
        private final ByteArrayOutputStream output = new ByteArrayOutputStream();

        void u8(int value) {
            if (value < 0 || value > 0xff) {
                throw protocol("value exceeds u8");
            }
            output.write(value);
        }

        void u64(long value) {
            if (value < 0) {
                throw protocol("value exceeds supported u64 range");
            }
            output.writeBytes(ByteBuffer.allocate(Long.BYTES)
                .order(ByteOrder.BIG_ENDIAN)
                .putLong(value)
                .array());
        }

        void bits64(long value) {
            output.writeBytes(ByteBuffer.allocate(Long.BYTES)
                .order(ByteOrder.BIG_ENDIAN)
                .putLong(value)
                .array());
        }

        void u16(int value) {
            if (value < 0 || value > 0xffff) {
                throw protocol("value exceeds u16");
            }
            output.write((value >>> 8) & 0xff);
            output.write(value & 0xff);
        }

        void u32(long value, String field) {
            if (value < 0 || value > 0xffff_ffffL) {
                throw protocol(field + " exceeds u32");
            }
            output.writeBytes(ByteBuffer.allocate(Integer.BYTES)
                .order(ByteOrder.BIG_ENDIAN)
                .putInt((int) value)
                .array());
        }

        void nonzero(long value, String field) {
            if (value <= 0) {
                throw protocol(field + " must be nonzero");
            }
            u64(value);
        }

        void rid(RoutingId value, String field) {
            byte[] bytes = Objects.requireNonNull(value, field).toBytes();
            if (bytes.length == 0 || bytes.length > 0xff) {
                throw protocol(field + " exceeds rid bound");
            }
            u8(bytes.length);
            output.writeBytes(bytes);
        }

        void text8(String value, String field) {
            byte[] bytes = Objects.requireNonNull(value, field)
                .getBytes(StandardCharsets.UTF_8);
            if (bytes.length == 0
                || bytes.length > 0xff
                || value.indexOf('\0') >= 0) {
                throw protocol(field + " exceeds text8");
            }
            u8(bytes.length);
            output.writeBytes(bytes);
        }

        void text16(String value, String field) {
            byte[] bytes = Objects.requireNonNull(value, field)
                .getBytes(StandardCharsets.UTF_8);
            if (bytes.length == 0
                || bytes.length > 0xffff
                || value.indexOf('\0') >= 0) {
                throw protocol(field + " exceeds text16");
            }
            u16(bytes.length);
            output.writeBytes(bytes);
        }

        void optionalRid(RoutingId value, String field) {
            if (value == null) {
                u8(0);
                return;
            }
            rid(value, field);
        }

        void optionalText8(String value, String field) {
            if (value == null) {
                u8(0);
                return;
            }
            text8(value, field);
        }

        void bytes(byte[] value) {
            output.writeBytes(value);
        }

        byte[] toByteArray() {
            return output.toByteArray();
        }
    }

    private static final class Reader {
        private final ByteBuffer input;

        Reader(byte[] value) {
            Objects.requireNonNull(value, "value");
            input = ByteBuffer.wrap(value).order(ByteOrder.BIG_ENDIAN);
        }

        Header prefix() {
            if (input.remaining() < PREFIX_BYTES
                || u8("magic0") != ServiceWireConstants.MAGIC_0
                || u8("magic1") != ServiceWireConstants.MAGIC_1
                || u8("major") != ServiceWireConstants.WIRE_MAJOR) {
                throw protocol("invalid service wire prefix");
            }
            return new Header(u8("command"), u8("flags"));
        }

        int u8(String field) {
            require(1, field);
            return Byte.toUnsignedInt(input.get());
        }

        long nonzeroU64(String field) {
            require(Long.BYTES, field);
            long value = input.getLong();
            if (value <= 0) {
                throw protocol(field + " must be nonzero");
            }
            return value;
        }

        long u64(String field) {
            require(Long.BYTES, field);
            long value = input.getLong();
            if (value < 0) {
                throw protocol(field + " exceeds supported u64 range");
            }
            return value;
        }

        long bits64(String field) {
            require(Long.BYTES, field);
            return input.getLong();
        }

        int u16(String field) {
            require(Short.BYTES, field);
            return Short.toUnsignedInt(input.getShort());
        }

        int u32(String field) {
            require(Integer.BYTES, field);
            int value = input.getInt();
            if (value < 0) {
                throw protocol(field + " exceeds supported u32 range");
            }
            return value;
        }

        long nonzeroU32(String field) {
            require(Integer.BYTES, field);
            long value = Integer.toUnsignedLong(input.getInt());
            if (value == 0) {
                throw protocol(field + " must be nonzero");
            }
            return value;
        }

        RoutingId rid(String field) {
            int length = u8(field + ".length");
            if (length == 0) {
                throw protocol(field + " must not be empty");
            }
            byte[] bytes = new byte[length];
            require(length, field);
            input.get(bytes);
            return RoutingId.from(bytes);
        }

        RoutingId optionalRid(String field) {
            int length = u8(field + ".length");
            if (length == 0) {
                return null;
            }
            byte[] bytes = new byte[length];
            require(length, field);
            input.get(bytes);
            return RoutingId.from(bytes);
        }

        int position() {
            return input.position();
        }

        Reader reader(int length) {
            require(length, "body");
            byte[] bytes = new byte[length];
            input.get(bytes);
            return new Reader(bytes);
        }

        byte[] remainingBytes() {
            byte[] bytes = new byte[input.remaining()];
            input.get(bytes);
            return bytes;
        }

        String optionalText8(String field) {
            int length = u8(field + ".length");
            return length == 0 ? null : text(length, field);
        }

        String text8(String field) {
            int length = u8(field + ".length");
            if (length == 0) {
                throw protocol(field + " must not be empty");
            }
            return text(length, field);
        }

        String text16(String field) {
            int length = u16(field + ".length");
            if (length == 0) {
                throw protocol(field + " must not be empty");
            }
            return text(length, field);
        }

        void end() {
            if (input.hasRemaining()) {
                throw protocol("trailing bytes are forbidden");
            }
        }

        private void require(int length, String field) {
            if (input.remaining() < length) {
                throw protocol("truncated " + field);
            }
        }

        private String text(int length, String field) {
            byte[] bytes = new byte[length];
            require(length, field);
            input.get(bytes);
            try {
                String value = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes))
                    .toString();
                if (value.indexOf('\0') >= 0
                    || !java.util.Arrays.equals(
                        bytes,
                        value.getBytes(StandardCharsets.UTF_8))) {
                    throw protocol(field + " is not canonical UTF-8");
                }
                return value;
            } catch (CharacterCodingException failure) {
                throw protocol(field + " is not canonical UTF-8");
            }
        }
    }
}
