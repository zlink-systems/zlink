using System.Buffers.Binary;
using Zlink.Framework.Runtime.Locations;
using Wire = Systems.Zlink.Framework.Runtime.Protocol.ServiceWireCodec;

namespace Zlink.Framework.Runtime.Spots;

internal static class ZLinkCanonicalSpotRelocationWriter
{
    private static readonly Wire.DecodeContext WireContext =
        ZLinkRelocationEnvelopeCodec.FrozenWireContext(int.MaxValue);

    internal static ZLinkRelocationEnvelope CreateInitial(
        ZLinkRelocationEnvelope inventory,
        string spotId,
        string stableType,
        RoutingId targetNodeRid,
        long applicationVersion
    )
    {
        if (applicationVersion < 0)
            throw new ArgumentOutOfRangeException(nameof(applicationVersion));
        var ordered = inventory
            .Participants.OrderBy(
                static participant => participant.AuthorityKey.Value,
                StringComparer.Ordinal
            )
            .ToArray();
        var spot = ordered.Single(static participant =>
            participant.ObjectKind
                is ZLinkPlacementObjectKind.UserSpot
                    or ZLinkPlacementObjectKind.InstanceSpot
        );
        var spotParticipantId = checked((ulong)Array.IndexOf(ordered, spot) + 1);
        Wire.RelocationObjectIdentity identity =
            spot.ObjectKind == ZLinkPlacementObjectKind.InstanceSpot
                ? new Wire.RelocationObjectIdentityCase2(
                    Wire.StatefulObjectKind.InstanceSpot,
                    new(stableType),
                    new(spotId),
                    new(spot.ObjectGeneration)
                )
                : new Wire.RelocationObjectIdentityCase1(
                    Wire.StatefulObjectKind.UserSpot,
                    new(new(spotId), new(spot.ObjectGeneration)),
                    new(spot.AuthorityOwnerGeneration)
                );
        var states = ordered
            .Select(
                (participant, index) =>
                    new Wire.RelocationParticipantApplicationState(
                        new(checked((ulong)index + 1)),
                        new Wire.RelocationApplicationStateCase1(
                            Wire.Bool8.True,
                            new(participant.ApplicationState.ToArray())
                        )
                    )
            )
            .ToArray();
        var savedWork = new List<Wire.SavedWorkEntry>();
        for (var index = 0; index < ordered.Length; index++)
        {
            var participant = ordered[index];
            foreach (
                var job in participant.AcceptedJobs.OrderBy(static job => job.AcceptedSequence)
            )
            {
                var record =
                    participant == spot
                        ? CreateAcceptedRecord(job, spotId, spot, targetNodeRid)
                        : Wire.DecodeFrozenRecord(job.Payload.ToArray(), WireContext);
                savedWork.Add(
                    new(new(checked((ulong)index + 1)), new(job.AcceptedSequence), record)
                );
            }
        }
        var snapshots = spot
            .LogicalTimers.Select(timer =>
                (Timer: timer, Snapshot: ZLinkSpotTimerRelocationCodec.Decode(timer))
            )
            .ToArray();
        var timers = snapshots
            .OrderBy(static item => item.Timer.TimerId, StringComparer.Ordinal)
            .Select(item =>
            {
                var timer = item.Snapshot.Timer;
                return new Wire.RelocationTimerRegistration(
                    new(spotParticipantId),
                    new(timer.Name),
                    new(
                        item.Snapshot.HandlerType.AssemblyQualifiedName
                            ?? item.Snapshot.HandlerType.FullName
                            ?? item.Snapshot.HandlerType.Name
                    ),
                    new(checked((ulong)Math.Max(1, Math.Ceiling(timer.Period.TotalMilliseconds)))),
                    (Wire.TimerOverrunPolicyKind)timer.Options.OverrunPolicy,
                    new(checked((ulong)Math.Max(1, timer.Options.MaxCatchUpTicks))),
                    timer.Options.StopOnUnhandledException ? Wire.Bool8.True : Wire.Bool8.False,
                    new(timer.DeliveryIndex),
                    new(timer.LastScheduledIndex),
                    new(
                        checked(
                            (ulong)
                                (
                                    timer.NextScheduledAt ?? timer.StartedAt + timer.Period
                                ).ToUnixTimeMilliseconds()
                        )
                    )
                );
            })
            .ToArray();
        var pending = snapshots
            .Where(static item => item.Snapshot.Timer.PendingTick.HasValue)
            .OrderBy(static item => item.Timer.PendingAcceptedSequence)
            .Select(item =>
            {
                var tick = item.Snapshot.Timer.PendingTick!.Value;
                return new Wire.RelocationPendingTimerTick(
                    new(spotParticipantId),
                    new(item.Timer.PendingAcceptedSequence),
                    new(item.Timer.TimerId),
                    new(tick.DeliveryIndex),
                    new(tick.ScheduledIndex),
                    new(checked((ulong)tick.ScheduledAt.ToUnixTimeMilliseconds())),
                    new(tick.SkippedTicks)
                );
            })
            .ToArray();
        var aggregateId = inventory.AggregateId.ToByteArray(bigEndian: true);
        var bindings = new List<Wire.RelocationSessionBinding>();
        for (var index = 0; index < ordered.Length; index++)
            if (ordered[index].SessionBinding is { } binding)
                bindings.Add(
                    new(
                        new(checked((ulong)index + 1)),
                        new(binding.SessionOwnerNodeRid),
                        new(binding.SessionOwnerNodeGeneration),
                        new(binding.SessionOwnerId),
                        new(binding.SessionOwnerLeaseGeneration),
                        new(binding.SessionRid),
                        new(binding.BindingGeneration)
                    )
                );
        var encoded = Wire.EncodeLogicalRelocationEnvelopeV1(
            new(
                new(
                    new(BinaryPrimitives.ReadUInt64BigEndian(aggregateId.AsSpan(0, 8))),
                    new(BinaryPrimitives.ReadUInt64BigEndian(aggregateId.AsSpan(8, 8)))
                ),
                identity,
                new(applicationVersion),
                new(states),
                new(savedWork),
                new(timers),
                new(pending),
                new(bindings)
            ),
            WireContext
        );
        using var stream = new MemoryStream(encoded, writable: false);
        var canonical = ZLinkRelocationEnvelopeCodec.Decode(stream, inventory.InventoryDigest);
        var projected = ordered
            .Select(
                (participant, index) =>
                {
                    var state = canonical.Participants[index];
                    return participant with
                    {
                        AcceptedJobs = state.AcceptedJobs,
                        LogicalTimers = state.LogicalTimers,
                        CompletionPayload = participant.CompletionPayload,
                        CanonicalParticipantId = state.CanonicalParticipantId,
                        SessionBinding = state.SessionBinding,
                    };
                }
            )
            .ToArray();
        return inventory with
        {
            Participants = projected,
            CanonicalLogicalStream = canonical.CanonicalLogicalStream,
            CanonicalRelocationHigh = canonical.CanonicalRelocationHigh,
            CanonicalRelocationLow = canonical.CanonicalRelocationLow,
            CanonicalApplicationVersion = canonical.CanonicalApplicationVersion,
        };
    }

    internal static byte[] EncodeAcceptedJob(
        ZLinkRelocationQueuedJob job,
        string targetSpotId,
        ZLinkRelocationParticipantEnvelope spot,
        RoutingId targetNodeRid
    ) =>
        Wire.EncodeFrozenRecord(
            CreateAcceptedRecord(job, targetSpotId, spot, targetNodeRid),
            WireContext
        );

    private static Wire.FrozenRecord CreateAcceptedRecord(
        ZLinkRelocationQueuedJob job,
        string targetSpotId,
        ZLinkRelocationParticipantEnvelope spot,
        RoutingId targetNodeRid
    )
    {
        var journal = ZLinkSpotAcceptedJournal.Decode(job.Payload.Span);
        var source =
            job.RequestSource
            ?? throw new ZLinkRelocationDataLostException(
                "Accepted request source fence was not captured."
            );
        var ready = journal.ReadyInstanceAuthority;
        if (journal.SourceNodeGeneration == 0 || (ready is null && journal.OperationId == default))
            throw new ZLinkRelocationDataLostException(
                "Accepted request operation fence was not captured."
            );
        if (journal.Parts.Count != 2)
            throw new InvalidDataException(
                "Canonical accepted application payload must contain header and body parts."
            );
        var request = journal.ReplyRouteId != 0;
        // Preserve the pre-existing ordinary Spot journal representation.
        var rid = new Wire.Rid(
            ready is null ? RoutingId.From(source.NodeRid) : RoutingId.FromHex(source.NodeRid)
        );
        var generation = new Wire.NonzeroU64(source.NodeGeneration);
        Wire.FrozenSourceIdentity sourceIdentity = string.IsNullOrEmpty(journal.SpotId)
            ? new Wire.FrozenSourceIdentityCase0(
                Wire.FrozenSourceKind.Node,
                rid,
                generation,
                new(source.OwnerId),
                new(source.OwnerLeaseGeneration)
            )
            : new Wire.FrozenSourceIdentityCase1(
                Wire.FrozenSourceKind.Spot,
                rid,
                generation,
                new(source.OwnerId),
                new(source.OwnerLeaseGeneration),
                new(journal.SpotId)
            );
        Wire.MetadataFrame? metadata =
            journal.Metadata.Values.Count == 0
                ? null
                : new(
                    journal
                        .Metadata.Values.OrderBy(static pair => pair.Key, StringComparer.Ordinal)
                        .Select(static pair => new Wire.MetadataEntry(
                            new(pair.Key),
                            new(pair.Value)
                        ))
                        .ToArray()
                );
        var hasMetadata = metadata.HasValue ? Wire.Bool8.True : Wire.Bool8.False;
        var operation = new Wire.OperationIdOrZero(
            new(journal.OperationId.High),
            new(journal.OperationId.Low)
        );
        if (ready is { } authority)
            return new(
                Wire.MeshRecordKind.InstanceSpotActivation,
                sourceIdentity,
                hasMetadata,
                metadata,
                operation,
                request ? Wire.MeshOperationKind.InstanceSpotRequest : Wire.MeshOperationKind.None,
                request
                    ? new Wire.FrozenReplyRouteCase4(new(journal.ReplyRouteId))
                    : new Wire.FrozenReplyRouteOtherwise(),
                new Wire.FrozenRecordBodyCase13(
                    new Wire.InstanceRouteV1Case0(
                        Wire.InstanceRouteKind.Ready,
                        new(targetNodeRid),
                        new(journal.TargetNodeGeneration),
                        new(targetSpotId),
                        new(
                            new(spot.ObjectGeneration),
                            new(authority.OwnerId),
                            new(journal.AuthorityOwnerGeneration),
                            new(journal.OwnerLeaseGeneration),
                            new(authority.StoreVersion)
                        ),
                        journal.InstanceIntent ? Wire.Bool8.True : Wire.Bool8.False
                    ),
                    new(journal.SourceNodeGeneration),
                    request ? Wire.InstanceOperationKind.Request : Wire.InstanceOperationKind.Send,
                    Wire.DecodeApplicationPayloadEnvelopeV1(
                        ZLinkApplicationPayloadEnvelopeCodec.EncodeFrameworkMultipart(
                            journal.Parts
                        ),
                        WireContext
                    )
                )
            );
        var header = ZLinkEnvelopeCodec.DecodeHeader(journal.Parts[0].Span, validateFlow: true);
        var payload = new Wire.ApplicationPayloadEnvelopeV1(
            new(header.MessageName),
            new(header.ContentType),
            new(journal.Parts[1].ToArray())
        );
        var fence = new Wire.SpotRouteFence(
            new(new(targetSpotId), new(spot.ObjectGeneration)),
            new(RoutingId.From(targetNodeRid.ToHex())),
            new(journal.TargetNodeGeneration),
            new(journal.AuthorityOwnerGeneration),
            new(journal.OwnerLeaseGeneration)
        );
        return new(
            request ? Wire.MeshRecordKind.SpotRequest : Wire.MeshRecordKind.SpotSend,
            sourceIdentity,
            hasMetadata,
            metadata,
            operation,
            request ? Wire.MeshOperationKind.SpotRequest : Wire.MeshOperationKind.None,
            request
                ? new Wire.FrozenReplyRouteCase2(new(journal.ReplyRouteId))
                : new Wire.FrozenReplyRouteOtherwise(),
            request
                ? new Wire.FrozenRecordBodyCase5(fence, payload)
                : new Wire.FrozenRecordBodyCase4(fence, payload)
        );
    }
}
