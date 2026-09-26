using System.Globalization;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkUserSpotOperationTarget(
    IZLinkLocationRepository authorityStore,
    ZLinkSpotNodeCatalog catalog,
    IZLinkBackendSpotNode node,
    ZLinkSpotNodeRegistration registration,
    ZLinkCodecRegistryBuilder codecs
) : IUserSpotOperationTarget
{
    public async ValueTask<UserSpotOperationTerminal> CreateAsync(
        UserSpotCreateOperation operation,
        CancellationToken cancellationToken
    )
    {
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(operation.SpotId);
        var read = await authorityStore
            .ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            throw Stale(operation.SpotId, "The reserved authority is missing.");
        var snapshot = found.Snapshot;
        ValidateCreateFence(operation, snapshot);
        if (
            !ZLinkUserSpotAuthorityPayloadCodec.TryDecode(snapshot.Payload.Span, out var authority)
            || authority.SpotId != operation.SpotId
            || !string.Equals(authority.StableType, operation.StableType, StringComparison.Ordinal)
            || authority.NodeRid != node.RoutingId
            || authority.NodeGeneration != node.MeshStatus().LifecycleGeneration
            || !string.Equals(authority.OwnerId, snapshot.OwnerId, StringComparison.Ordinal)
            || authority.OwnerLeaseGeneration != checked((ulong)snapshot.OwnerLeaseGeneration)
        )
            throw Protocol(operation.SpotId, "The pending authority payload is invalid.");

        if (snapshot.Allocation.State == ZLinkPlacementAllocationState.Active)
        {
            if (authority.State != ZLinkUserSpotAuthorityState.Ready)
                throw Moving(operation.SpotId);
            if (
                await catalog.CloseReadinessAsync(operation.SpotId).ConfigureAwait(false)
                == ReservedSpotCloseReadiness.LocalMissing
            )
                throw Moving(operation.SpotId);
            return SuccessCreate(UserSpotCreateResult.Existing, operation, reply: null);
        }
        if (
            snapshot.Allocation.State != ZLinkPlacementAllocationState.Reserved
            || authority.State != ZLinkUserSpotAuthorityState.Creating
            || snapshot.ReservedCreation is not { } pending
            || !string.Equals(
                pending.ReservationId,
                operation.Reservation.ReservationId,
                StringComparison.Ordinal
            )
        )
            throw Moving(operation.SpotId);
        if (
            !ZLinkInlineCreationIntentCodec.TryDecode(
                pending.RequestContentReference,
                pending.RequestSha256.Span,
                pending.RequestEncodedSize,
                out var content
            )
        )
            throw Protocol(
                operation.SpotId,
                "The immutable creation content failed integrity validation."
            );
        if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecode(content, out var application))
            throw Protocol(operation.SpotId, "The immutable creation content envelope is invalid.");

        if (!registration.SpotRelocations.TryGetValue(operation.StableType, out var factory))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                $"User Spot type '{operation.StableType}' is not registered."
            );

        using var requestPayload = Message.From(application.Payload.Span);
        var request = ZLinkMessage.FromEnvelopePayload(
            application.ContentType,
            requestPayload,
            codecs
        );
        PreparedReservedSpot prepared;
        try
        {
            prepared = await catalog
                .PrepareReservedAsync(
                    factory.InstanceType,
                    operation.SpotId,
                    operation.Reservation.ObjectGeneration,
                    operation.Reservation.AuthorityOwnerGeneration,
                    request,
                    cancellationToken
                )
                .ConfigureAwait(false);
        }
        catch
        {
            await authorityStore
                .AbortAsync(Reservation(operation, key, snapshot), CancellationToken.None)
                .ConfigureAwait(false);
            throw;
        }
        if (prepared.Response is { Accepted: false } rejectedResponse)
        {
            await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
            var aborted = await authorityStore
                .AbortAsync(Reservation(operation, key, snapshot), cancellationToken)
                .ConfigureAwait(false);
            if (
                aborted
                is not (ZLinkObjectAbortResult.Aborted or ZLinkObjectAbortResult.AlreadyAborted)
            )
                throw Moving(operation.SpotId);
            return SuccessCreate(UserSpotCreateResult.Rejected, operation, rejectedResponse.Reply);
        }

        var readyPayload = ZLinkUserSpotAuthorityPayloadCodec.Encode(
            new ZLinkUserSpotAuthorityPayload(
                ZLinkUserSpotAuthorityState.Ready,
                operation.StableType,
                operation.SpotId,
                snapshot.OwnerId,
                checked((ulong)snapshot.OwnerLeaseGeneration),
                registration.SpotNodeName,
                node.RoutingId,
                node.MeshStatus().LifecycleGeneration
            )
        );
        ZLinkObjectCommitResult committed;
        try
        {
            committed = await authorityStore
                .CommitAsync(Reservation(operation, key, snapshot), readyPayload, cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
            await authorityStore
                .AbortAsync(Reservation(operation, key, snapshot), CancellationToken.None)
                .ConfigureAwait(false);
            throw;
        }
        if (
            committed
            is not (ZLinkObjectCommitResult.Committed or ZLinkObjectCommitResult.AlreadyCommitted)
        )
        {
            await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
            await authorityStore
                .AbortAsync(Reservation(operation, key, snapshot), CancellationToken.None)
                .ConfigureAwait(false);
            throw Moving(operation.SpotId);
        }
        await catalog
            .PublishReservedAsync(
                prepared,
                operation.StableType,
                operation.Reservation.ObjectGeneration,
                operation.Reservation.AuthorityOwnerGeneration,
                cancellationToken
            )
            .ConfigureAwait(false);

        return SuccessCreate(
            prepared.Existing ? UserSpotCreateResult.Existing : UserSpotCreateResult.Created,
            operation,
            prepared.Response?.Reply
        );
    }

    public async ValueTask<UserSpotOperationTerminal> CloseAsync(
        UserSpotCloseOperation operation,
        CancellationToken cancellationToken
    )
    {
        var closed = await catalog
            .CloseReservedAsync(
                operation.Target.SpotId,
                DateTimeOffset.FromUnixTimeMilliseconds(checked((long)operation.DeadlineUnixMs)),
                operation.Target,
                cancellationToken
            )
            .ConfigureAwait(false);
        return new UserSpotOperationTerminal(
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            new UserSpotCloseCompletion(closed)
        );
    }

    private UserSpotOperationTerminal SuccessCreate(
        UserSpotCreateResult result,
        UserSpotCreateOperation operation,
        ZLinkMessage? reply
    )
    {
        return new UserSpotOperationTerminal(
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            new UserSpotCreateCompletion(
                result,
                operation.SpotId,
                operation.Reservation.ObjectGeneration
            ),
            EncodeReply(operation.Correlation, reply)
        );
    }

    private IReadOnlyList<ReadOnlyMemory<byte>>? EncodeReply(ulong correlation, ZLinkMessage? reply)
    {
        if (reply is null)
            return null;
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Response,
                string.Empty,
                string.Empty,
                ZLinkEnvelopeCodec.DefaultContentType,
                correlation.ToString(CultureInfo.InvariantCulture),
                null,
                null,
                null,
                null
            ),
            reply,
            typeof(ZLinkMessage),
            codecs
        );
        try
        {
            return parts
                .Select(static part => (ReadOnlyMemory<byte>)part.AsReadOnlyMemory().ToArray())
                .ToArray();
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static ZLinkObjectReservation Reservation(
        UserSpotCreateOperation operation,
        ZLinkAuthorityKey key,
        ZLinkAuthoritySnapshot snapshot
    ) =>
        new(
            key,
            operation.Reservation.ExpectedStoreVersion,
            operation.Reservation.ObjectGeneration,
            operation.Reservation.AuthorityOwnerGeneration,
            operation.Reservation.ReservationId,
            snapshot.Allocation.Descriptor,
            operation.Reservation.TargetNodeGeneration,
            new ZLinkLocationOwnerToken(
                operation.Reservation.TargetOwnerId,
                operation.Reservation.TargetOwnerLeaseGeneration
            )
        );

    private static void ValidateCreateFence(
        UserSpotCreateOperation operation,
        ZLinkAuthoritySnapshot snapshot
    )
    {
        var fence = operation.Reservation;
        if (
            snapshot.ObjectGeneration != fence.ObjectGeneration
            || snapshot.AuthorityOwnerGeneration != fence.AuthorityOwnerGeneration
            || !string.Equals(
                snapshot.StoreVersion,
                fence.ExpectedStoreVersion,
                StringComparison.Ordinal
            )
            || snapshot.Allocation.ObjectKind != ZLinkPlacementObjectKind.UserSpot
            || !string.Equals(
                snapshot.Allocation.StableType,
                operation.StableType,
                StringComparison.Ordinal
            )
            || snapshot.Allocation.Descriptor.Rid != fence.TargetNodeRid
            || snapshot.Allocation.DescriptorLifecycleGeneration != fence.TargetNodeGeneration
            || !string.Equals(snapshot.OwnerId, fence.TargetOwnerId, StringComparison.Ordinal)
            || snapshot.OwnerLeaseGeneration != checked((long)fence.TargetOwnerLeaseGeneration)
            || snapshot.Allocation.Capacity
                != new ZLinkCapacityVector(
                    0,
                    checked((int)fence.PendingCapacityDelta),
                    new ZLinkSpotTypeCapacityDelta(
                        ZLinkPlacementObjectKind.UserSpot,
                        operation.StableType,
                        checked((int)fence.PendingCapacityDelta)
                    )
                )
        )
            throw Moving(operation.SpotId);
    }

    private static ZLinkFrameworkException Stale(string spotId, string message) =>
        new(ZLinkFrameworkErrorKind.InvalidOperation, $"User Spot '{spotId}': {message}");

    private static ZLinkFrameworkException Moving(string spotId) =>
        Moving(spotId, "the authority changed during the operation");

    private static ZLinkFrameworkException Moving(string spotId, string reason) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"User Spot '{spotId}': {reason}.",
            ZLinkRetryAdvice.RetryAfterBackoff
        );

    private static ZLinkFrameworkException Protocol(string spotId, string message) =>
        new(ZLinkFrameworkErrorKind.ProtocolError, $"User Spot '{spotId}': {message}");
}
