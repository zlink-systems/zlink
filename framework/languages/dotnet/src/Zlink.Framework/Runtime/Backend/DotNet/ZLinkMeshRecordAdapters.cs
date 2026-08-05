using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet.Mappings;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// Adapts 10.0.0 MeshReceiveRecord payloads into the framework-owned seam objects
// the actor/lifecycle/join plane already consumes.
internal static class ZLinkMeshRecordAdapters
{
    public static ZLinkBackendActorJoinRequest ToActorJoinRequest(
        MeshReceiveBatch batch, int index, MeshReceiveRecord record)
    {
        IReadOnlyList<Message> parts = batch.RetainMessage(index);
        var message = parts.Count > 0 ? parts[0] : Message.From(ReadOnlySpan<byte>.Empty);
        var epoch = record.ActorControl?.CurrentMembershipEpoch ?? 0;
        // Locally submitted joins leave the record's SourceActor zero-filled;
        // the control payload names the joining actor.
        var joiningActor = record.SourceActor.ActorId is { Length: > 0 }
            ? record.SourceActor
            : record.ActorControl?.CurrentActor ?? record.SourceActor;
        return new ZLinkMeshActorJoinRequest(
            joiningActor.ToBackend(),
            joiningActor.ToBackend(),
            record.SourceNodeRid,
            record.SourceSpotId,
            epoch,
            message,
            parts,
            record);
    }

    public static ZLinkBackendSpotActorLifecycleEvent? ToLifecycleEvent(
        ActorControlRecord control)
    {
        var kind = control.Kind switch
        {
            ActorLifecycleKind.Joined => ZLinkBackendActorLifecycleEventKind.Joined,
            ActorLifecycleKind.Left => ZLinkBackendActorLifecycleEventKind.Left,
            ActorLifecycleKind.Disconnected => ZLinkBackendActorLifecycleEventKind.Disconnected,
            _ => (ZLinkBackendActorLifecycleEventKind?)null
        };
        if (kind is not { } lifecycleKind) return null;

        var info = new ZLinkBackendSpotActorLifecycleInfo(
            control.PreviousActor.ActorId is { Length: > 0 }
                ? control.PreviousActor.ToBackend()
                : null,
            control.CurrentActor.ActorId is { Length: > 0 }
                ? control.CurrentActor.ToBackend()
                : null,
            control.PreviousSpotId,
            control.CurrentSpotId,
            control.CurrentMembershipEpoch,
            0);
        return new ZLinkBackendSpotActorLifecycleEvent(lifecycleKind, info);
    }

    public static IReadOnlyList<ZLinkBackendActorPart> ToActorParts(
        MeshReceiveBatch batch, int index, MeshReceiveRecord record,
        ActorRef ownerActor = default, ulong requestId = 0,
        ZLinkServiceWireCodec.RequestSourceFence? requestSource = null,
        Func<IReadOnlyList<Message>, SendFlags, SubmitResult>? directReply = null)
    {
        IReadOnlyList<Message> messages = batch.RetainMessage(index);
        if (messages.Count == 0) return Array.Empty<ZLinkBackendActorPart>();

        // Session-relayed sends leave the record's SourceActor empty (the sender
        // is an external session, not an actor); the target actor is the claim
        // owner's identity from the ready record.
        var actorRef = record.SourceActor.ActorId is { Length: > 0 }
            ? record.SourceActor
            : ownerActor;
        if (actorRef.ActorId is not { Length: > 0 })
        {
            ZLinkMessageParts.DisposeAll(messages);
            return Array.Empty<ZLinkBackendActorPart>();
        }

        var actor = actorRef.ToBackend();
        // RequestId is the source-allocated reply route (wire correlation).
        // OperationId remains the independent durable dedupe identity.
        var flags = record.Kind == MeshRecordKind.ActorRequest ? 1u : 0u;
        var parts = new ZLinkBackendActorPart[messages.Count];
        var sourceSpotId = string.IsNullOrEmpty(record.SourceSpotId)
            ? default
            : ZLinkSpotId.ToNativeRoutingId(record.SourceSpotId);
        for (var i = 0; i < parts.Length; i++)
            parts[i] = new ZLinkBackendActorPart(
                actor,
                record.SourceNodeRid,
                sourceSpotId,
                requestId,
                flags,
                messages[i],
                i + 1 < parts.Length,
                RouteContext: new ZLinkBackendActorRouteContext(
                    record.OperationId,
                    record.MessageFollowHopCount,
                    record.TargetNodeGeneration,
                    record.AuthorityOwnerGeneration,
                    record.OwnerLeaseGeneration,
                    requestId,
                    flags,
                    // Core uses ulong.MaxValue for an unbounded operation.
                    // Framework wire contracts use 0 for the same meaning.
                    DeadlineUnixMs: NormalizeDeadline(record.DeadlineUnixMs)),
                SourceNodeGeneration: record.SourceBindingGeneration,
                RequestSource: requestSource,
                DirectReply: i == 0 ? directReply : null,
                ApplicationMetadata:
                    record.ApplicationMetadata ?? ReadOnlyMemory<byte>.Empty);
        return parts;
    }

    internal static ulong NormalizeDeadline(ulong deadlineUnixMs)
    {
        if (deadlineUnixMs <= long.MaxValue)
            return deadlineUnixMs;
        if (deadlineUnixMs == ulong.MaxValue)
            return 0;
        throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.ProtocolError,
            $"Actor message deadline '{deadlineUnixMs}' is outside the supported range.");
    }
}

// Framework actor-join request that carries the originating pull-dispatch record
// so ReplyActorJoin can route the admission reply through MeshReceiveRecord.
internal sealed class ZLinkMeshActorJoinRequest(
    ZLinkBackendActorRef sourceActor,
    ZLinkBackendActorRef targetActor,
    RoutingId sourceNodeRid,
    string targetSpotId,
    ulong joinEpoch,
    Message message,
    IReadOnlyList<Message> parts,
    MeshReceiveRecord record)
    : ZLinkBackendActorJoinRequest(
        sourceActor,
        targetActor,
        sourceNodeRid,
        targetSpotId,
        joinEpoch,
        message,
        parts)
{
    private readonly MeshReceiveRecord _record = record;

    public SubmitResult ReplyJoin(int joinResultCode, IReadOnlyList<Message> parts)
    {
        var result = joinResultCode == 0
            ? ActorJoinResult.Accepted
            : ActorJoinResult.Rejected;
        return _record.ReplyJoin(result, parts);
    }
}
