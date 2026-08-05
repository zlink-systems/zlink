namespace Zlink.Framework.Runtime.Actors;

internal enum ZLinkActorFrameRoute
{
    Current,
    MessageFollow,
    MessageFollowExpired,
    Stale
}

internal enum ZLinkActorHandoffCaptureResult
{
    NotSealed,
    Captured,
    Full
}

internal sealed class ZLinkActorHandoffRejectedException(
    string message,
    Exception? innerException = null) : InvalidOperationException(message, innerException);

internal sealed record ZLinkActorBoundSessionHandoffFence(
    string ActorId,
    ulong ActorGeneration,
    RoutingId SessionRid,
    string BindingToken,
    ulong BindingGeneration,
    ulong SessionSequence);

internal sealed record ZLinkActorHandoffFrame(
    byte[] ReplyActorNodeRid,
    ulong ReplyActorGeneration,
    byte[] SourceNodeRid,
    byte[] SourceSessionRid,
    ulong RequestId,
    uint Flags,
    byte[] Header,
    byte[] Body,
    long ArrivalIndex,
    ZLinkBackendActorRouteContext RouteContext = default,
    ulong SourceNodeGeneration = 0,
    ZLinkServiceWireCodec.RequestSourceFence? RequestSource = null,
    ulong RelocationReplyRouteId = 0,
    long CanonicalEncodedLength = 0,
    ZLinkActorBoundSessionHandoffFence? BoundSessionSource = null);

internal sealed record ZLinkActorAcceptedRecord(
    ZLinkActorHandoffFrame Frame,
    ZLinkServiceWireCodec.RequestSourceFence RequestSource,
    ZLinkBackendActorRef? FrozenTargetActor = null);

internal readonly record struct ZLinkActorHandoffCommitBoundary(
    IReadOnlyList<ZLinkActorHandoffFrame> Frames,
    ulong AcceptedHighWater,
    int RemainingHoldRecords,
    long RemainingHoldBytes);

internal static class ZLinkActorHandoffFrames
{

    public static ZLinkActorHandoffFrame Capture(
        ZLinkSpotActorFrame frame,
        long arrivalIndex)
    {
        // Bound-session requests do not use the direct reply-preservation
        // registry. Their existing ReplyRequestId is the stable correlation
        // needed when the accepted frame is frozen for target replay.
        var relocationReplyRouteId = frame.RelocationReplyRouteId;
        if (relocationReplyRouteId == 0
            && (frame.Flags & 1U) != 0
            && frame.RouteContext.IsBoundSessionRoute)
            relocationReplyRouteId = frame.RouteContext.ReplyRequestId;

        return new ZLinkActorHandoffFrame(
            frame.ReplyActor.NodeRid.ToBytes().ToArray(),
            frame.ReplyActor.Generation,
            frame.SourceNodeRid.ToBytes().ToArray(),
            frame.SourceSessionRid.ToBytes().ToArray(),
            frame.RequestId,
            frame.Flags,
            ZLinkStreamProtocolDefaults.EncodeHeader(frame.Header).ToArray(),
            frame.Body.ToArray(),
            arrivalIndex,
            frame.RouteContext,
            frame.SourceNodeGeneration,
            frame.RequestSource,
            relocationReplyRouteId,
            BoundSessionSource: ZLinkActorBoundSessionHandoffMetadata.TryDecode(
                frame.ApplicationMetadata.Span,
                out var boundSession)
                ? boundSession
                : null);
    }

    internal static long CanonicalEncodedLength(
        ZLinkActorHandoffFrame frame,
        ZLinkBackendActorRef targetActor)
    {
        ArgumentNullException.ThrowIfNull(frame);
        var source = frame.RequestSource
                     ?? throw new ZLinkActorHandoffRejectedException(
                         "Canonical Actor ingress requires a request-source fence.");
        return ZLinkCanonicalActorAcceptedJournal.Encode(
            new ZLinkActorAcceptedRecord(frame, source, targetActor),
            targetActor).LongLength;
    }

    private static RoutingId RidOrDefault(byte[] rid) =>
        rid is { Length: > 0 } ? RoutingId.From(rid) : default;

    public static ZLinkSpotActorFrameBatch Restore(
        ZLinkBackendActorRef actor,
        IReadOnlyList<ZLinkActorHandoffFrame> frames)
    {
        var restored = new List<ZLinkSpotActorFrame>(frames.Count);
        try
        {
            foreach (var frame in frames.OrderBy(static frame => frame.ArrivalIndex))
            {
                var replyActor = new ZLinkBackendActorRef(
                    RidOrDefault(frame.ReplyActorNodeRid),
                    actor.ActorId,
                    frame.ReplyActorGeneration);
                var restoredFrame = new ZLinkSpotActorFrame(
                    actor,
                    replyActor,
                    RidOrDefault(frame.SourceNodeRid),
                    // A caller-routed frame carries no session identity.
                    RidOrDefault(frame.SourceSessionRid),
                    frame.RequestId,
                    frame.Flags,
                    frame.RouteContext,
                    ZLinkStreamProtocolDefaults.DecodeHeader(frame.Header),
                    Message.From(frame.Body),
                    frame.SourceNodeGeneration,
                    frame.RequestSource,
                    applicationMetadata: frame.BoundSessionSource is { } bound
                        ? ZLinkActorBoundSessionHandoffMetadata.Encode(bound)
                        : default,
                    handoffArrivalIndex: frame.ArrivalIndex);
                if (frame.RelocationReplyRouteId != 0)
                    restoredFrame.BindRelocationReplyRoute(
                        frame.RelocationReplyRouteId);
                restored.Add(restoredFrame);
            }

            return new ZLinkSpotActorFrameBatch(restored);
        }
        catch
        {
            foreach (var frame in restored) frame.Dispose();
            throw;
        }
    }

    internal static ZLinkSpotActorFrameBatch RestoreCanonical(
        ZLinkBackendActorRef currentActor,
        ZLinkBackendActorRef expectedSourceActor,
        IReadOnlyList<ZLinkActorAcceptedRecord> accepted)
    {
        if (currentActor.ActorId != expectedSourceActor.ActorId
            || currentActor.Generation != expectedSourceActor.Generation)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor relocation changed the logical Actor generation.");
        foreach (var record in accepted)
        {
            var frozen = record.FrozenTargetActor
                         ?? throw new ZLinkRelocationDataLostException(
                             "Standalone Actor frozen record lost its source target fence.");
            if (frozen != expectedSourceActor)
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor frozen record does not match the relocation source fence.");
        }

        // The immutable frozen records keep the source target fence. Only this
        // replay endpoint, after validating that fence against the root, binds
        // dispatch to the materialized target instance.
        return Restore(
            currentActor,
            accepted.Select(static record => record.Frame).ToArray());
    }
}
