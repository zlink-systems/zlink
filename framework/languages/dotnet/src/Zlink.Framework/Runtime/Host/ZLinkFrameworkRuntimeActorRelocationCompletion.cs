using Systems.Zlink.Framework.Runtime.Protocol;

namespace Zlink.Framework.Runtime.Host;

internal sealed partial class ZLinkFrameworkRuntime
{
    internal Task QueueCanonicalStandaloneActorFrames(
        ZLinkActorRuntimeState actorState,
        ZLinkActorAuthorityPayload targetAuthority,
        ZLinkRelocationEnvelope identity,
        string handoffId,
        ZLinkAuthoritySnapshot authority,
        IReadOnlyList<ZLinkActorHandoffFrame> acceptedFrames
    )
    {
        var actorRef =
            actorState.NativeActorRef
            ?? throw new ZLinkRelocationDataLostException(
                "Standalone Actor target reference is unavailable during replay."
            );
        var relocation = authority.Relocation;
        if (relocation?.Target is not { } target || relocation.RelocationId != identity.AggregateId)
            throw new ZLinkRelocationDataLostException(
                "Standalone Actor completion authority has no matching target fence."
            );
        var requests = acceptedFrames
            .Where(frame => frame.RelocationReplyRouteId != 0)
            .ToDictionary(frame => (frame.RouteContext.OperationId, frame.RequestSource));
        var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
            authority.OwnerId,
            checked((ulong)authority.OwnerLeaseGeneration),
            targetAuthority.NodeRid,
            targetAuthority.NodeGeneration,
            authority.StoreVersion
        );
        var writer = new ZLinkLocationOwnerToken(authority.OwnerId, authority.OwnerLeaseGeneration);
        var key = identity.Participants.Single().AuthorityKey;
        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this),
            targetAuthority.MeshName
        );
        var cancellationToken = ShutdownToken;
        var prefix = AdmitTransferredActorPrefix(
            actorState,
            handoffId,
            targetAuthority.MeshName,
            frame =>
            {
                Func<
                    ZLinkSpotActorFrame,
                    ZLinkActorReply?,
                    CancellationToken,
                    ValueTask
                >? complete = requests.TryGetValue(
                    (frame.RouteContext.OperationId, frame.RequestSource),
                    out var accepted
                )
                    ? (replay, reply, _) => Complete(accepted, replay, reply)
                    : null;
                return actorState.LiveActivation is { } activation
                    ? activation.DispatchOwnedActorFrameAsync(
                        actorRef,
                        frame,
                        complete,
                        cancellationToken
                    )
                    : pipeline.DispatchOwnedFrameAsync(
                        ZLinkActorHandoffFrames.Restore(actorRef, [frame]),
                        complete,
                        cancellationToken
                    );
            }
        );
        if (requests.Count != 0)
            RetainAcceptedRepliesUntilReplayEnds(identity.AggregateId, writer, prefix);
        return prefix;

        ValueTask Complete(
            ZLinkActorHandoffFrame accepted,
            ZLinkSpotActorFrame replay,
            ZLinkActorReply? reply
        )
        {
            if (reply is null || accepted.RequestSource is not { } source)
                throw new ZLinkRelocationDataLostException(
                    "Standalone Actor accepted request has no reply or source fence."
                );
            var relay = CreateStandaloneActorReplyRelay(
                identity,
                coordinator,
                target.AttemptGeneration,
                accepted
            );
            var entry = CreateRelocationCompletionEntry(
                key,
                relay,
                source,
                [reply.ToFrame(replay.Header)]
            );
            ScheduleRelocationReply(new(identity.AggregateId, writer, entry), relay);
            return ValueTask.CompletedTask;
        }
    }

    internal static ZLinkRelocationCompletionEntry CreateAcceptedRelocationReply(
        ZLinkAuthorityKey key,
        MeshOperationId operation,
        ulong replyRouteId,
        ZLinkServiceWireCodec.RequestSourceFence source
    ) =>
        new(
            key.Value,
            $"{operation.High:x16}{operation.Low:x16}",
            replyRouteId,
            "",
            new(
                source.OwnerId,
                source.LeaseGeneration,
                source.NodeRid.ToHex(),
                source.NodeGeneration
            ),
            ZLinkRelocationCompletionDelivery.Accepted
        );

    private static ZLinkServiceWireCodec.ReplyRelayRecord CreateStandaloneActorReplyRelay(
        ZLinkRelocationEnvelope identity,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator,
        ulong targetAttemptGeneration,
        ZLinkActorHandoffFrame frame
    ) =>
        new(
            frame.RouteContext.OperationId,
            frame.RelocationReplyRouteId,
            new(identity.CanonicalRelocationHigh, identity.CanonicalRelocationLow),
            targetAttemptGeneration,
            coordinator,
            identity.Participants.Single().CanonicalParticipantId,
            checked((ulong)frame.ArrivalIndex),
            0,
            0
        );

    private static ZLinkRelocationCompletionEntry CreateRelocationCompletionEntry(
        ZLinkAuthorityKey key,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        ZLinkServiceWireCodec.RequestSourceFence source,
        IReadOnlyList<byte[]>? payload
    )
    {
        var header = ZLinkServiceWireCodec.EncodeReplyRelay(relay);
        var reply = ZLinkApplicationPayloadEnvelopeCodec.EncodeMultipartPayload(
            payload is null
                ? [header]
                :
                [
                    header,
                    ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                        payload.Select(static part => (ReadOnlyMemory<byte>)part).ToArray()
                    ),
                ]
        );
        return new(
            key.Value,
            $"{relay.OperationId.High:x16}{relay.OperationId.Low:x16}",
            relay.ReplyRouteId,
            Convert.ToBase64String(reply),
            new(
                source.OwnerId,
                source.LeaseGeneration,
                source.NodeRid.ToHex(),
                source.NodeGeneration
            )
        );
    }

    private void RetainAcceptedRepliesUntilReplayEnds(
        Guid relocationId,
        ZLinkLocationOwnerToken writer,
        Task replay
    )
    {
        if (
            replay.IsCompleted
            || _locationRuntime is not { } locations
            || Registration.Locations.ResolveStore() is not { } repository
        )
            return;

        _ = TryRunDetached(
            "retain accepted relocation replies",
            async token =>
            {
                locations.OwnerLeaseRenewed += OnOwnerLeaseRenewed;
                try
                {
                    await replay.WaitAsync(token).ConfigureAwait(false);
                }
                finally
                {
                    locations.OwnerLeaseRenewed -= OnOwnerLeaseRenewed;
                }
            }
        );

        void OnOwnerLeaseRenewed(ZLinkOwnerLeaseRenewal _) =>
            RunDetached(
                "renew accepted relocation reply rows",
                token => repository.RenewAcceptedRelocationRepliesAsync(relocationId, writer, token)
            );
    }

    private void ScheduleRelocationReply(
        ZLinkRelocationReplyRow result,
        ZLinkServiceWireCodec.ReplyRelayRecord relay
    )
    {
        _ = TryRunDetached(
            "relocation reply delivery",
            token => PublishAndDeliverRelocationReplyAsync(result, relay, token)
        );
    }

    private async ValueTask PublishAndDeliverRelocationReplyAsync(
        ZLinkRelocationReplyRow result,
        ZLinkServiceWireCodec.ReplyRelayRecord relay,
        CancellationToken cancellationToken
    )
    {
        var authorities = Registration.Locations.ResolveStore()!;
        var store = new ZLinkRelocationCompletionStore(authorities);
        var source = result.Entry.Source;
        var sourceFence = new ZLinkServiceWireCodec.RequestSourceFence(
            source.OwnerId,
            source.OwnerLeaseGeneration,
            RoutingId.FromHex(source.NodeRid),
            source.NodeGeneration
        );
        while (true)
        {
            try
            {
                var pending = await store
                    .PublishAsync(
                        result,
                        () =>
                            CreateRelocationCompletionEntry(
                                new(result.Entry.ObjectKey),
                                relay with
                                {
                                    TerminalResult = (uint)RequestResult.InternalError,
                                    FailureCode = ServiceWireConstants
                                        .FrameworkErrorCode
                                        .RequestFailed,
                                },
                                sourceFence,
                                null
                            ).Reply,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (pending is not { Entry.Delivery: ZLinkRelocationCompletionDelivery.Pending })
                    return;
                var evidence = await TryDeliverStoredRelocationReplyAsync(
                        pending,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (evidence != ZLinkRelocationCompletionDelivery.Pending)
                {
                    await store
                        .AcknowledgeAsync(pending, evidence, cancellationToken)
                        .ConfigureAwait(false);
                    return;
                }
            }
            catch (Exception error)
                when (ZLinkLocationStoreFailure.IsUnavailable(error, cancellationToken))
            {
                ZLinkFrameworkDebugLog.SpotDiscovery(
                    $"relocation_reply_pending relocation={result.RelocationId:N} error={error.Message}"
                );
            }
            await Task.Delay(Registration.Locations.Options.PollingInterval, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask<ZLinkRelocationCompletionDelivery> TryDeliverStoredRelocationReplyAsync(
        ZLinkRelocationReplyRow row,
        CancellationToken cancellationToken
    )
    {
        var source = row.Entry.Source;
        var sourceRid = RoutingId.FromHex(source.NodeRid);
        var sourceFence = new ZLinkServiceWireCodec.RequestSourceFence(
            source.OwnerId,
            source.OwnerLeaseGeneration,
            sourceRid,
            source.NodeGeneration
        );
        var (relay, payload) = DecodeStoredRelocationReply(row);
        if (
            relay.Coordinator.OwnerId != row.Writer.OwnerId
            || relay.Coordinator.LeaseGeneration != checked((ulong)row.Writer.LeaseGeneration)
        )
            throw new ZLinkRelocationDataLostException(
                "Stored relocation reply has a different writer."
            );
        try
        {
            var lease = await Registration
                .Locations.ResolveStore()!
                .ReadOwnerLeaseAsync(source.OwnerId, cancellationToken)
                .ConfigureAwait(false);
            if (IsExactSourceLeaseExpired(lease, source.OwnerId, source.OwnerLeaseGeneration))
                return ZLinkRelocationCompletionDelivery.SourceLeaseExpired;

            var acknowledgement = await RelayRelocationReplyAsync(
                    sourceRid,
                    relay,
                    sourceFence,
                    payload,
                    cancellationToken
                )
                .ConfigureAwait(false);
            return acknowledgement switch
            {
                ZLinkRelocationReplyAckState.TerminalReceived =>
                    ZLinkRelocationCompletionDelivery.TerminalReceived,
                ZLinkRelocationReplyAckState.AlreadyTerminal =>
                    ZLinkRelocationCompletionDelivery.AlreadyTerminal,
                _ => ZLinkRelocationCompletionDelivery.Pending,
            };
        }
        catch (Exception error)
            when (ZLinkLocationStoreFailure.IsUnavailable(error, cancellationToken))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"relocation_reply_pending relocation={row.RelocationId:N} error={error.Message}"
            );
            return ZLinkRelocationCompletionDelivery.Pending;
        }
    }

    private static (
        ZLinkServiceWireCodec.ReplyRelayRecord Relay,
        byte[][] Payload
    ) DecodeStoredRelocationReply(ZLinkRelocationReplyRow row)
    {
        var entry = row.Entry;
        if (
            !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeMultipart(
                Convert.FromBase64String(entry.Reply),
                out var wire
            )
        )
            throw new ZLinkRelocationDataLostException(
                "Stored relocation reply multipart is invalid."
            );
        try
        {
            if (
                wire.Length is < 1 or > 2
                || !ZLinkServiceWireCodec.TryDecodeReplyRelay(
                    wire[0].AsReadOnlySpan(),
                    out var relay,
                    out _
                )
                || entry.OperationId != $"{relay.OperationId.High:x16}{relay.OperationId.Low:x16}"
                || entry.ReplyRouteId != relay.ReplyRouteId
                || row.RelocationId.ToString("N")
                    != $"{relay.RelocationId.High:x16}{relay.RelocationId.Low:x16}"
                || wire.Length == 2 && relay.TerminalResult != 0
            )
                throw new ZLinkRelocationDataLostException(
                    "Stored relocation reply does not match its request fence."
                );
            if (wire.Length == 1)
                return (relay, []);
            if (
                !ZLinkApplicationPayloadEnvelopeCodec.TryDecodeFrameworkMultipart(
                    wire[1],
                    out var payload
                )
            )
                throw new ZLinkRelocationDataLostException(
                    "Stored relocation reply payload is invalid."
                );
            try
            {
                return (relay, payload.Select(part => part.AsReadOnlySpan().ToArray()).ToArray());
            }
            finally
            {
                ZLinkMessageParts.DisposeAll(payload);
            }
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(wire);
        }
    }

    internal static bool IsExactSourceLeaseExpired(
        ZLinkOwnerLeaseReadResult lease,
        string ownerId,
        ulong leaseGeneration
    ) =>
        lease switch
        {
            ZLinkOwnerLeaseReadResult.Missing => true,
            ZLinkOwnerLeaseReadResult.Found found => leaseGeneration > long.MaxValue
                || !StringComparer.Ordinal.Equals(found.Token.OwnerId, ownerId)
                || found.Token.LeaseGeneration != checked((long)leaseGeneration)
                || found.LeaseExpiresAt <= found.StoreNow,
            _ => false,
        };
}
