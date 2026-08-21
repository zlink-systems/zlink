using System.Buffers.Binary;
using System.Security.Cryptography;

namespace Zlink.Framework.Runtime.Actors;

internal static class ZLinkRemoteActorJoinPackets
{
    public const string RequestPacketName = "__zlink.actor.join_spot.request";
    public const string AdmissionPacketName = "__zlink.actor.join_spot.admission";
    public const string AdmissionAbortPacketName = "__zlink.actor.join_spot.admission_abort";
    public const string CommitPacketName = "__zlink.actor.join_spot.commit";
    public const string HandoffCompletionPacketName = "__zlink.actor.join_spot.handoff_completion";
    public const string BoundSessionBindPacketName = "zlink.framework.actor.bound_session.bind";
    public const string SessionDisconnectedPacketName = "zlink.framework.actor.session_disconnected";
    public const string RecreateRelocationContentType =
        "application/vnd.zlink.actor-relocation.recreate";
    public const string SnapshotRelocationContentType =
        "application/vnd.zlink.actor-relocation.snapshot";
    internal const long SnapshotApplicationStateReservationBytes =
        64L * 1024 * 1024;
    private const long FrameworkMetadataUpperBound = 64L * 1024;
    private const long AcceptedJournalUpperBound = 16L * 1024 * 1024;

    //  Spec 28 direct transfer: conservative receive-chunk-limit advertised
    //  by a target admission accept. 0 (default) means not advertised — the
    //  source then falls back to its own RelocationPayloadChunkLimit only.
    internal const ulong ConservativeReceiveChunkLimitBytes = 32_768;

    // Command 28 deliberately carries no handoff id. Both ends derive this
    // valid local relocation identity from the authenticated source Actor
    // route and the command correlation; it is never serialized on the wire.
    internal static string CreateCanonicalHandoffId(
        RoutingId sourceActorNodeRid,
        string actorId,
        ulong actorGeneration,
        ulong sourceActorNodeGeneration,
        ulong correlation)
    {
        var source = sourceActorNodeRid.ToBytes();
        var actor = System.Text.Encoding.UTF8.GetBytes(actorId);
        var material = new byte[checked(
            source.Length + sizeof(ushort) + actor.Length + (sizeof(ulong) * 3))];
        var offset = 0;
        source.CopyTo(material);
        offset += source.Length;
        BinaryPrimitives.WriteUInt16BigEndian(material.AsSpan(offset), checked((ushort)actor.Length));
        offset += sizeof(ushort);
        actor.CopyTo(material, offset);
        offset += actor.Length;
        BinaryPrimitives.WriteUInt64BigEndian(material.AsSpan(offset), actorGeneration);
        offset += sizeof(ulong);
        BinaryPrimitives.WriteUInt64BigEndian(
            material.AsSpan(offset), sourceActorNodeGeneration);
        offset += sizeof(ulong);
        BinaryPrimitives.WriteUInt64BigEndian(material.AsSpan(offset), correlation);
        var hash = SHA256.HashData(material);
        return new Guid(hash.AsSpan(0, 16)).ToString("N");
    }

    //  Spec 28 direct transfer chunk sizing: the source clamps to the
    //  smaller of its own configured limit and the target's advertised
    //  receive-chunk-limit. 0 (not advertised) is tolerated as unlimited —
    //  the source falls back to its own configured limit only.
    internal static long EffectiveDirectTransferChunkLimit(
        long configuredChunkLimit,
        ulong advertisedReceiveChunkLimitBytes) =>
        advertisedReceiveChunkLimitBytes > 0
            ? Math.Min(
                configuredChunkLimit,
                checked((long)advertisedReceiveChunkLimitBytes))
            : configuredChunkLimit;

    public static IReadOnlyList<Message> EncodeAdmissionRequest(
        ZLinkEnvelopeHeader header,
        string actorId,
        string actorType,
        string handoffId,
        DateTimeOffset deadline,
        string sourceSpotId,
        RoutingId sourceNodeRid,
        ZLinkMessage request,
        ZLinkCodecRegistryBuilder codecs,
        ulong actorGeneration,
        ulong actorAuthorityOwnerGeneration,
        long predictedPayloadBytes,
        ulong targetSpotGeneration,
        ulong targetSpotAuthorityOwnerGeneration)
    {
        var encodedRequest = request.Encode(codecs);
        var payload = new ZLinkRemoteActorAdmissionRequest(
            actorId,
            actorType,
            ZLinkSpotId.Require(sourceSpotId, nameof(sourceSpotId)),
            sourceNodeRid.ToBytes().ToArray(),
            encodedRequest.ContentType,
            encodedRequest.Payload.ToArray(),
            handoffId,
            deadline.ToUnixTimeMilliseconds(),
            actorGeneration,
            actorAuthorityOwnerGeneration,
            predictedPayloadBytes,
            targetSpotGeneration,
            targetSpotAuthorityOwnerGeneration);

        return ZLinkEnvelopeCodec.EncodeParts(
            header,
            payload,
            typeof(ZLinkRemoteActorAdmissionRequest),
            null);
    }

    public static IReadOnlyList<Message> EncodeJoinRequest(
        ZLinkEnvelopeHeader header,
        string actorId,
        string actorType,
        string handoffId,
        string sourceSpotId,
        RoutingId sourceNodeRid,
        ulong actorGeneration,
        ulong actorAuthorityOwnerGeneration,
        RoutingId? boundSessionNodeRid,
        RoutingId boundSessionRid,
        string relocationContentType,
        ZLinkRelocationManifestReference relocation,
        ZLinkMessage request,
        ZLinkCodecRegistryBuilder codecs,
        ZLinkActorBoundSession? boundSessionIdentity = null,
        ZLinkActorRelocationReservation? reservation = null,
        ZLinkSessionRelocationContext sessionRelocationContext = default,
        ulong actorNodeGeneration = 0,
        ulong expectedOwnerLeaseGeneration = 0)
    {
        var payload = CreateJoinRequest(
            actorId,
            actorType,
            handoffId,
            sourceSpotId,
            sourceNodeRid,
            actorGeneration,
            actorAuthorityOwnerGeneration,
            boundSessionNodeRid,
            boundSessionRid,
            relocationContentType,
            relocation,
            request,
            codecs,
            boundSessionIdentity,
            reservation,
            sessionRelocationContext,
            actorNodeGeneration,
            expectedOwnerLeaseGeneration);
        return EncodeJoinRequest(header, payload);
    }

    internal static ZLinkRemoteActorJoinRequest CreateJoinRequest(
        string actorId,
        string actorType,
        string handoffId,
        string sourceSpotId,
        RoutingId sourceNodeRid,
        ulong actorGeneration,
        ulong actorAuthorityOwnerGeneration,
        RoutingId? boundSessionNodeRid,
        RoutingId boundSessionRid,
        string relocationContentType,
        ZLinkRelocationManifestReference relocation,
        ZLinkMessage request,
        ZLinkCodecRegistryBuilder codecs,
        ZLinkActorBoundSession? boundSessionIdentity = null,
        ZLinkActorRelocationReservation? reservation = null,
        ZLinkSessionRelocationContext sessionRelocationContext = default,
        ulong actorNodeGeneration = 0,
        ulong expectedOwnerLeaseGeneration = 0)
    {
        var encodedRequest = request.Encode(codecs);
        return new ZLinkRemoteActorJoinRequest(
            actorId,
            actorType,
            handoffId,
            boundSessionNodeRid?.ToBytes().ToArray(),
            boundSessionRid.Size > 0 ? boundSessionRid.ToBytes().ToArray() : null,
            relocationContentType,
            relocation.Reference,
            relocation.ChecksumCrc32c,
            relocation.AggregateId,
            relocation.AggregateGeneration,
            relocation.InventoryDigest.ToArray(),
            encodedRequest.ContentType,
            encodedRequest.Payload.ToArray(),
            [],
            ZLinkSpotId.Require(sourceSpotId, nameof(sourceSpotId)),
            sourceNodeRid.ToBytes().ToArray(),
            actorGeneration,
            actorAuthorityOwnerGeneration,
            boundSessionIdentity?.BindingToken,
            boundSessionIdentity?.BindingGeneration ?? 0,
            boundSessionIdentity?.ObjectGeneration ?? actorGeneration,
            boundSessionIdentity?.AuthorityOwnerGeneration
                ?? actorAuthorityOwnerGeneration,
            boundSessionIdentity?.MeshName.Value,
            boundSessionIdentity?.TargetNodeGeneration ?? 0,
            boundSessionIdentity?.OwnerLeaseGeneration ?? 0,
            boundSessionIdentity?.SessionOwnerNodeGeneration ?? 0,
            boundSessionIdentity?.AcceptedHighWater ?? 0,
            boundSessionIdentity?.SessionOwnerId,
            boundSessionIdentity?.SessionOwnerLeaseGeneration ?? 0,
            reservation?.Token ?? "",
            reservation?.ReservedPayloadBytes ?? 0,
            reservation?.TargetNodeRid.ToBytes().ToArray(),
            reservation?.TargetNodeGeneration ?? 0,
            reservation?.TargetSpotGeneration ?? 0,
            reservation?.TargetAuthorityOwnerGeneration ?? 0,
            reservation?.TargetSpotAuthorityOwnerGeneration ?? 0,
            sessionRelocationContext.Coordinator.OwnerId,
            sessionRelocationContext.Coordinator.LeaseGeneration,
            sessionRelocationContext.Coordinator.NodeRid.IsEmpty
                ? null
                : sessionRelocationContext.Coordinator.NodeRid.ToBytes().ToArray(),
            sessionRelocationContext.Coordinator.NodeGeneration,
            sessionRelocationContext.Coordinator.ExpectedAuthorityStoreVersion,
            actorNodeGeneration,
            expectedOwnerLeaseGeneration);
    }

    internal static IReadOnlyList<Message> EncodeJoinRequest(
        ZLinkEnvelopeHeader header,
        ZLinkRemoteActorJoinRequest payload)
    {
        return ZLinkEnvelopeCodec.EncodeParts(
            header,
            payload,
            typeof(ZLinkRemoteActorJoinRequest),
            null);
    }

    internal static long MeasureRelocationPayloadBytes(
        ZLinkRemoteActorJoinRequest request)
    {
        try
        {
            var bytes = checked(
                FrameworkMetadataUpperBound
                + request.Request.LongLength
                + request.SourceNodeRid.LongLength
                + request.RelocationReference.Length * 3L
                + request.RelocationInventoryDigest.LongLength);
            if (request.BoundSessionNodeRid is { } sessionNode)
                bytes = checked(bytes + sessionNode.LongLength);
            if (request.BoundSessionRid is { } session)
                bytes = checked(bytes + session.LongLength);
            return bytes;
        }
        catch (OverflowException)
        {
            return long.MaxValue;
        }
    }

    internal static long MeasurePredictedRelocationPayloadBytes(
        ZLinkMessage request,
        ZLinkCodecRegistryBuilder codecs,
        bool snapshot)
    {
        var encoded = request.Encode(codecs);
        try
        {
            return checked(
                FrameworkMetadataUpperBound
                + AcceptedJournalUpperBound
                + encoded.Payload.Bytes.Length
                + (snapshot
                    ? SnapshotApplicationStateReservationBytes
                    : 0));
        }
        catch (OverflowException)
        {
            return long.MaxValue;
        }
    }

    internal static long MeasureStandaloneMaintenancePayloadUpperBound(
        bool snapshot)
    {
        // Standalone Actor maintenance has no logical-timer payload. The
        // accepted journal already accounts for the captured application data;
        // admission held after the seal is flow-controlled rather than capped
        // by a second relocation reservation.
        return checked(
            FrameworkMetadataUpperBound
            + AcceptedJournalUpperBound
            + (snapshot
                ? SnapshotApplicationStateReservationBytes
                : 0));
    }

    public static ZLinkRemoteActorJoinRequest DecodeJoinRequest(IReadOnlyList<Message> parts)
    {
        return (ZLinkRemoteActorJoinRequest?)ZLinkEnvelopeCodec.DecodeBody(
                   parts,
                   typeof(ZLinkRemoteActorJoinRequest))
               ?? throw new InvalidOperationException("Remote actor join request was empty.");
    }

    public static IReadOnlyList<Message> EncodeHandoffCompletionRequest(
        ZLinkEnvelopeHeader header,
        string actorId,
        string handoffId,
        string sourceSpotId,
        RoutingId sourceNodeRid,
        string targetSpotId,
        ZLinkActorJoinOperationId? operationId,
        ZLinkRemoteActorAdmissionReply admissionReply,
        ZLinkActorBoundSession? boundSession,
        IReadOnlyList<ZLinkActorHandoffFrame> frames,
        ZLinkSessionRelocationContext sessionRelocationContext = default)
    {
        return ZLinkEnvelopeCodec.EncodeParts(
            header,
            new ZLinkRemoteActorHandoffCompletionRequest(
                actorId,
                handoffId,
                ZLinkSpotId.Require(sourceSpotId, nameof(sourceSpotId)),
                sourceNodeRid.ToBytes().ToArray(),
                ZLinkSpotId.Require(targetSpotId, nameof(targetSpotId)),
                frames,
                operationId?.High ?? 0,
                operationId?.Low ?? 0,
                operationId is null ? null : admissionReply.ReplyContentType,
                operationId is null ? null : admissionReply.Reply,
                boundSession?.SessionNodeRid?.ToBytes().ToArray(),
                boundSession?.SessionRid.ToBytes().ToArray(),
                boundSession?.BindingToken,
                boundSession?.BindingGeneration ?? 0,
                boundSession?.ObjectGeneration ?? 0,
                boundSession?.AuthorityOwnerGeneration ?? 0,
                boundSession?.MeshName.Value,
                boundSession?.TargetNodeGeneration ?? 0,
                boundSession?.OwnerLeaseGeneration ?? 0,
                boundSession?.SessionOwnerNodeGeneration ?? 0,
                boundSession?.AcceptedHighWater ?? 0,
                boundSession?.SessionOwnerId,
                boundSession?.SessionOwnerLeaseGeneration ?? 0,
                sessionRelocationContext.Coordinator.OwnerId,
                sessionRelocationContext.Coordinator.LeaseGeneration,
                sessionRelocationContext.Coordinator.NodeRid.IsEmpty
                    ? null
                    : sessionRelocationContext.Coordinator.NodeRid.ToBytes().ToArray(),
                sessionRelocationContext.Coordinator.NodeGeneration,
                sessionRelocationContext.Coordinator
                    .ExpectedAuthorityStoreVersion),
            typeof(ZLinkRemoteActorHandoffCompletionRequest),
            null);
    }

    public static ZLinkRemoteActorHandoffCompletionRequest DecodeHandoffCompletionRequest(
        IReadOnlyList<Message> parts)
    {
        return (ZLinkRemoteActorHandoffCompletionRequest?)ZLinkEnvelopeCodec.DecodeBody(
                   parts,
                   typeof(ZLinkRemoteActorHandoffCompletionRequest))
               ?? throw new InvalidOperationException("Remote actor handoff completion request was empty.");
    }

    public static ZLinkRemoteActorAdmissionRequest DecodeAdmissionRequest(IReadOnlyList<Message> parts)
    {
        return (ZLinkRemoteActorAdmissionRequest?)ZLinkEnvelopeCodec.DecodeBody(
                   parts,
                   typeof(ZLinkRemoteActorAdmissionRequest))
               ?? throw new InvalidOperationException("Remote actor admission request was empty.");
    }

    public static IReadOnlyList<Message> EncodeAdmissionAbortRequest(
        ZLinkEnvelopeHeader header,
        string actorId,
        string handoffId,
        string reservationToken)
    {
        return ZLinkEnvelopeCodec.EncodeParts(
            header,
            new ZLinkRemoteActorAdmissionAbortRequest(
                actorId,
                handoffId,
                reservationToken),
            typeof(ZLinkRemoteActorAdmissionAbortRequest),
            null);
    }

    public static ZLinkRemoteActorAdmissionAbortRequest DecodeAdmissionAbortRequest(
        IReadOnlyList<Message> parts)
    {
        return (ZLinkRemoteActorAdmissionAbortRequest?)ZLinkEnvelopeCodec.DecodeBody(
                   parts,
                   typeof(ZLinkRemoteActorAdmissionAbortRequest))
               ?? throw new InvalidOperationException(
                   "Remote actor admission abort request was empty.");
    }

    public static ZLinkRemoteActorBoundSessionRoute DecodeBoundSessionRoute(ZLinkRemoteActorJoinRequest request)
    {
        return new ZLinkRemoteActorBoundSessionRoute(
            ToRoutingId(request.BoundSessionNodeRid),
            ToRoutingId(request.BoundSessionRid),
            request.BoundSessionBindingToken,
            request.BoundSessionBindingGeneration,
            request.BoundSessionObjectGeneration,
            request.BoundSessionAuthorityOwnerGeneration,
            request.BoundSessionMeshName,
            request.BoundSessionTargetNodeGeneration,
            request.BoundSessionOwnerLeaseGeneration,
            request.BoundSessionOwnerNodeGeneration,
            request.BoundSessionAcceptedHighWater,
            request.BoundSessionSessionOwnerId,
            request.BoundSessionSessionOwnerLeaseGeneration);
    }

    internal static ZLinkSessionRelocationContext
        DecodeSessionRelocationContext(ZLinkRemoteActorJoinRequest request)
    {
        if (!request.RelocationAggregateId.Equals(Guid.Empty)
            && !string.IsNullOrWhiteSpace(
                request.RelocationCoordinatorOwnerId)
            && request.RelocationCoordinatorLeaseGeneration != 0
            && request.RelocationCoordinatorNodeRid is { Length: > 0 }
            && request.RelocationCoordinatorNodeGeneration != 0
            && !string.IsNullOrWhiteSpace(
                request.RelocationCoordinatorExpectedAuthorityStoreVersion))
            return ZLinkSessionRelocationContext.Create(
                request.RelocationAggregateId,
                request.RelocationCoordinatorOwnerId,
                request.RelocationCoordinatorLeaseGeneration,
                RoutingId.From(request.RelocationCoordinatorNodeRid),
                request.RelocationCoordinatorNodeGeneration,
                request.RelocationCoordinatorExpectedAuthorityStoreVersion);
        return default;
    }

    internal static ZLinkSessionRelocationContext
        DecodeSessionRelocationContext(
            ZLinkRemoteActorHandoffCompletionRequest request)
    {
        if (Guid.TryParseExact(request.HandoffId, "N", out var relocationId)
            && !string.IsNullOrWhiteSpace(
                request.RelocationCoordinatorOwnerId)
            && request.RelocationCoordinatorLeaseGeneration != 0
            && request.RelocationCoordinatorNodeRid is { Length: > 0 }
            && request.RelocationCoordinatorNodeGeneration != 0
            && !string.IsNullOrWhiteSpace(
                request.RelocationCoordinatorExpectedAuthorityStoreVersion))
            return ZLinkSessionRelocationContext.Create(
                relocationId,
                request.RelocationCoordinatorOwnerId,
                request.RelocationCoordinatorLeaseGeneration,
                RoutingId.From(request.RelocationCoordinatorNodeRid),
                request.RelocationCoordinatorNodeGeneration,
                request.RelocationCoordinatorExpectedAuthorityStoreVersion);
        return default;
    }

    public static ZLinkRemoteActorBoundSessionRoute DecodeBoundSessionRoute(
        ZLinkRemoteActorHandoffCompletionRequest request) =>
        new(
            ToRoutingId(request.BoundSessionNodeRid),
            ToRoutingId(request.BoundSessionRid),
            request.BoundSessionBindingToken,
            request.BoundSessionBindingGeneration,
            request.BoundSessionObjectGeneration,
            request.BoundSessionAuthorityOwnerGeneration,
            request.BoundSessionMeshName,
            request.BoundSessionTargetNodeGeneration,
            request.BoundSessionOwnerLeaseGeneration,
            request.BoundSessionOwnerNodeGeneration,
            request.BoundSessionAcceptedHighWater,
            request.BoundSessionSessionOwnerId,
            request.BoundSessionSessionOwnerLeaseGeneration);

    public static ZLinkMessage DecodeJoinRequestPayload(
        ZLinkRemoteActorJoinRequest request,
        ZLinkCodecRegistryBuilder codecs)
    {
        return DecodeJoinRequestPayload(
            request.RequestContentType,
            request.Request,
            codecs);
    }

    public static ZLinkMessage DecodeAdmissionRequestPayload(
        ZLinkRemoteActorAdmissionRequest request,
        ZLinkCodecRegistryBuilder codecs)
    {
        return DecodeJoinRequestPayload(
            request.RequestContentType,
            request.Request,
            codecs);
    }

    private static ZLinkMessage DecodeJoinRequestPayload(
        string requestContentType,
        byte[] requestPayload,
        ZLinkCodecRegistryBuilder codecs)
    {
        using var payload = Message.From(requestPayload);
        return ZLinkMessage.FromEnvelopePayload(
            requestContentType,
            payload,
            codecs);
    }

    public static ZLinkRemoteActorJoinReply CreateJoinReply(
        bool accepted,
        ZLinkBackendActorRef actorRef)
    {
        return new ZLinkRemoteActorJoinReply(
            accepted,
            actorRef.NodeRid.ToBytes().ToArray(),
            actorRef.ActorId,
            actorRef.Generation);
    }

    public static ZLinkRemoteActorAdmissionReply CreateAdmissionReply(
        bool accepted,
        ZLinkMessage? reply,
        ZLinkCodecRegistryBuilder codecs,
        long deadlineUnixTimeMilliseconds = 0,
        ZLinkActorRelocationReservation? reservation = null,
        ulong receiveChunkLimitBytes = 0)
    {
        var replyContentType = ZLinkEnvelopeCodec.DefaultContentType;
        Message? encodedReply = null;
        if (reply is not null)
        {
            var encoded = reply.Encode(codecs);
            replyContentType = encoded.ContentType;
            encodedReply = Message.From(encoded.Payload.Bytes.Span);
        }

        using (encodedReply)
        {
            return new ZLinkRemoteActorAdmissionReply(
                accepted,
                replyContentType,
                encodedReply?.ToArray() ?? Array.Empty<byte>(),
                deadlineUnixTimeMilliseconds,
                reservation?.Token ?? "",
                reservation?.ReservedPayloadBytes ?? 0,
                reservation?.TargetNodeRid.ToBytes().ToArray(),
                reservation?.TargetNodeGeneration ?? 0,
                reservation?.TargetSpotGeneration ?? 0,
                reservation?.TargetAuthorityOwnerGeneration ?? 0,
                reservation?.TargetSpotAuthorityOwnerGeneration ?? 0,
                receiveChunkLimitBytes);
        }
    }

    public static IReadOnlyList<Message> EncodeJoinReplyEnvelope(
        string channelName,
        string messageName,
        string? correlationId,
        ZLinkRemoteActorJoinReply reply)
    {
        return ZLinkSpotReplyEnvelope.EncodeResponseParts(
            channelName,
            messageName,
            correlationId,
            reply,
            typeof(ZLinkRemoteActorJoinReply));
    }

    public static ZLinkRemoteActorJoinReply DecodeJoinReplyAndDispose(
        IReadOnlyList<Message> parts,
        string actorId,
        string targetSpotId)
    {
        return ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<ZLinkRemoteActorJoinReply>(
            parts,
            "Remote actor join reply was empty.",
            $"Remote actor join failed for '{actorId}' to SPOT '{targetSpotId}'.",
            null);
    }

    public static ZLinkRemoteActorAdmissionReply DecodeAdmissionReplyAndDispose(
        IReadOnlyList<Message> parts,
        string actorId,
        string targetSpotId)
    {
        return ZLinkClientCallCodec.DecodeEnvelopeReplyAndDispose<ZLinkRemoteActorAdmissionReply>(
            parts,
            "Remote actor admission reply was empty.",
            $"Remote actor admission failed for '{actorId}' to SPOT '{targetSpotId}'.",
            null);
    }

    public static ZLinkMessage DecodeAdmissionReplyPayload(
        ZLinkRemoteActorAdmissionReply reply,
        ZLinkCodecRegistryBuilder codecs)
    {
        using var payload = Message.From(reply.Reply);
        return ZLinkMessage.FromEnvelopePayload(
            reply.ReplyContentType,
            payload,
            codecs);
    }

    public static ZLinkBackendActorRef ToActorRef(ZLinkRemoteActorJoinReply reply)
    {
        return new ZLinkBackendActorRef(
            RoutingId.From(reply.ActorNodeRid),
            reply.ActorId,
            reply.ActorGeneration);
    }

    private static RoutingId? ToRoutingId(byte[]? bytes)
    {
        return bytes is { Length: > 0 } ? RoutingId.From(bytes) : null;
    }
}

internal readonly record struct ZLinkRemoteActorBoundSessionRoute(
    RoutingId? NodeRid,
    RoutingId? SessionRid,
    string? BindingToken,
    ulong BindingGeneration,
    ulong ObjectGeneration,
    ulong AuthorityOwnerGeneration,
    string? MeshName,
    ulong TargetNodeGeneration,
    ulong OwnerLeaseGeneration,
    ulong SessionOwnerNodeGeneration,
    ulong AcceptedHighWater,
    string? SessionOwnerId = null,
    ulong SessionOwnerLeaseGeneration = 0)
{
    internal bool HasRouteCoordinates => NodeRid is not null || SessionRid is not null;

    internal bool IsBound =>
        NodeRid is not null
        && SessionRid is not null
        && !string.IsNullOrEmpty(BindingToken)
        && BindingGeneration > 0
        && ObjectGeneration > 0
        && AuthorityOwnerGeneration > 0
        && !string.IsNullOrWhiteSpace(MeshName)
        && TargetNodeGeneration > 0
        && OwnerLeaseGeneration > 0
        && SessionOwnerNodeGeneration > 0;
}

internal sealed record ZLinkRemoteActorAdmissionRequest(
    string ActorId,
    string ActorType,
    string SourceSpotId,
    byte[] SourceNodeRid,
    string RequestContentType,
    byte[] Request,
    string HandoffId = "",
    long DeadlineUnixTimeMilliseconds = 0,
    ulong ActorGeneration = 0,
    ulong ActorAuthorityOwnerGeneration = 0,
    long PredictedPayloadBytes = 0,
    ulong TargetSpotGeneration = 0,
    ulong TargetSpotAuthorityOwnerGeneration = 0,
    [property: System.Text.Json.Serialization.JsonIgnore]
    ZLinkCanonicalActorJoinAdmission? Canonical = null);

internal sealed record ZLinkRemoteActorAdmissionReply(
    bool Accepted,
    string ReplyContentType,
    byte[] Reply,
    long DeadlineUnixTimeMilliseconds = 0,
    string ReservationToken = "",
    long ReservedPayloadBytes = 0,
    byte[]? TargetNodeRid = null,
    ulong TargetNodeGeneration = 0,
    ulong TargetSpotGeneration = 0,
    ulong TargetAuthorityOwnerGeneration = 0,
    ulong TargetSpotAuthorityOwnerGeneration = 0,
    ulong ReceiveChunkLimitBytes = 0);

internal sealed record ZLinkRemoteActorAdmissionAbortRequest(
    string ActorId,
    string HandoffId,
    string ReservationToken);

internal sealed record ZLinkRemoteActorJoinRequest(
string ActorId,
string ActorType,
string HandoffId,
byte[]? BoundSessionNodeRid,
byte[]? BoundSessionRid,
string RelocationContentType,
string RelocationReference,
uint RelocationChecksumCrc32c,
[property: System.Text.Json.Serialization.JsonConverter(
    typeof(ZLinkCanonicalGuidJsonConverter))]
Guid RelocationAggregateId,
ulong RelocationAggregateGeneration,
byte[] RelocationInventoryDigest,
string RequestContentType,
byte[] Request,
IReadOnlyList<ZLinkActorHandoffFrame> HandoffFrames,
string SourceSpotId,
byte[] SourceNodeRid,
ulong ActorGeneration,
ulong ActorAuthorityOwnerGeneration,
string? BoundSessionBindingToken = null,
ulong BoundSessionBindingGeneration = 0,
ulong BoundSessionObjectGeneration = 0,
ulong BoundSessionAuthorityOwnerGeneration = 0,
string? BoundSessionMeshName = null,
ulong BoundSessionTargetNodeGeneration = 0,
    ulong BoundSessionOwnerLeaseGeneration = 0,
    ulong BoundSessionOwnerNodeGeneration = 0,
    ulong BoundSessionAcceptedHighWater = 0,
    string? BoundSessionSessionOwnerId = null,
    ulong BoundSessionSessionOwnerLeaseGeneration = 0,
    string ReservationToken = "",
long ReservedPayloadBytes = 0,
byte[]? TargetNodeRid = null,
ulong TargetNodeGeneration = 0,
ulong TargetSpotGeneration = 0,
ulong TargetAuthorityOwnerGeneration = 0,
ulong TargetSpotAuthorityOwnerGeneration = 0,
string RelocationCoordinatorOwnerId = "",
ulong RelocationCoordinatorLeaseGeneration = 0,
byte[]? RelocationCoordinatorNodeRid = null,
ulong RelocationCoordinatorNodeGeneration = 0,
string RelocationCoordinatorExpectedAuthorityStoreVersion = "",
ulong ActorNodeGeneration = 0,
ulong ExpectedOwnerLeaseGeneration = 0);

internal readonly record struct ZLinkActorRelocationReservation(
    string Token,
    long ReservedPayloadBytes,
    RoutingId TargetNodeRid,
    ulong TargetNodeGeneration,
    ulong TargetSpotGeneration,
    ulong TargetAuthorityOwnerGeneration,
    ulong TargetSpotAuthorityOwnerGeneration,
    ulong ReceiveChunkLimitBytes = 0);

internal sealed record ZLinkRemoteActorJoinReply(
    bool Accepted,
    byte[] ActorNodeRid,
    string ActorId,
    ulong ActorGeneration);

internal sealed record ZLinkRemoteActorHandoffCompletionRequest(
    string ActorId,
    string HandoffId,
    string SourceSpotId,
    byte[] SourceNodeRid,
    string TargetSpotId,
    IReadOnlyList<ZLinkActorHandoffFrame> Frames,
    ulong OperationIdHigh = 0,
    ulong OperationIdLow = 0,
    string? ReplyContentType = null,
    byte[]? Reply = null,
    byte[]? BoundSessionNodeRid = null,
    byte[]? BoundSessionRid = null,
    string? BoundSessionBindingToken = null,
    ulong BoundSessionBindingGeneration = 0,
    ulong BoundSessionObjectGeneration = 0,
    ulong BoundSessionAuthorityOwnerGeneration = 0,
    string? BoundSessionMeshName = null,
    ulong BoundSessionTargetNodeGeneration = 0,
    ulong BoundSessionOwnerLeaseGeneration = 0,
    ulong BoundSessionOwnerNodeGeneration = 0,
    ulong BoundSessionAcceptedHighWater = 0,
    string? BoundSessionSessionOwnerId = null,
    ulong BoundSessionSessionOwnerLeaseGeneration = 0,
    string RelocationCoordinatorOwnerId = "",
    ulong RelocationCoordinatorLeaseGeneration = 0,
    byte[]? RelocationCoordinatorNodeRid = null,
    ulong RelocationCoordinatorNodeGeneration = 0,
    string RelocationCoordinatorExpectedAuthorityStoreVersion = "");
