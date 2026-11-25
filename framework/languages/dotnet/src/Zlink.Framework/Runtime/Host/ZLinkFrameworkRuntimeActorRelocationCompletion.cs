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
        IReadOnlyList<ZLinkActorHandoffFrame> acceptedFrames,
        ulong? legacyTargetAttempt = null
    )
    {
        var actorRef =
            actorState.NativeActorRef
            ?? throw new ZLinkRelocationDataLostException(
                "Standalone Actor target reference is unavailable during replay."
            );
        var participant = identity.Participants.Single();
        var requests = acceptedFrames
            .Where(frame => frame.RelocationReplyRouteId != 0)
            .ToDictionary(frame => (frame.RouteContext.OperationId, frame.RequestSource));
        if (requests.Count == 0)
            return AdmitTransferredActorPrefix(actorState, handoffId, targetAuthority.MeshName);
        var pipeline = new ZLinkActorInboundPipeline(
            this,
            new ZLinkEntrySpotActorInboundEndpoint(this),
            targetAuthority.MeshName
        );
        var relocation = authority.Relocation;
        var coordinator = new ZLinkServiceWireCodec.RelocationCoordinatorFence(
            authority.OwnerId,
            checked((ulong)authority.OwnerLeaseGeneration),
            targetAuthority.NodeRid,
            targetAuthority.NodeGeneration,
            relocation?.Coordinator.ExpectedStoreVersion ?? authority.StoreVersion
        );
        var budget = relocation?.Completion is { AcceptedRequests: > 0 }
            ? CreateStandaloneActorCompletionBudget(
                identity,
                acceptedFrames,
                coordinator,
                relocation.Target!.AttemptGeneration
            )
            : null;
        var completionStore = budget is null ? null : CreateRelocationCompletionStore(budget);
        Task<(
            ZLinkAuthoritySnapshot Authority,
            ZLinkRelocationCompletionEntry Entry
        )>? persistence = null;
        var delivery = Task.CompletedTask;
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
                    ? (replay, reply, token) => Complete(accepted, replay, reply, token)
                    : null;
                return actorState.LiveActivation is { } activation
                    ? activation.DispatchOwnedActorFrameAsync(
                        actorRef,
                        frame,
                        complete,
                        ShutdownToken
                    )
                    : pipeline.DispatchOwnedFrameAsync(
                        ZLinkActorHandoffFrames.Restore(actorRef, [frame]),
                        complete,
                        ShutdownToken
                    );

                async ValueTask Complete(
                    ZLinkActorHandoffFrame accepted,
                    ZLinkSpotActorFrame replay,
                    ZLinkActorReply? reply,
                    CancellationToken token
                )
                {
                    var attempt =
                        authority.Relocation?.Target?.AttemptGeneration ?? legacyTargetAttempt;
                    if (
                        reply is null
                        || accepted.RequestSource is not { } source
                        || attempt is null
                    )
                        throw new ZLinkRelocationDataLostException(
                            "Standalone Actor accepted request has no reply or relocation fence."
                        );
                    var relay = CreateStandaloneActorReplyRelay(
                        identity,
                        coordinator,
                        attempt.Value,
                        accepted
                    );
                    var payload = reply.ToFrame(replay.Header);
                    if (completionStore is null)
                    {
                        var ack = await RelayRelocationReplyAsync(
                                source.NodeRid,
                                relay with
                                {
                                    Coordinator = coordinator with
                                    {
                                        ExpectedAuthorityStoreVersion = authority.StoreVersion,
                                    },
                                },
                                source,
                                [payload],
                                token
                            )
                            .ConfigureAwait(false);
                        if (ack != ZLinkRelocationReplyAckState.NotAcknowledged)
                            return;
                        var lease = await Registration
                            .Locations.ResolveStore()!
                            .ReadOwnerLeaseAsync(source.OwnerId, token)
                            .ConfigureAwait(false);
                        if (
                            !IsExactSourceLeaseExpired(
                                lease,
                                source.OwnerId,
                                source.LeaseGeneration
                            )
                        )
                            throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.Unavailable,
                                "Standalone Actor reply is waiting for its exact source acknowledgement.",
                                ZLinkRetryAdvice.RetryAfterBackoff
                            );
                        return;
                    }
                    var entry = CreateRelocationCompletionEntry(
                        participant.AuthorityKey,
                        relay,
                        source,
                        [payload]
                    );
                    // The Actor FIFO owns result order. Store I/O runs after the
                    // result is emitted without holding the Actor execution turn.
                    var previous = persistence;
                    var previousDelivery = delivery;
                    using (ExecutionContext.SuppressFlow())
                    {
                        persistence = Task.Run(async () =>
                        {
                            var expected = previous is null
                                ? authority
                                : (await previous.ConfigureAwait(false)).Authority;
                            return await completionStore
                                .AppendAsync(
                                    participant.AuthorityKey,
                                    expected,
                                    entry,
                                    ShutdownToken
                                )
                                .ConfigureAwait(false);
                        });
                        var published = persistence;
                        delivery = Task.Run(async () =>
                        {
                            await previousDelivery.ConfigureAwait(false);
                            var (current, storedEntry) = await published.ConfigureAwait(false);
                            if (storedEntry.Delivery != ZLinkRelocationCompletionDelivery.Pending)
                                return;
                            var acknowledged = await DeliverStoredRelocationReplyAsync(
                                    current,
                                    storedEntry,
                                    completionStore,
                                    ShutdownToken
                                )
                                .ConfigureAwait(false);
                            await completionStore
                                .AcknowledgeAsync(
                                    participant.AuthorityKey,
                                    current,
                                    storedEntry,
                                    acknowledged,
                                    ShutdownToken
                                )
                                .ConfigureAwait(false);
                        });
                    }
                }
            }
        );
        return completionStore is null ? prefix : WaitForPersistenceAsync();

        async Task WaitForPersistenceAsync()
        {
            // Await the prefix's terminal state before reading its last publication task.
            await Task.WhenAny(prefix).ConfigureAwait(false);
            await Task.WhenAll((Task?)persistence ?? Task.CompletedTask, delivery, prefix)
                .ConfigureAwait(false);
        }
    }

    internal static ZLinkRelocationCompletionBudget CreateStandaloneActorCompletionBudget(
        ZLinkRelocationEnvelope identity,
        IEnumerable<ZLinkActorHandoffFrame> acceptedFrames,
        ZLinkServiceWireCodec.RelocationCoordinatorFence coordinator,
        ulong targetAttemptGeneration
    )
    {
        var key = identity.Participants.Single().AuthorityKey;
        return new(
            identity.AggregateId,
            acceptedFrames
                .Where(frame => frame.RelocationReplyRouteId != 0)
                .Select(frame =>
                    CreateRelocationCompletionEntry(
                        key,
                        CreateStandaloneActorReplyRelay(
                            identity,
                            coordinator,
                            targetAttemptGeneration,
                            frame
                        ) with
                        {
                            TerminalResult = (uint)RequestResult.InternalError,
                            FailureCode = ServiceWireConstants.FrameworkErrorCode.RequestFailed,
                        },
                        frame.RequestSource
                            ?? throw new ZLinkRelocationDataLostException(
                                "Standalone Actor accepted request has no source fence."
                            ),
                        null
                    )
                )
        );
    }

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

    private ZLinkRelocationCompletionStore CreateRelocationCompletionStore(
        ZLinkRelocationCompletionBudget? budget = null
    ) =>
        new(
            Registration.Locations.ResolveStore()
                ?? throw new ZLinkConfigurationException(
                    "Relocation completion requires a Location Store."
                ),
            Registration.Locations.ResolveRelocationStore()
                ?? throw new ZLinkConfigurationException(
                    "Relocation completion requires a Relocation Store."
                ),
            budget,
            Registration.Locations.Options.PollingInterval
        );

    internal bool TryFinishDurableActorCompletion(
        Task prefix,
        ZLinkRelocationEnvelope identity,
        ZLinkLocationOwnerToken owner,
        ZLinkStandaloneActorRelocationProgressCoordinator coordinator,
        Func<ValueTask>? releaseAdmission = null
    ) =>
        TryRunDetached(
            "actor relocation completion delivery",
            async token =>
            {
                try
                {
                    // A prefix failure leaves any already-published results eligible for delivery.
                    await Task.WhenAny(prefix).WaitAsync(token).ConfigureAwait(false);
                    var current = await coordinator
                        .ReadAsync(identity, owner, token)
                        .ConfigureAwait(false);
                    var store = CreateRelocationCompletionStore();
                    var key = identity.Participants.Single().AuthorityKey;
                    await store
                        .RedeliverPendingAsync(
                            key,
                            current.Authority,
                            (authority, entry, ct) =>
                                DeliverStoredRelocationReplyAsync(authority, entry, store, ct),
                            token
                        )
                        .ConfigureAwait(false);
                    await prefix.ConfigureAwait(false);
                    await coordinator
                        .NormalizeSteadyAsync(identity, owner, token)
                        .ConfigureAwait(false);
                }
                finally
                {
                    if (releaseAdmission is not null)
                        await releaseAdmission().ConfigureAwait(false);
                }
            }
        );

    private async ValueTask<ZLinkRelocationCompletionDelivery> DeliverStoredRelocationReplyAsync(
        ZLinkAuthoritySnapshot authority,
        ZLinkRelocationCompletionEntry entry,
        ZLinkRelocationCompletionStore store,
        CancellationToken cancellationToken
    )
    {
        while (true)
        {
            try
            {
                var delivery = await TryDeliverStoredRelocationReplyAsync(
                        authority,
                        entry,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                if (delivery != ZLinkRelocationCompletionDelivery.Pending)
                    return delivery;

                authority = await store
                    .ReadCurrentAsync(
                        new ZLinkAuthorityKey(entry.ObjectKey),
                        authority,
                        cancellationToken
                    )
                    .ConfigureAwait(false);
                _ = await store
                    .ReadAsync(authority.Relocation!, cancellationToken, renew: true)
                    .ConfigureAwait(false);
            }
            catch (Exception error)
                when (ZLinkRelocationCompletionStore.IsStoreUnavailable(error, cancellationToken))
            {
                await store
                    .RetainIfPendingAsync(authority.Relocation!, cancellationToken)
                    .ConfigureAwait(false);
            }
            await Task.Delay(Registration.Locations.Options.PollingInterval, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    private async ValueTask<ZLinkRelocationCompletionDelivery> TryDeliverStoredRelocationReplyAsync(
        ZLinkAuthoritySnapshot authority,
        ZLinkRelocationCompletionEntry entry,
        CancellationToken cancellationToken
    )
    {
        var source = entry.Source;
        var sourceRid = RoutingId.FromHex(source.NodeRid);
        var sourceFence = new ZLinkServiceWireCodec.RequestSourceFence(
            source.OwnerId,
            source.OwnerLeaseGeneration,
            sourceRid,
            source.NodeGeneration
        );
        var relocation =
            authority.Relocation
            ?? throw new ZLinkRelocationDataLostException(
                "Stored reply authority lost its relocation."
            );
        var target =
            relocation.Target
            ?? throw new ZLinkRelocationDataLostException(
                "Stored reply authority lost its target."
            );
        var (relay, payload) = DecodeStoredRelocationReply(relocation, entry);
        relay = relay with
        {
            Coordinator = new(
                authority.OwnerId,
                checked((ulong)authority.OwnerLeaseGeneration),
                RoutingId.FromHex(target.NodeRid),
                target.NodeGeneration,
                authority.StoreVersion
            ),
        };
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
            when (ZLinkRelocationCompletionStore.IsStoreUnavailable(error, cancellationToken))
        {
            ZLinkFrameworkDebugLog.SpotDiscovery(
                $"relocation_completion_pending relocation={relocation.RelocationId:N} error={error.Message}"
            );
            return ZLinkRelocationCompletionDelivery.Pending;
        }
    }

    private static (
        ZLinkServiceWireCodec.ReplyRelayRecord Relay,
        byte[][] Payload
    ) DecodeStoredRelocationReply(
        ZLinkAuthorityRelocation relocation,
        ZLinkRelocationCompletionEntry entry
    )
    {
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
                || relocation.Target?.AttemptGeneration != relay.TargetAttemptGeneration
                || relocation.RelocationId.ToString("N")
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
