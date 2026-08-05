using System.Globalization;
using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkUserSpotOperationTarget(
    IZLinkLocationRepository authorityStore,
    ZLinkSpotNodeCatalog catalog,
    IZLinkBackendSpotNode node,
    ZLinkSpotNodeRegistration registration,
    ZLinkCodecRegistryBuilder codecs) : IUserSpotOperationTarget
{
    private const int ExactAuthorityCasRetryLimit = 64;

    public async ValueTask<UserSpotOperationTerminal> CreateAsync(
        UserSpotCreateOperation operation,
        CancellationToken cancellationToken)
    {
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(operation.SpotId);
        var read = await authorityStore.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is not ZLinkAuthorityReadResult.Found found)
            throw Stale(operation.SpotId, "The reserved authority is missing.");
        var snapshot = found.Snapshot;
        ValidateCreateFence(operation, snapshot);
        if (!ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var authority)
            || authority.SpotId != operation.SpotId
            || !string.Equals(
                authority.StableType,
                operation.StableType,
                StringComparison.Ordinal)
            || authority.NodeRid != node.RoutingId
            || authority.NodeGeneration != node.MeshStatus().LifecycleGeneration
            || !string.Equals(
                authority.OwnerId,
                snapshot.OwnerId,
                StringComparison.Ordinal)
            || authority.OwnerLeaseGeneration
            != checked((ulong)snapshot.OwnerLeaseGeneration))
            throw Protocol(operation.SpotId, "The pending authority payload is invalid.");

        if (snapshot.Allocation.State == ZLinkPlacementAllocationState.Active)
        {
            if (authority.State != ZLinkUserSpotAuthorityState.Ready)
                throw Moving(operation.SpotId);
            if (catalog.CloseReadiness(operation.SpotId)
                == ReservedSpotCloseReadiness.LocalMissing)
                throw Moving(operation.SpotId);
            return SuccessCreate(
                UserSpotCreateResult.Existing,
                operation,
                reply: null);
        }
        if (snapshot.Allocation.State != ZLinkPlacementAllocationState.Reserved
            || authority.State != ZLinkUserSpotAuthorityState.Creating
            || snapshot.ReservedCreation is not { } pending
            || !string.Equals(
                pending.ReservationId,
                operation.Reservation.ReservationId,
                StringComparison.Ordinal))
            throw Moving(operation.SpotId);
        if (!ZLinkInlineCreationIntentCodec.TryDecode(
                pending.RequestContentReference,
                out var content)
            || content.Length != pending.RequestEncodedSize
            || !System.Security.Cryptography.CryptographicOperations.FixedTimeEquals(
                System.Security.Cryptography.SHA256.HashData(content),
                pending.RequestSha256.Span))
            throw Protocol(
                operation.SpotId,
                "The immutable creation content failed integrity validation.");
        if (!ZLinkApplicationPayloadEnvelopeCodec.TryDecode(
                content,
                out var application))
            throw Protocol(
                operation.SpotId,
                "The immutable creation content envelope is invalid.");

        if (!registration.SpotRelocations.TryGetValue(
                operation.StableType,
                out var factory))
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.TypeMismatch,
                $"User Spot type '{operation.StableType}' is not registered.");

        using var requestPayload = Message.From(application.Payload.Span);
        var request = ZLinkMessage.FromEnvelopePayload(
            application.ContentType,
            requestPayload,
            codecs);
        PreparedReservedSpot prepared;
        try
        {
            prepared = await catalog.PrepareReservedAsync(
                    factory.InstanceType,
                    operation.SpotId,
                    operation.Reservation.ObjectGeneration,
                    operation.Reservation.AuthorityOwnerGeneration,
                    request,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            await authorityStore.AbortAsync(
                    Reservation(operation, key, snapshot),
                    CancellationToken.None)
                .ConfigureAwait(false);
            throw;
        }
        if (prepared.Response is { Accepted: false } rejectedResponse)
        {
            await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
            var aborted = await authorityStore.AbortAsync(
                    Reservation(operation, key, snapshot),
                    cancellationToken)
                .ConfigureAwait(false);
            if (aborted is not (ZLinkObjectAbortResult.Aborted
                or ZLinkObjectAbortResult.AlreadyAborted))
                throw Moving(operation.SpotId);
            return SuccessCreate(
                UserSpotCreateResult.Rejected,
                operation,
                rejectedResponse.Reply);
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
                node.MeshStatus().LifecycleGeneration));
        ZLinkObjectCommitResult committed;
        try
        {
            committed = await authorityStore.CommitAsync(
                    Reservation(operation, key, snapshot),
                    readyPayload,
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
            await authorityStore.AbortAsync(
                    Reservation(operation, key, snapshot),
                    CancellationToken.None)
                .ConfigureAwait(false);
            throw;
        }
        if (committed is not (ZLinkObjectCommitResult.Committed
            or ZLinkObjectCommitResult.AlreadyCommitted))
        {
            await catalog.DiscardReservedAsync(prepared).ConfigureAwait(false);
            await authorityStore.AbortAsync(
                    Reservation(operation, key, snapshot),
                    CancellationToken.None)
                .ConfigureAwait(false);
            throw Moving(operation.SpotId);
        }
        await catalog.PublishReservedAsync(
                prepared,
                operation.StableType,
                operation.Reservation.ObjectGeneration,
                operation.Reservation.AuthorityOwnerGeneration,
                cancellationToken)
            .ConfigureAwait(false);

        return SuccessCreate(
            prepared.Existing
                ? UserSpotCreateResult.Existing
                : UserSpotCreateResult.Created,
            operation,
            prepared.Response?.Reply);
    }

    public async ValueTask<UserSpotOperationTerminal> CloseAsync(
        UserSpotCloseOperation operation,
        CancellationToken cancellationToken)
    {
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey(
            operation.Target.SpotId);
        var read = await authorityStore.ReadAuthorityAsync(key, cancellationToken)
            .ConfigureAwait(false);
        if (read is ZLinkAuthorityReadResult.Missing)
            return new UserSpotOperationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new UserSpotCloseCompletion(false));

        var snapshot = ((ZLinkAuthorityReadResult.Found)read).Snapshot;
        ValidateCloseFence(operation.Target, snapshot);
        if (!ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                snapshot.Payload.Span,
                out var authority)
            || authority.SpotId != operation.Target.SpotId)
            throw Protocol(operation.Target.SpotId, "The current authority payload is invalid.");
        if (authority.NodeRid != node.RoutingId
            || authority.NodeGeneration != node.MeshStatus().LifecycleGeneration
            || !string.Equals(authority.OwnerId, snapshot.OwnerId, StringComparison.Ordinal)
            || authority.OwnerLeaseGeneration
            != checked((ulong)snapshot.OwnerLeaseGeneration))
            throw Moving(
                operation.Target.SpotId,
                "the Ready authority identity does not match the local owner");
        if (snapshot.Allocation.State != ZLinkPlacementAllocationState.Active
            || snapshot.Allocation.ObjectKind
            != ZLinkPlacementObjectKind.UserSpot
            || authority.State != ZLinkUserSpotAuthorityState.Ready)
            throw Moving(
                operation.Target.SpotId,
                "the authority is not an active Ready User Spot");
        var readiness = catalog.CloseReadiness(operation.Target.SpotId);
        if (readiness == ReservedSpotCloseReadiness.HasActors)
            return new UserSpotOperationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new UserSpotCloseCompletion(false));
        if (readiness is ReservedSpotCloseReadiness.LocalMissing
            or ReservedSpotCloseReadiness.Closing)
            throw Moving(
                operation.Target.SpotId,
                $"the local catalog close readiness is {readiness}");

        var closingPayload = ZLinkUserSpotAuthorityPayloadCodec.Encode(
            authority with
            {
                State = ZLinkUserSpotAuthorityState.Closing
            });
        var sealedResult = await CompareExchangeExactAuthorityAsync(
                key,
                snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    closingPayload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null),
                cancellationToken)
            .ConfigureAwait(false);
        if (sealedResult is not ZLinkAuthorityCompareExchangeResult.Stored sealedSnapshot)
            throw Moving(
                operation.Target.SpotId,
                "the Ready-to-Closing authority exchange lost its exact fence");

        bool closed;
        try
        {
            closed = await catalog.CloseReservedAsync(
                    operation.Target.SpotId,
                    DateTimeOffset.FromUnixTimeMilliseconds(
                        checked((long)operation.DeadlineUnixMs)),
                    cancellationToken)
                .ConfigureAwait(false);
        }
        catch
        {
            await RestoreReadyAsync(
                    key,
                    sealedSnapshot.Snapshot.StoreVersion,
                    snapshot)
                .ConfigureAwait(false);
            throw;
        }
        if (!closed)
        {
            await RestoreReadyAsync(
                    key,
                    sealedSnapshot.Snapshot.StoreVersion,
                    snapshot)
                .ConfigureAwait(false);
            return new UserSpotOperationTerminal(
                RequestResult.Ok,
                ServiceWireConstants.FrameworkErrorCode.None,
                new UserSpotCloseCompletion(false));
        }
        var deleted = await CompareExchangeExactAuthorityAsync(
                key,
                sealedSnapshot.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Delete(),
                cancellationToken)
            .ConfigureAwait(false);
        if (deleted is not ZLinkAuthorityCompareExchangeResult.Deleted)
            throw Moving(
                operation.Target.SpotId,
                "the Closing-to-deleted authority exchange lost its exact fence");
        return new UserSpotOperationTerminal(
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            new UserSpotCloseCompletion(closed));
    }

    private async ValueTask RestoreReadyAsync(
        ZLinkAuthorityKey key,
        string closingStoreVersion,
        ZLinkAuthoritySnapshot ready)
    {
        var restored = await CompareExchangeExactAuthorityAsync(
                key,
                closingStoreVersion,
                new ZLinkAuthorityMutation.Put(
                    ready.Payload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null),
                CancellationToken.None)
            .ConfigureAwait(false);
        if (restored is not ZLinkAuthorityCompareExchangeResult.Stored)
            throw Moving(
                ZLinkUserSpotAuthorityPayloadCodec.TryDecode(
                    ready.Payload.Span, out var authority)
                    ? authority.SpotId
                    : key.Value);
    }

    private async ValueTask<ZLinkAuthorityCompareExchangeResult>
        CompareExchangeExactAuthorityAsync(
        ZLinkAuthorityKey key,
        string expectedStoreVersion,
        ZLinkAuthorityMutation mutation,
        CancellationToken cancellationToken)
    {
        for (var attempt = 0;
             attempt < ExactAuthorityCasRetryLimit;
             attempt++)
        {
            var result = await authorityStore.CompareExchangeAuthorityAsync(
                    key,
                    expectedStoreVersion,
                    mutation,
                    cancellationToken)
                .ConfigureAwait(false);
            if (result is not ZLinkAuthorityCompareExchangeResult.Conflict(
                    ZLinkAuthorityReadResult.Found current)
                || !string.Equals(
                    current.Snapshot.StoreVersion,
                    expectedStoreVersion,
                    StringComparison.Ordinal))
                return result;

            // The authority row itself is unchanged. Retry a transient loss
            // against owner/capacity heartbeat conditions without relaxing
            // the caller's exact row fence.
            cancellationToken.ThrowIfCancellationRequested();
            await Task.Yield();
        }

        return await authorityStore.CompareExchangeAuthorityAsync(
                key,
                expectedStoreVersion,
                mutation,
                cancellationToken)
            .ConfigureAwait(false);
    }

    private UserSpotOperationTerminal SuccessCreate(
        UserSpotCreateResult result,
        UserSpotCreateOperation operation,
        ZLinkMessage? reply)
    {
        return new UserSpotOperationTerminal(
            RequestResult.Ok,
            ServiceWireConstants.FrameworkErrorCode.None,
            new UserSpotCreateCompletion(
                result,
                operation.SpotId,
                operation.Reservation.ObjectGeneration),
            EncodeReply(operation.Correlation, reply));
    }

    private IReadOnlyList<ReadOnlyMemory<byte>>? EncodeReply(
        ulong correlation,
        ZLinkMessage? reply)
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
                null),
            reply,
            typeof(ZLinkMessage),
            codecs);
        try
        {
            return parts
                .Select(static part =>
                    (ReadOnlyMemory<byte>)part.AsReadOnlyMemory().ToArray())
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
        ZLinkAuthoritySnapshot snapshot) =>
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
                operation.Reservation.TargetOwnerLeaseGeneration));

    private static void ValidateCreateFence(
        UserSpotCreateOperation operation,
        ZLinkAuthoritySnapshot snapshot)
    {
        var fence = operation.Reservation;
        if (snapshot.ObjectGeneration != fence.ObjectGeneration
            || snapshot.AuthorityOwnerGeneration != fence.AuthorityOwnerGeneration
            || !string.Equals(
                snapshot.StoreVersion,
                fence.ExpectedStoreVersion,
                StringComparison.Ordinal)
            || snapshot.Allocation.ObjectKind != ZLinkPlacementObjectKind.UserSpot
            || !string.Equals(
                snapshot.Allocation.StableType,
                operation.StableType,
                StringComparison.Ordinal)
            || snapshot.Allocation.Descriptor.Rid != fence.TargetNodeRid
            || snapshot.Allocation.DescriptorLifecycleGeneration
            != fence.TargetNodeGeneration
            || !string.Equals(snapshot.OwnerId, fence.TargetOwnerId, StringComparison.Ordinal)
            || snapshot.OwnerLeaseGeneration
            != checked((long)fence.TargetOwnerLeaseGeneration)
            || snapshot.Allocation.Capacity
            != new ZLinkCapacityVector(
                0,
                checked((int)fence.PendingCapacityDelta),
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    operation.StableType,
                    checked((int)fence.PendingCapacityDelta))))
            throw Moving(operation.SpotId);
    }

    private static void ValidateCloseFence(
        UserSpotCloseFence fence,
        ZLinkAuthoritySnapshot snapshot)
    {
        if (snapshot.ObjectGeneration != fence.ObjectGeneration)
            throw Stale(fence.SpotId, "The exact User Spot generation is stale.");
        if (snapshot.AuthorityOwnerGeneration != fence.AuthorityOwnerGeneration
            || !string.Equals(
                snapshot.StoreVersion,
                fence.ExpectedStoreVersion,
                StringComparison.Ordinal)
            || snapshot.Allocation.Descriptor.Rid != fence.TargetNodeRid
            || snapshot.Allocation.DescriptorLifecycleGeneration
            != fence.TargetNodeGeneration)
            throw Moving(
                fence.SpotId,
                "the command 48 close fence does not match the current authority");
    }

    private static ZLinkFrameworkException Stale(string spotId, string message) =>
        new(
            ZLinkFrameworkErrorKind.InvalidOperation,
            $"User Spot '{spotId}': {message}");

    private static ZLinkFrameworkException Moving(string spotId) =>
        Moving(spotId, "the authority changed during the operation");

    private static ZLinkFrameworkException Moving(
        string spotId,
        string reason) =>
        new(
            ZLinkFrameworkErrorKind.Unavailable,
            $"User Spot '{spotId}': {reason}.",
            ZLinkRetryAdvice.RetryAfterBackoff);

    private static ZLinkFrameworkException Protocol(string spotId, string message) =>
        new(
            ZLinkFrameworkErrorKind.ProtocolError,
            $"User Spot '{spotId}': {message}");
}
