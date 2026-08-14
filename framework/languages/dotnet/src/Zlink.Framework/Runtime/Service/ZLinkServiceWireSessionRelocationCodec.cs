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
        if (record.SenderRole is not (1 or 3))
            throw new ArgumentOutOfRangeException(nameof(record));
        ValidateSessionActorRouteFence(record.Actor);
        ValidateSessionOwnerFence(record.Session);

        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        WriteCoordinator(body, record.Coordinator);
        body.U8(record.SenderRole);
        WriteSessionActorRouteFence(body, record.Actor);
        WriteSessionOwnerFence(body, record.Session);
        return Finish(ServiceWireConstants.Command.SessionRelocationSeal,
            ServiceWireConstants.Flag.None, body);
    }

    internal static bool TryDecodeSessionRelocationSeal(
        ReadOnlySpan<byte> bytes, out SessionRelocationSealRecord record,
        out DecodeError error)
    {
        record = default;
        if (!Begin(bytes, ServiceWireConstants.Command.SessionRelocationSeal,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var relocation)
            || !TryCoordinator(ref reader, out var coordinator)
            || !reader.TryU8(out var senderRole)
            || senderRole is not (1 or 3)
            || !TrySessionActorRouteFence(ref reader, out var actor)
            || !TrySessionOwnerFence(ref reader, out var session))
            return DecodeFailure(ref reader, out error);
        record = new SessionRelocationSealRecord(relocation, coordinator,
            senderRole, actor, session);
        return End(ref reader, out error);
    }

    internal static byte[] EncodeSessionRelocationSealed(
        SessionRelocationSealedRecord record)
    {
        ValidateSessionRelocationCommon(record.RelocationId,
            record.Coordinator);
        ValidateSessionActorRouteFence(record.Actor);
        ValidateSessionOwnerFence(record.Session);

        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        WriteCoordinator(body, record.Coordinator);
        WriteSessionActorRouteFence(body, record.Actor);
        WriteSessionOwnerFence(body, record.Session);
        return Finish(ServiceWireConstants.Command.SessionRelocationSealed,
            ServiceWireConstants.Flag.None, body);
    }

    internal static bool TryDecodeSessionRelocationSealed(
        ReadOnlySpan<byte> bytes, out SessionRelocationSealedRecord record,
        out DecodeError error)
    {
        record = default;
        if (!Begin(bytes, ServiceWireConstants.Command.SessionRelocationSealed,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var relocation)
            || !TryCoordinator(ref reader, out var coordinator)
            || !TrySessionActorRouteFence(ref reader, out var actor)
            || !TrySessionOwnerFence(ref reader, out var session))
            return DecodeFailure(ref reader, out error);
        record = new SessionRelocationSealedRecord(relocation, coordinator,
            actor, session);
        return End(ref reader, out error);
    }

    internal static byte[] EncodeSessionRelocationRoute(
        SessionRelocationRouteRecord record)
    {
        ValidateSessionRelocationCommon(record.RelocationId,
            record.Coordinator);
        ValidateSessionActor(record.Actor);
        ValidateSessionOwnerFence(record.Session);
        ValidateSessionRelocationRoute(record.SenderRole, record.Route);

        var route = new WireWriter();
        if (record.Route.Action == SessionRelocationRouteAction.Commit)
        {
            route.U64(record.Route.PreviousAuthorityOwnerGeneration);
            route.U64(record.Route.TargetAuthorityOwnerGeneration);
            route.Rid(record.Route.TargetNodeRid);
            route.U64(record.Route.TargetNodeGeneration);
        }
        else
        {
            route.U64(record.Route.CurrentAuthorityOwnerGeneration);
        }

        var body = new WireWriter();
        WriteRelocationId(body, record.RelocationId);
        WriteCoordinator(body, record.Coordinator);
        body.U8(record.SenderRole);
        WriteSessionActor(body, record.Actor);
        WriteSessionOwnerFence(body, record.Session);
        body.U8((byte)record.Route.Action);
        body.U16(checked((ushort)route.Count));
        body.Bytes(route.ToArray());
        return Finish(ServiceWireConstants.Command.SessionRelocationRoute,
            ServiceWireConstants.Flag.None, body);
    }

    internal static bool TryDecodeSessionRelocationRoute(
        ReadOnlySpan<byte> bytes, out SessionRelocationRouteRecord record,
        out DecodeError error)
    {
        record = default;
        if (!Begin(bytes, ServiceWireConstants.Command.SessionRelocationRoute,
                ServiceWireConstants.Flag.None, out var reader, out error))
            return false;
        if (!TryRelocationId(ref reader, out var relocation)
            || !TryCoordinator(ref reader, out var coordinator)
            || !reader.TryU8(out var senderRole)
            || !TrySessionActor(ref reader, out var actor)
            || !TrySessionOwnerFence(ref reader, out var session)
            || !reader.TryU8(out var actionValue)
            || !reader.TryU16(out var routeLength)
            || !reader.TrySlice(routeLength, out var routeBytes))
            return DecodeFailure(ref reader, out error);

        var routeReader = new WireReader(routeBytes);
        SessionRelocationRouteUpdateRecord route;
        if (actionValue == (byte)SessionRelocationRouteAction.Commit
            && senderRole is 2 or 3
            && routeReader.TryU64(out var previousAuthority)
            && previousAuthority != 0
            && routeReader.TryU64(out var targetAuthority)
            && targetAuthority > previousAuthority
            && routeReader.TryRid(out var targetNodeRid)
            && routeReader.TryU64(out var targetNodeGeneration)
            && targetNodeGeneration != 0)
        {
            route = SessionRelocationRouteUpdateRecord.Commit(
                previousAuthority, targetAuthority, targetNodeRid,
                targetNodeGeneration);
        }
        else if (actionValue == (byte)SessionRelocationRouteAction.Abort
                 && senderRole is 1 or 3
                 && routeReader.TryU64(out var currentAuthority)
                 && currentAuthority != 0)
        {
            route = SessionRelocationRouteUpdateRecord.Abort(currentAuthority);
        }
        else
        {
            return DecodeFailure(ref routeReader, out error);
        }
        if (routeReader.Remaining != 0)
        {
            error = DecodeError.InvalidField;
            return false;
        }

        record = new SessionRelocationRouteRecord(relocation, coordinator,
            senderRole, actor, session, route);
        return End(ref reader, out error);
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
            if (senderRole is not (2 or 3)
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
            || senderRole is not (1 or 3)
            || route.CurrentAuthorityOwnerGeneration == 0
            || route.PreviousAuthorityOwnerGeneration != 0
            || route.TargetAuthorityOwnerGeneration != 0
            || !route.TargetNodeRid.IsEmpty
            || route.TargetNodeGeneration != 0)
            throw new ArgumentOutOfRangeException(nameof(route));
    }

    private static void WriteSessionActor(WireWriter writer,
        SessionActorIdentityRecord actor)
    {
        writer.Text8(actor.ActorId);
        writer.U64(actor.ObjectGeneration);
    }

    private static bool TrySessionActor(ref WireReader reader,
        out SessionActorIdentityRecord actor)
    {
        actor = default;
        if (!reader.TryText8(out var actorId)
            || string.IsNullOrEmpty(actorId)
            || !reader.TryU64(out var objectGeneration)
            || objectGeneration == 0)
            return false;
        actor = new SessionActorIdentityRecord(actorId, objectGeneration);
        return true;
    }

    private static void WriteSessionActorRouteFence(WireWriter writer,
        SessionActorRouteFenceRecord actor)
    {
        WriteSessionActor(writer, actor.Actor);
        writer.Rid(actor.TargetNodeRid);
        writer.U64(actor.TargetNodeGeneration);
        writer.U64(actor.AuthorityOwnerGeneration);
        writer.U64(actor.OwnerLeaseGeneration);
    }

    private static bool TrySessionActorRouteFence(ref WireReader reader,
        out SessionActorRouteFenceRecord actor)
    {
        actor = default;
        if (!TrySessionActor(ref reader, out var identity)
            || !reader.TryRid(out var targetNodeRid)
            || !reader.TryU64(out var targetNodeGeneration)
            || targetNodeGeneration == 0
            || !reader.TryU64(out var authorityOwnerGeneration)
            || authorityOwnerGeneration == 0
            || !reader.TryU64(out var ownerLeaseGeneration)
            || ownerLeaseGeneration == 0)
            return false;
        actor = new SessionActorRouteFenceRecord(identity, targetNodeRid,
            targetNodeGeneration, authorityOwnerGeneration,
            ownerLeaseGeneration);
        return true;
    }

    private static void WriteSessionOwnerFence(WireWriter writer,
        SessionOwnerFenceRecord session)
    {
        writer.Rid(session.SessionOwnerNodeRid);
        writer.U64(session.SessionOwnerNodeGeneration);
        writer.Text8(session.SessionOwnerId);
        writer.U64(session.SessionOwnerLeaseGeneration);
        writer.Rid(session.SessionRid);
        writer.U64(session.BindingGeneration);
    }

    private static bool TrySessionOwnerFence(ref WireReader reader,
        out SessionOwnerFenceRecord session)
    {
        session = default;
        if (!reader.TryRid(out var ownerNodeRid)
            || !reader.TryU64(out var ownerNodeGeneration)
            || ownerNodeGeneration == 0
            || !reader.TryText8(out var ownerId)
            || string.IsNullOrEmpty(ownerId)
            || !reader.TryU64(out var ownerLeaseGeneration)
            || ownerLeaseGeneration == 0
            || !reader.TryRid(out var sessionRid)
            || !reader.TryU64(out var bindingGeneration)
            || bindingGeneration == 0)
            return false;
        session = new SessionOwnerFenceRecord(ownerNodeRid,
            ownerNodeGeneration, ownerId, ownerLeaseGeneration, sessionRid,
            bindingGeneration);
        return true;
    }
}
