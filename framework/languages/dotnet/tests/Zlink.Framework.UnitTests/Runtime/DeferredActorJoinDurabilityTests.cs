using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class DeferredActorJoinDurabilityTests
{
    [Fact]
    public async Task Aggregate_actor_without_join_completion_is_a_no_op()
    {
        var authority = CreateAuthority();
        var relocation = new TestRelocationStore();
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey("actor-1");
        var envelope = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [
                new ZLinkRelocationParticipantEnvelope(
                    key,
                    ZLinkPlacementObjectKind.Actor,
                    7,
                    3,
                    ReadOnlyMemory<byte>.Empty,
                    [],
                    [],
                    ReadOnlyMemory<byte>.Empty,
                    ReadOnlyMemory<byte>.Empty)
            ]);
        await new ZLinkRelocationPublicationCoordinator(authority, relocation)
            .PublishAsync(
                new ZLinkRelocationPublicationRequest(
                    key,
                    authority.Snapshot.StoreVersion,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    authority.Snapshot.OwnerId,
                    authority.Snapshot.OwnerLeaseGeneration,
                    authority.Snapshot.Payload,
                    null,
                    envelope),
                CancellationToken.None);

        var recovered = await new ZLinkDeferredActorJoinCompletionJournal(
                authority,
                relocation)
            .RecoverAsync("actor-1", CancellationToken.None);

        Assert.Null(recovered);
    }

    [Fact]
    public void Completion_codec_accepts_maximum_reply_and_metadata()
    {
        var maximumString = new string('a', ushort.MaxValue);
        var maximumActorId = new string('a', 255);
        var maximumRoutingId = RoutingId.From(
            Enumerable.Repeat((byte)0x5a, byte.MaxValue).ToArray());
        var maximumReply = new byte[1024 * 1024];
        var record = new ZLinkDeferredJoinCompletionRecord(
            maximumActorId,
            7,
            new ZLinkActorJoinOperationId(11, 13),
            new ActorRef(
                maximumActorId,
                7,
                maximumString,
                maximumRoutingId),
            maximumString,
            maximumReply,
            ZLinkDeferredJoinCompletionCursor.Delivered);

        var encoded = ZLinkDeferredJoinCompletionCodec.Encode(record);
        var decoded = ZLinkDeferredJoinCompletionCodec.Decode(encoded);

        Assert.Equal(
            1024 * 1024
            + sizeof(ushort) + 255
            + 2 * (sizeof(ushort) + ushort.MaxValue)
            + sizeof(byte) + byte.MaxValue
            + 4 * sizeof(ulong)
            + sizeof(uint) + sizeof(int) + 3 * sizeof(byte),
            encoded.Length);
        Assert.Equal(record.ActorId, decoded.ActorId);
        Assert.Equal(record.ObjectGeneration, decoded.ObjectGeneration);
        Assert.Equal(record.OperationId, decoded.OperationId);
        Assert.Equal(record.Actor, decoded.Actor);
        Assert.Equal(record.ReplyContentType, decoded.ReplyContentType);
        Assert.Equal(record.Reply.ToArray(), decoded.Reply.ToArray());
        Assert.Equal(record.Cursor, decoded.Cursor);
    }

    [Fact]
    public async Task Completion_publication_preserves_the_canonical_relocation_participant()
    {
        var authority = CreateAuthority();
        var relocation = new TestRelocationStore();
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey("actor-1");
        var applicationState = new byte[] { 1, 3, 5, 7 };
        var acceptedJob = new ZLinkRelocationQueuedJob(9, new byte[] { 2, 4, 6 });
        var logicalTimer = new ZLinkRelocationLogicalTimer(
            "lease-renewal",
            1234,
            5000,
            new byte[] { 8, 10 });
        var recoveryPayload = new byte[] { 11, 12, 13 };
        var envelope = new ZLinkRelocationEnvelope(
            Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
            3,
            Enumerable.Repeat((byte)0x2a, 32).ToArray(),
            [
                new ZLinkRelocationParticipantEnvelope(
                    key,
                    ZLinkPlacementObjectKind.Actor,
                    7,
                    3,
                    applicationState,
                    [acceptedJob],
                    [logicalTimer],
                    recoveryPayload)
            ]);
        var originalAuthorityPayload = authority.Snapshot.Payload;
        await new ZLinkRelocationPublicationCoordinator(authority, relocation)
            .PublishAsync(
                new ZLinkRelocationPublicationRequest(
                    key,
                    authority.Snapshot.StoreVersion,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    authority.Snapshot.OwnerId,
                    authority.Snapshot.OwnerLeaseGeneration,
                    originalAuthorityPayload,
                    null,
                    envelope),
                CancellationToken.None);

        var actor = new ActorRef("actor-1", 7, "play", RoutingId.From("node-target"));
        var operation = new ZLinkActorJoinOperationId(19, 41);
        await new ZLinkDeferredActorJoinCompletionJournal(authority, relocation)
            .PrepareAsync(
                actor.ActorId,
                operation,
                actor,
                "raw",
                new byte[] { 14, 15 },
                CancellationToken.None);

        var recovered = await new ZLinkDeferredActorJoinCompletionJournal(
                authority,
                relocation)
            .RecoverAsync(actor.ActorId, CancellationToken.None);

        var participant = Assert.Single(recovered!.Envelope.Participants);
        Assert.Equal((ulong)7, participant.ObjectGeneration);
        Assert.Equal(applicationState, participant.ApplicationState.ToArray());
        var restoredJob = Assert.Single(participant.AcceptedJobs);
        Assert.Equal(acceptedJob.AcceptedSequence, restoredJob.AcceptedSequence);
        Assert.Equal(acceptedJob.Payload.ToArray(), restoredJob.Payload.ToArray());
        var restoredTimer = Assert.Single(participant.LogicalTimers);
        Assert.Equal(logicalTimer.TimerId, restoredTimer.TimerId);
        Assert.Equal(logicalTimer.DueUnixTimeMilliseconds, restoredTimer.DueUnixTimeMilliseconds);
        Assert.Equal(logicalTimer.PeriodMilliseconds, restoredTimer.PeriodMilliseconds);
        Assert.Equal(logicalTimer.Payload.ToArray(), restoredTimer.Payload.ToArray());
        Assert.Equal(recoveryPayload, participant.RecoveryPayload.ToArray());
        Assert.False(participant.CompletionPayload.IsEmpty);
        Assert.Equal((ulong)7, authority.Snapshot.ObjectGeneration);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Canonical_completion_cursors_survive_target_restart_without_losing_phase(
        bool legacyRecovery)
    {
        var authority = CreateAuthority();
        var relocation = new TestRelocationStore();
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey("actor-1");
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecodeDirect(
            authority.Snapshot.Payload.Span,
            out var actorAuthority));
        var relocationId =
            Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
        var root = ZLinkStandaloneActorRelocationRuntime.CreateImmutableRoot(
            authority.Snapshot,
            actorAuthority,
            new ZLinkStandaloneActorRelocationDestination(
                actorAuthority.CurrentSpotId,
                actorAuthority.CurrentSpotGeneration,
                actorAuthority.CurrentSpotKind,
                actorAuthority.NodeRid,
                actorAuthority.NodeGeneration,
                actorAuthority.MeshName,
                new ZLinkLocationOwnerToken(
                    actorAuthority.OwnerId,
                    checked((long)actorAuthority.OwnerLeaseGeneration))),
            relocationId,
            ReadOnlyMemory<byte>.Empty,
            [],
            default,
            new byte[] { 1 });
        if (legacyRecovery)
        {
            var rootParticipant = Assert.Single(root.Participants);
            var rootRecovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
                rootParticipant.RecoveryPayload.Span);
            var sourceFence = ZLinkActorRelocationSourceFenceCodec.Decode(
                rootRecovery.MembershipMutation.Span);
            var legacySourceFence = EncodeLegacySourceFence(
                sourceFence,
                new byte[] { 1 });
            var encoded = ZLinkCanonicalParticipantRecoveryCodec.Encode(
                rootRecovery with
                {
                    MembershipMutation = legacySourceFence,
                    OperationRecovery = ReadOnlyMemory<byte>.Empty
                });
            encoded[4] = 1;
            Array.Resize(
                ref encoded,
                encoded.Length - sizeof(uint) - sizeof(byte));
            root = root with
            {
                Participants =
                [
                    rootParticipant with { RecoveryPayload = encoded }
                ]
            };
        }
        var stored = await ZLinkRelocationTreeStore.PutAsync(
            relocation,
            root,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var canonicalPayload =
            ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
                authority.Snapshot.Payload.Span,
                new ZLinkCanonicalRelocationAuthorityState(
                    root.CanonicalRelocationHigh,
                    root.CanonicalRelocationLow,
                    1,
                    actorAuthority.NodeRid.ToHex(),
                    actorAuthority.NodeGeneration,
                    actorAuthority.OwnerId,
                    actorAuthority.OwnerLeaseGeneration,
                    actorAuthority.NodeRid.ToHex(),
                    actorAuthority.NodeGeneration,
                    actorAuthority.OwnerId,
                    actorAuthority.OwnerLeaseGeneration,
                    1,
                    actorAuthority.OwnerId,
                    actorAuthority.OwnerLeaseGeneration,
                    actorAuthority.NodeRid.ToHex(),
                    actorAuthority.NodeGeneration,
                    (byte)ZLinkStandaloneActorCanonicalPhase.Completed,
                    stored.Root.Reference,
                    stored.Root.ChecksumCrc32c,
                    0,
                    1),
                root);
        authority.ReplacePayload(canonicalPayload);
        Assert.True(
            ZLinkFrameworkRuntime.IsCompletedCanonicalActorRelocation(
                authority.Snapshot,
                stored.Root.Reference));
        Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            authority.Snapshot.Payload.Span,
            out var completedProjection));
        var activatedPayload =
            ZLinkCanonicalRelocationAuthorityStateCodec.ReplaceRelocationState(
                authority.Snapshot.Payload.Span,
                completedProjection.State with
                {
                    Phase =
                        (byte)ZLinkStandaloneActorCanonicalPhase.Activated,
                    SourceCleanupState = 0
                },
                root);
        Assert.False(
            ZLinkFrameworkRuntime.IsCompletedCanonicalActorRelocation(
                authority.Snapshot with { Payload = activatedPayload },
                stored.Root.Reference));

        var actor = new ActorRef(
            actorAuthority.ActorId,
            authority.Snapshot.ObjectGeneration,
            actorAuthority.MeshName,
            actorAuthority.NodeRid);
        var operation = new ZLinkActorJoinOperationId(23, 47);
        var completionReply = Enumerable.Repeat(
                (byte)0x5a,
                1024 * 1024)
            .ToArray();
        var prepared =
            await new ZLinkDeferredActorJoinCompletionJournal(
                    authority,
                    relocation)
                .PrepareAsync(
                    actor.ActorId,
                    operation,
                    actor,
                    "raw",
                    completionReply,
                    CancellationToken.None);

        var startupPublication =
            await new ZLinkRelocationPublicationCoordinator(
                    authority,
                    relocation)
                .RecoverAsync(key, CancellationToken.None);
        Assert.NotNull(startupPublication);
        var startupCandidate = new ZLinkRelocationRecoveryCandidate(
            new ZLinkRelocationManifestReference(
                startupPublication.Relocation.Reference,
                startupPublication.Relocation.ChecksumCrc32c,
                startupPublication.Envelope.AggregateId,
                startupPublication.Envelope.AggregateGeneration,
                startupPublication.Envelope.InventoryDigest),
            startupPublication.Envelope,
            [new ZLinkAuthorityEntry(key, startupPublication.Authority)]);
        Assert.True(
            ZLinkStandaloneActorRelocationRuntime.OwnsRecovery(
                startupCandidate));

        var afterPrepared = AssertCanonicalCursor(
            authority,
            prepared,
            ZLinkDeferredJoinCompletionCursor.Prepared);
        var committed =
            await new ZLinkDeferredActorJoinCompletionJournal(
                    authority,
                    relocation)
                .MarkCommittedAsync(
                    afterPrepared,
                    CancellationToken.None);
        var afterCommitted = AssertCanonicalCursor(
            authority,
            committed,
            ZLinkDeferredJoinCompletionCursor.Committed);
        var delivered =
            await new ZLinkDeferredActorJoinCompletionJournal(
                    authority,
                    relocation)
                .MarkDeliveredAsync(
                    afterCommitted,
                    CancellationToken.None);
        var afterDelivered = AssertCanonicalCursor(
            authority,
            delivered,
            ZLinkDeferredJoinCompletionCursor.Delivered);

        await new ZLinkDeferredActorJoinCompletionJournal(
                authority,
                relocation)
            .ReleaseAsync(afterDelivered, CancellationToken.None);

        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecodeDirect(
            authority.Snapshot.Payload.Span,
            out _));
        Assert.DoesNotContain(afterDelivered.Reference, relocation.Payloads.Keys);
        return;

        ZLinkDeferredJoinCompletionRoot AssertCanonicalCursor(
            TestAuthorityStore store,
            ZLinkDeferredJoinCompletionRoot expected,
            ZLinkDeferredJoinCompletionCursor cursor)
        {
            Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                store.Snapshot.Payload.Span,
                out var canonical));
            Assert.Equal(
                (byte)ZLinkStandaloneActorCanonicalPhase.Completed,
                canonical.Phase);
            Assert.Equal(expected.Reference, canonical.RelocationReference);
            Assert.Equal(
                expected.ChecksumCrc32c,
                canonical.RelocationChecksumCrc32c);
            var recovered =
                new ZLinkDeferredActorJoinCompletionJournal(store, relocation)
                    .RecoverAsync(actor.ActorId, CancellationToken.None)
                    .AsTask().GetAwaiter().GetResult();
            Assert.NotNull(recovered);
            Assert.Equal(operation, recovered.Completion.OperationId);
            Assert.Equal(cursor, recovered.Completion.Cursor);
            Assert.Equal(
                completionReply,
                recovered.Completion.Reply.ToArray());
            Assert.Equal(expected.Reference, recovered.Reference);
            var recoveredParticipant =
                Assert.Single(recovered.Envelope.Participants);
            var recoveredParticipantState =
                ZLinkCanonicalParticipantRecoveryCodec.Decode(
                    recoveredParticipant.RecoveryPayload.Span);
            if (legacyRecovery)
                Assert.True(
                    recoveredParticipantState.OperationRecovery.IsEmpty);
            else
                Assert.Equal(
                    new byte[] { 1 },
                    recoveredParticipantState.OperationRecovery.ToArray());
            var recoveredSourceFence =
                ZLinkActorRelocationSourceFenceCodec.Decode(
                    recoveredParticipantState.MembershipMutation.Span);
            if (legacyRecovery)
                Assert.Equal(
                    new byte[] { 1 },
                    recoveredSourceFence.LegacyRemoteJoinRecovery.ToArray());
            else
                Assert.True(
                    recoveredSourceFence.LegacyRemoteJoinRecovery.IsEmpty);
            return recovered;
        }
    }

    private static byte[] EncodeLegacySourceFence(
        ZLinkActorRelocationSourceFence sourceFence,
        byte[] recovery)
    {
        var versionOne =
            ZLinkActorRelocationSourceFenceCodec.Encode(sourceFence);
        var versionTwo = new byte[
            versionOne.Length + sizeof(uint) + recovery.Length];
        versionOne.CopyTo(versionTwo, 0);
        versionTwo[4] = 2;
        System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(
            versionTwo.AsSpan(versionOne.Length, sizeof(uint)),
            checked((uint)recovery.Length));
        recovery.CopyTo(
            versionTwo.AsSpan(versionOne.Length + sizeof(uint)));
        return versionTwo;
    }

    [Fact]
    public async Task Prepared_completion_survives_journal_recreation_with_raw_reply()
    {
        var authority = CreateAuthority();
        var relocation = new TestRelocationStore();
        var actor = new ActorRef("actor-1", 7, "play", RoutingId.From("node-target"));
        var operation = new ZLinkActorJoinOperationId(11, 29);

        var first = new ZLinkDeferredActorJoinCompletionJournal(authority, relocation);
        var prepared = await first.PrepareAsync(
            actor.ActorId,
            operation,
            actor,
            "application/octet-stream",
            new byte[] { 0, 1, 2, 255 },
            CancellationToken.None);

        var recovered = await new ZLinkDeferredActorJoinCompletionJournal(
                authority,
                relocation)
            .RecoverAsync(actor.ActorId, CancellationToken.None);

        Assert.NotNull(recovered);
        Assert.Equal(prepared.Reference, recovered.Reference);
        Assert.Equal(operation, recovered.Completion.OperationId);
        Assert.Equal(actor, recovered.Completion.Actor);
        Assert.Equal(new byte[] { 0, 1, 2, 255 }, recovered.Completion.Reply.ToArray());
        Assert.Equal(
            ZLinkDeferredJoinCompletionCursor.Prepared,
            recovered.Completion.Cursor);
        Assert.Equal((ulong)7, authority.Snapshot.ObjectGeneration);
    }

    [Fact]
    public async Task Committed_cursor_retries_callback_without_repeating_owner_commit()
    {
        var authority = CreateAuthority();
        var relocation = new TestRelocationStore();
        var actor = new ActorRef("actor-1", 7, "play", RoutingId.From("node-target"));
        var operation = new ZLinkActorJoinOperationId(13, 31);
        var journal = new ZLinkDeferredActorJoinCompletionJournal(authority, relocation);
        var root = await journal.PrepareAsync(
            actor.ActorId,
            operation,
            actor,
            null,
            ReadOnlyMemory<byte>.Empty,
            CancellationToken.None);
        root = await journal.MarkCommittedAsync(root, CancellationToken.None);
        var ownerCommitCount = authority.CompareExchangeCount;

        // Simulate process termination after the owner/membership commit but
        // before OnJoinCompletedAsync succeeds.
        var recovered = await new ZLinkDeferredActorJoinCompletionJournal(
                authority,
                relocation)
            .RecoverAsync(actor.ActorId, CancellationToken.None);

        Assert.NotNull(recovered);
        Assert.Equal(
            ZLinkDeferredJoinCompletionCursor.Committed,
            recovered.Completion.Cursor);
        Assert.Equal(ownerCommitCount, authority.CompareExchangeCount);
        Assert.Equal(operation, recovered.Completion.OperationId);
    }

    [Fact]
    public async Task Delivered_cursor_deduplicates_retry_and_releases_root_in_order()
    {
        var authority = CreateAuthority();
        var relocation = new TestRelocationStore();
        var actor = new ActorRef("actor-1", 7, "play", RoutingId.From("node-target"));
        var operation = new ZLinkActorJoinOperationId(17, 37);
        var journal = new ZLinkDeferredActorJoinCompletionJournal(authority, relocation);
        var root = await journal.PrepareAsync(
            actor.ActorId,
            operation,
            actor,
            "raw",
            new byte[] { 9, 8, 7 },
            CancellationToken.None);

        // Retrying Prepare with the same OperationId reuses the published
        // manifest instead of creating another callback record.
        var duplicate = await journal.PrepareAsync(
            actor.ActorId,
            operation,
            actor,
            "raw",
            new byte[] { 9, 8, 7 },
            CancellationToken.None);
        Assert.Equal(root.Reference, duplicate.Reference);

        root = await journal.MarkCommittedAsync(root, CancellationToken.None);
        root = await journal.MarkDeliveredAsync(root, CancellationToken.None);
        var recovered = await journal.RecoverAsync(actor.ActorId, CancellationToken.None);
        Assert.Equal(
            ZLinkDeferredJoinCompletionCursor.Delivered,
            recovered!.Completion.Cursor);

        var referenced = root.Reference;
        await journal.ReleaseAsync(root, CancellationToken.None);

        Assert.Null(await journal.RecoverAsync(actor.ActorId, CancellationToken.None));
        Assert.DoesNotContain(referenced, relocation.Payloads.Keys);
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecodeDirect(
            authority.Snapshot.Payload.Span,
            out var actorAuthority));
        Assert.Equal(actor.ActorId, actorAuthority.ActorId);
        Assert.Equal((ulong)7, authority.Snapshot.ObjectGeneration);
    }

    [Fact]
    public async Task Target_completion_waits_for_the_actor_mailbox_turn()
    {
        var state = new ZLinkActorRuntimeState("actor-1");
        state.BindNativeActorRef(
            new ZLinkBackendActorRef(
                RoutingId.From("node-target"),
                "actor-1",
                7));
        state.BindActorInstance(new TestActor("actor-1"));
        var entered = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new List<string>();

        var current = state.ExecuteLifecycleAsync(
            async _ =>
            {
                order.Add("message");
                entered.SetResult();
                await release.Task;
            },
            CancellationToken.None).AsTask();
        await entered.Task;
        var completion = state.ExecuteRelocationCompletionAsync(
            7,
            _ =>
            {
                order.Add("completion");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None).AsTask();

        Assert.False(completion.IsCompleted);
        release.SetResult();
        await Task.WhenAll(current, completion);
        Assert.Equal(["message", "completion"], order);
    }

    private static TestAuthorityStore CreateAuthority()
    {
        var nodeRid = RoutingId.From("node-source");
        var payload = ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                "avatar",
                "actor-1",
                "entry-source",
                1,
                ZLinkSpotKind.Entry,
                "owner-source",
                3,
                "mesh",
                nodeRid,
                5));
        return new TestAuthorityStore(
            new ZLinkAuthoritySnapshot(
                "1",
                payload,
                7,
                3,
                "owner-source",
                3,
                new ZLinkPlacementAllocation(
                    ZLinkPlacementAllocationState.Active,
                    ZLinkPlacementObjectKind.Actor,
                    "avatar",
                    new ZLinkMeshNodeDescriptorKey("mesh", nodeRid),
                    5,
                    new ZLinkCapacityVector(1, 0, null)),
                null,
                DateTimeOffset.UtcNow));
    }

    private sealed class TestAuthorityStore(ZLinkAuthoritySnapshot snapshot)
        : ZLinkLocationStoreTestDouble
    {
        internal ZLinkAuthoritySnapshot Snapshot { get; private set; } = snapshot;
        internal int CompareExchangeCount { get; private set; }

        internal void ReplacePayload(ReadOnlyMemory<byte> payload) =>
            Snapshot = Snapshot with { Payload = payload };

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ZLinkAuthorityReadResult>(
                new ZLinkAuthorityReadResult.Found(Snapshot));

        public override ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default)
        {
            CompareExchangeCount++;
            if (!string.Equals(
                    expectedStoreVersion,
                    Snapshot.StoreVersion,
                    StringComparison.Ordinal))
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(Snapshot)));
            if (mutation is not ZLinkAuthorityMutation.Put put)
                throw new NotSupportedException();
            Snapshot = Snapshot with
            {
                StoreVersion = (int.Parse(Snapshot.StoreVersion) + 1).ToString(),
                Payload = put.Payload,
                StoreNow = DateTimeOffset.UtcNow
            };
            return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                new ZLinkAuthorityCompareExchangeResult.Stored(Snapshot));
        }

    }

    private sealed class TestRelocationStore : IZLinkRelocationRepository
    {
        internal Dictionary<string, byte[]> Payloads { get; } = [];

        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var reference = Guid.NewGuid().ToString("n");
            Payloads.Add(reference, payload.ToArray());
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(
                new ZLinkRelocationStored(
                    reference,
                    ZLinkCrc32C.Compute(payload.Span),
                    now + retention,
                    now));
        }

        public ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
            string reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var bytes = payload.ToArray();
            if (Payloads.TryGetValue(reference, out var current)
                && !current.AsSpan().SequenceEqual(bytes))
                throw new InvalidDataException("Relocation reference collision.");
            Payloads[reference] = bytes;
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(bytes),
                now + retention,
                now));
        }

        public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ZLinkRelocationReadResult>(
                Payloads.TryGetValue(reference, out var payload)
                    ? new ZLinkRelocationReadResult.Found(payload)
                    : new ZLinkRelocationReadResult.Missing());

        public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkRelocationRenewResult>(
                Payloads.ContainsKey(reference)
                    ? new ZLinkRelocationRenewResult.Renewed(now + retention, now)
                    : new ZLinkRelocationRenewResult.Missing());
        }

        public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(
                Payloads.Remove(reference)
                    ? ZLinkRelocationDeleteResult.Deleted
                    : ZLinkRelocationDeleteResult.Missing);
    }

    private sealed class TestActor(string actorId) : IZLinkActor
    {
        public string ActorId { get; } = actorId;
        public IZLinkActorContext Context { get; } = new TestActorContext(actorId);
    }
}
