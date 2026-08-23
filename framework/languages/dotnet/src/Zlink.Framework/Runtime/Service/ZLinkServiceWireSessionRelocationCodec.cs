using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Service;

internal static partial class ZLinkServiceWireCodec
{
    internal enum SessionRelocationRouteAction : byte
    {
        Commit = 1,
        Abort = 2
    }

    internal readonly record struct SessionActorIdentityRecord(
        string ActorId,
        ulong ObjectGeneration);

    internal readonly record struct SessionActorRouteFenceRecord(
        SessionActorIdentityRecord Actor,
        RoutingId TargetNodeRid,
        ulong TargetNodeGeneration,
        ulong AuthorityOwnerGeneration,
        ulong OwnerLeaseGeneration);

    internal readonly record struct SessionOwnerFenceRecord(
        RoutingId SessionOwnerNodeRid,
        ulong SessionOwnerNodeGeneration,
        string SessionOwnerId,
        ulong SessionOwnerLeaseGeneration,
        RoutingId SessionRid,
        ulong BindingGeneration);

    internal readonly record struct SessionRelocationSealRecord(
        RelocationWireId RelocationId,
        RelocationCoordinatorFence Coordinator,
        byte SenderRole,
        SessionActorRouteFenceRecord Actor,
        SessionOwnerFenceRecord Session);

    internal readonly record struct SessionRelocationSealedRecord(
        RelocationWireId RelocationId,
        RelocationCoordinatorFence Coordinator,
        SessionActorRouteFenceRecord Actor,
        SessionOwnerFenceRecord Session);

    internal readonly record struct SessionRelocationRouteUpdateRecord(
        SessionRelocationRouteAction Action,
        ulong PreviousAuthorityOwnerGeneration,
        ulong TargetAuthorityOwnerGeneration,
        RoutingId TargetNodeRid,
        ulong TargetNodeGeneration,
        ulong CurrentAuthorityOwnerGeneration)
    {
        internal static SessionRelocationRouteUpdateRecord Commit(
            ulong previousAuthorityOwnerGeneration,
            ulong targetAuthorityOwnerGeneration,
            RoutingId targetNodeRid,
            ulong targetNodeGeneration) =>
            new(
                SessionRelocationRouteAction.Commit,
                previousAuthorityOwnerGeneration,
                targetAuthorityOwnerGeneration,
                targetNodeRid,
                targetNodeGeneration,
                0);

        internal static SessionRelocationRouteUpdateRecord Abort(
            ulong currentAuthorityOwnerGeneration) =>
            new(
                SessionRelocationRouteAction.Abort,
                0,
                0,
                default,
                0,
                currentAuthorityOwnerGeneration);
    }

    internal readonly record struct SessionRelocationRouteRecord(
        RelocationWireId RelocationId,
        RelocationCoordinatorFence Coordinator,
        byte SenderRole,
        SessionActorIdentityRecord Actor,
        SessionOwnerFenceRecord Session,
        SessionRelocationRouteUpdateRecord Route);

    internal static byte[] EncodeSessionRelocationSeal(
        SessionRelocationSealRecord record)
    {
        ValidateSessionRelocationCommon(record.RelocationId,
            record.Coordinator);
        if (record.SenderRole != 1)
            throw new ArgumentOutOfRangeException(nameof(record));
        ValidateSessionActorRouteFence(record.Actor);
        ValidateSessionOwnerFence(record.Session);
        return ServiceWirePilotCodec.EncodeSessionRelocationSeal42(new(
            ToGenerated(record.RelocationId),
            ToGenerated(record.Coordinator),
            ServiceWirePilotCodec.RelocationRole.Source,
            ToGenerated(record.Actor),
            ToGenerated(record.Session)));
    }

    internal static bool TryDecodeSessionRelocationSeal(
        ReadOnlySpan<byte> bytes, out SessionRelocationSealRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.SessionRelocationSeal,
                ServiceWirePilotCodec.DecodeSessionRelocationSeal42,
                out var generated, out error))
            return false;
        record = new SessionRelocationSealRecord(
            FromGenerated(generated.Relocation),
            FromGenerated(generated.Coordinator),
            FromGeneratedRole(generated.SenderRole),
            FromGeneratedActorFence(generated.Actor),
            FromGenerated(generated.Session));
        return true;
    }

    internal static byte[] EncodeSessionRelocationSealed(
        SessionRelocationSealedRecord record)
    {
        ValidateSessionRelocationCommon(record.RelocationId,
            record.Coordinator);
        ValidateSessionActorRouteFence(record.Actor);
        ValidateSessionOwnerFence(record.Session);
        return ServiceWirePilotCodec.EncodeSessionRelocationSealed43(new(
            ToGenerated(record.RelocationId),
            ToGenerated(record.Coordinator),
            ToGenerated(record.Actor),
            ToGenerated(record.Session)));
    }

    internal static bool TryDecodeSessionRelocationSealed(
        ReadOnlySpan<byte> bytes, out SessionRelocationSealedRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.SessionRelocationSealed,
                ServiceWirePilotCodec.DecodeSessionRelocationSealed43,
                out var generated, out error))
            return false;
        record = new SessionRelocationSealedRecord(
            FromGenerated(generated.Relocation),
            FromGenerated(generated.Coordinator),
            FromGeneratedActorFence(generated.Actor),
            FromGenerated(generated.Session));
        return true;
    }

    internal static byte[] EncodeSessionRelocationRoute(
        SessionRelocationRouteRecord record)
    {
        ValidateSessionRelocationCommon(record.RelocationId,
            record.Coordinator);
        ValidateSessionActor(record.Actor);
        ValidateSessionOwnerFence(record.Session);
        ValidateSessionRelocationRoute(record.SenderRole, record.Route);
        return ServiceWirePilotCodec.EncodeSessionRelocationRoute44(new(
            ToGenerated(record.RelocationId),
            ToGenerated(record.Coordinator),
            ToGeneratedRole(record.SenderRole),
            new ServiceWirePilotCodec.ActorRef(
                record.Actor.ActorId,
                record.Actor.ObjectGeneration),
            ToGenerated(record.Session),
            ToGenerated(record.Route)));
    }

    internal static bool TryDecodeSessionRelocationRoute(
        ReadOnlySpan<byte> bytes, out SessionRelocationRouteRecord record,
        out DecodeError error)
    {
        record = default;
        if (!TryDecodeGenerated(bytes,
                ServiceWireConstants.Command.SessionRelocationRoute,
                ServiceWirePilotCodec.DecodeSessionRelocationRoute44,
                out var generated, out error))
            return false;
        record = new SessionRelocationRouteRecord(
            FromGenerated(generated.Relocation),
            FromGenerated(generated.Coordinator),
            FromGeneratedRole(generated.SenderRole),
            new SessionActorIdentityRecord(
                generated.Actor.ActorId,
                generated.Actor.ObjectGeneration),
            FromGenerated(generated.Session),
            FromGenerated(generated.Route));
        return true;
    }

    private static void ValidateSessionRelocationCommon(
        RelocationWireId relocation, RelocationCoordinatorFence coordinator)
    {
        if (relocation.IsEmpty)
            throw new ArgumentOutOfRangeException(nameof(relocation));
        ValidateCoordinator(coordinator);
    }

    private static void ValidateSessionActor(SessionActorIdentityRecord actor)
    {
        if (string.IsNullOrEmpty(actor.ActorId) || actor.ObjectGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(actor));
    }

    private static void ValidateSessionActorRouteFence(
        SessionActorRouteFenceRecord actor)
    {
        ValidateSessionActor(actor.Actor);
        if (actor.TargetNodeRid.IsEmpty || actor.TargetNodeGeneration == 0
            || actor.AuthorityOwnerGeneration == 0
            || actor.OwnerLeaseGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(actor));
    }

    private static void ValidateSessionOwnerFence(
        SessionOwnerFenceRecord session)
    {
        if (session.SessionOwnerNodeRid.IsEmpty
            || session.SessionOwnerNodeGeneration == 0
            || string.IsNullOrEmpty(session.SessionOwnerId)
            || session.SessionOwnerLeaseGeneration == 0
            || session.SessionRid.IsEmpty
            || session.BindingGeneration == 0)
            throw new ArgumentOutOfRangeException(nameof(session));
    }

    private static void ValidateSessionRelocationRoute(byte senderRole,
        SessionRelocationRouteUpdateRecord route)
    {
        if (route.Action == SessionRelocationRouteAction.Commit)
        {
            if (senderRole != 2
                || route.PreviousAuthorityOwnerGeneration == 0
                || route.TargetAuthorityOwnerGeneration
                   <= route.PreviousAuthorityOwnerGeneration
                || route.TargetNodeRid.IsEmpty
                || route.TargetNodeGeneration == 0
                || route.CurrentAuthorityOwnerGeneration != 0)
                throw new ArgumentOutOfRangeException(nameof(route));
            return;
        }
        if (route.Action != SessionRelocationRouteAction.Abort
            || senderRole != 1
            || route.CurrentAuthorityOwnerGeneration == 0
            || route.PreviousAuthorityOwnerGeneration != 0
            || route.TargetAuthorityOwnerGeneration != 0
            || !route.TargetNodeRid.IsEmpty
            || route.TargetNodeGeneration != 0)
            throw new ArgumentOutOfRangeException(nameof(route));
    }

    private static ServiceWirePilotCodec.Fence ToGenerated(
        SessionActorRouteFenceRecord value) => new(
        value.Actor.ActorId,
        value.Actor.ObjectGeneration,
        value.TargetNodeRid.ToBytes().ToArray(),
        value.TargetNodeGeneration,
        value.AuthorityOwnerGeneration,
        value.OwnerLeaseGeneration);

    private static SessionActorRouteFenceRecord FromGeneratedActorFence(
        ServiceWirePilotCodec.Fence value) => new(
        new SessionActorIdentityRecord(value.Id, value.Generation),
        RoutingId.From(value.TargetNodeRid),
        value.TargetNodeGeneration,
        value.ExpectedAuthorityOwnerGeneration,
        value.ExpectedOwnerLeaseGeneration);

    private static ServiceWirePilotCodec.SessionIdentity ToGenerated(
        SessionOwnerFenceRecord value) => new(
        value.SessionOwnerNodeRid.ToBytes().ToArray(),
        value.SessionOwnerNodeGeneration,
        value.SessionOwnerId,
        value.SessionOwnerLeaseGeneration,
        value.SessionRid.ToBytes().ToArray(),
        value.BindingGeneration);

    private static SessionOwnerFenceRecord FromGenerated(
        ServiceWirePilotCodec.SessionIdentity value) => new(
        RoutingId.From(value.SessionOwnerNodeRid),
        value.SessionOwnerNodeGeneration,
        value.SessionOwnerId,
        value.SessionOwnerLeaseGeneration,
        RoutingId.From(value.SessionRid),
        value.BindingGeneration);

    private static ServiceWirePilotCodec.SessionRouteUpdate ToGenerated(
        SessionRelocationRouteUpdateRecord value) => value.Action switch
    {
        SessionRelocationRouteAction.Commit =>
            new ServiceWirePilotCodec.SessionRouteCommit(
                value.PreviousAuthorityOwnerGeneration,
                value.TargetAuthorityOwnerGeneration,
                value.TargetNodeRid.ToBytes().ToArray(),
                value.TargetNodeGeneration),
        SessionRelocationRouteAction.Abort =>
            new ServiceWirePilotCodec.SessionRouteAbort(
                value.CurrentAuthorityOwnerGeneration),
        _ => throw new ArgumentOutOfRangeException(nameof(value))
    };

    private static SessionRelocationRouteUpdateRecord FromGenerated(
        ServiceWirePilotCodec.SessionRouteUpdate value) => value switch
    {
        ServiceWirePilotCodec.SessionRouteCommit commit =>
            SessionRelocationRouteUpdateRecord.Commit(
                commit.PreviousAuthorityOwnerGeneration,
                commit.TargetAuthorityOwnerGeneration,
                RoutingId.From(commit.TargetNodeRid),
                commit.TargetNodeGeneration),
        ServiceWirePilotCodec.SessionRouteAbort abort =>
            SessionRelocationRouteUpdateRecord.Abort(
                abort.CurrentAuthorityOwnerGeneration),
        _ => throw new InvalidDataException("Unknown Session relocation route.")
    };
}
