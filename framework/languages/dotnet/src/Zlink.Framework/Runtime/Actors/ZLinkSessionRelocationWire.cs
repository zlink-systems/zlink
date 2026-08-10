using System.Buffers.Binary;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.Runtime.Actors;

internal readonly record struct ZLinkSessionRelocationContext(
    ZLinkServiceWireCodec.RelocationWireId RelocationId,
    ZLinkServiceWireCodec.RelocationCoordinatorFence Coordinator)
{
    internal bool IsExact =>
        !RelocationId.IsEmpty
        && !string.IsNullOrWhiteSpace(Coordinator.OwnerId)
        && Coordinator.LeaseGeneration != 0
        && !Coordinator.NodeRid.IsEmpty
        && Coordinator.NodeGeneration != 0
        && !string.IsNullOrWhiteSpace(
            Coordinator.ExpectedAuthorityStoreVersion);

    internal static ZLinkSessionRelocationContext Create(
        Guid relocationId,
        string ownerId,
        ulong ownerLeaseGeneration,
        RoutingId sourceNodeRid,
        ulong sourceNodeGeneration,
        string expectedAuthorityStoreVersion)
    {
        var bytes = relocationId.ToByteArray(bigEndian: true);
        var context = new ZLinkSessionRelocationContext(
            new ZLinkServiceWireCodec.RelocationWireId(
                BinaryPrimitives.ReadUInt64BigEndian(bytes.AsSpan(0, 8)),
                BinaryPrimitives.ReadUInt64BigEndian(bytes.AsSpan(8, 8))),
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                ownerId,
                ownerLeaseGeneration,
                sourceNodeRid,
                sourceNodeGeneration,
                expectedAuthorityStoreVersion));
        if (!context.IsExact)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Session relocation requires the exact durable coordinator fence.",
                ZLinkRetryAdvice.DoNotRetry);
        return context;
    }
}

internal static class ZLinkSessionRelocationWire
{
    internal static ZLinkServiceWireCodec.SessionRelocationSealRecord CreateSeal(
        string actorId,
        ZLinkActorBoundSession session,
        ZLinkSessionRelocationContext context)
    {
        var route = new ZLinkRemoteActorBoundSessionRoute(
            session.SessionNodeRid,
            session.SessionRid,
            session.BindingToken,
            session.BindingGeneration,
            session.ObjectGeneration,
            session.AuthorityOwnerGeneration,
            session.MeshName.Value,
            session.TargetNodeGeneration,
            session.OwnerLeaseGeneration,
            session.SessionOwnerNodeGeneration,
            session.AcceptedHighWater,
            session.SessionOwnerId,
            session.SessionOwnerLeaseGeneration);
        return CreateSeal(actorId, route, context);
    }

    internal static ZLinkServiceWireCodec.SessionRelocationSealRecord CreateSeal(
        string actorId,
        ZLinkRemoteActorBoundSessionRoute route,
        ZLinkSessionRelocationContext context)
    {
        RequireExact(route, context);
        return new ZLinkServiceWireCodec.SessionRelocationSealRecord(
            context.RelocationId,
            context.Coordinator,
            1,
            new ZLinkServiceWireCodec.SessionActorRouteFenceRecord(
                new ZLinkServiceWireCodec.SessionActorIdentityRecord(
                    actorId,
                    route.ObjectGeneration),
                route.NodeRid!.Value,
                route.TargetNodeGeneration,
                route.AuthorityOwnerGeneration,
                route.OwnerLeaseGeneration),
            CreateSessionFence(route));
    }

    internal static ZLinkServiceWireCodec.SessionRelocationRouteRecord
        CreateCommit(
            string actorId,
            ZLinkPendingActorSessionRoute pending)
    {
        var route = pending.Route;
        RequireExact(route, pending.WireContext);
        var target = pending.TargetActor
                     ?? throw new InvalidOperationException(
                         "Session relocation commit requires a target Actor.");
        return new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            pending.WireContext.RelocationId,
            pending.WireContext.Coordinator,
            2,
            new ZLinkServiceWireCodec.SessionActorIdentityRecord(
                actorId,
                route.ObjectGeneration),
            CreateSessionFence(route),
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Commit(
                route.AuthorityOwnerGeneration,
                pending.TargetAuthorityOwnerGeneration,
                target.NodeRid,
                pending.TargetNodeGeneration,
                route.AcceptedHighWater));
    }

    internal static ZLinkServiceWireCodec.SessionRelocationRouteRecord
        CreateAbort(
            string actorId,
            ZLinkActorBoundSession session,
            ZLinkSessionRelocationContext context) =>
        CreateAbort(
            actorId,
            new ZLinkRemoteActorBoundSessionRoute(
                session.SessionNodeRid,
                session.SessionRid,
                session.BindingToken,
                session.BindingGeneration,
                session.ObjectGeneration,
                session.AuthorityOwnerGeneration,
                session.MeshName.Value,
                session.TargetNodeGeneration,
                session.OwnerLeaseGeneration,
                session.SessionOwnerNodeGeneration,
                session.AcceptedHighWater,
                session.SessionOwnerId,
                session.SessionOwnerLeaseGeneration),
            context);

    internal static ZLinkServiceWireCodec.SessionRelocationRouteRecord
        CreateAbort(
            string actorId,
            ZLinkRemoteActorBoundSessionRoute route,
            ZLinkSessionRelocationContext context)
    {
        RequireExact(route, context);
        return new ZLinkServiceWireCodec.SessionRelocationRouteRecord(
            context.RelocationId,
            context.Coordinator,
            1,
            new ZLinkServiceWireCodec.SessionActorIdentityRecord(
                actorId,
                route.ObjectGeneration),
            CreateSessionFence(route),
            ZLinkServiceWireCodec.SessionRelocationRouteUpdateRecord.Abort(
                route.AuthorityOwnerGeneration));
    }

    private static ZLinkServiceWireCodec.SessionOwnerFenceRecord
        CreateSessionFence(ZLinkRemoteActorBoundSessionRoute route) =>
        new(
            route.NodeRid!.Value,
            route.SessionOwnerNodeGeneration,
            route.SessionOwnerId!,
            route.SessionOwnerLeaseGeneration,
            route.SessionRid!.Value,
            route.BindingGeneration);

    private static void RequireExact(
        ZLinkRemoteActorBoundSessionRoute route,
        ZLinkSessionRelocationContext context)
    {
        if (!route.IsBound
            || route.NodeRid is null
            || route.SessionRid is null
            || string.IsNullOrWhiteSpace(route.SessionOwnerId)
            || route.SessionOwnerLeaseGeneration == 0
            || !context.IsExact)
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                "Session relocation requires exact route, owner, and coordinator fences.",
                ZLinkRetryAdvice.DoNotRetry);
    }
}
