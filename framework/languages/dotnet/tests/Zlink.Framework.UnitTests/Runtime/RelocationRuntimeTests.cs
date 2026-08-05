using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Configuration.Builders;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class RelocationRuntimeTests
{
    [Fact]
    public void Target_takeover_fence_advances_from_each_published_generation()
    {
        var first = ZLinkSpotRetireTargetRuntime.NextTakeoverFence(3, 17);
        var second = ZLinkSpotRetireTargetRuntime.NextTakeoverFence(
            first.TargetAttemptGeneration,
            first.AggregateGeneration);

        Assert.Equal((4UL, 18UL), first);
        Assert.Equal((5UL, 19UL), second);
    }

    [Fact]
    public void Target_takeover_requires_the_exact_application_version()
    {
        Assert.True(
            ZLinkSpotRetireTargetRuntime.MatchesTakeoverApplicationVersion(
                7,
                7));
        Assert.False(
            ZLinkSpotRetireTargetRuntime.MatchesTakeoverApplicationVersion(
                8,
                7));
        Assert.False(
            ZLinkSpotRetireTargetRuntime.MatchesTakeoverApplicationVersion(
                6,
                7));
    }

    [Fact]
    public void Target_takeover_rejects_corrupt_target_descriptor_fence()
    {
        var (_, candidate) = CreateCanonicalPublishedReconciliationFixture(
            sourceCleanupState: 0);
        var authority = Assert.Single(candidate.Authorities);
        Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            authority.Snapshot.Payload.Span,
            out var projection));
        Assert.True(
            ZLinkSpotRetireTargetRuntime.HasExactTargetOwnedSnapshot(
                authority.Snapshot,
                projection));

        var corrupt = authority.Snapshot with
        {
            Allocation = authority.Snapshot.Allocation with
            {
                Descriptor = new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("other-target"))
            }
        };

        Assert.False(
            ZLinkSpotRetireTargetRuntime.HasExactTargetOwnedSnapshot(
                corrupt,
                projection));
    }

    [Fact]
    public async Task Consecutive_target_takeovers_publish_and_restore_exact_roots()
    {
        var (_, fixture) = CreateCanonicalPublishedReconciliationFixture(
            sourceCleanupState: 0);
        var initialAuthority = Assert.Single(fixture.Authorities);
        var initial = fixture with
        {
            Envelope = fixture.Envelope with
            {
                Participants =
                [
                    fixture.Envelope.Participants[0] with
                    {
                        AuthorityKey = initialAuthority.Key
                    }
                ]
            }
        };
        var store = new TakeoverRepositoryStore(initial.Authorities);
        var relocation = new RecordingRelocationStore();

        var generationTwo =
            await ZLinkSpotRetireTargetRuntime
                .TryCommitTakeoverPublicationAsync(
                    store,
                    relocation,
                    initial,
                    CreateTakeoverDescriptor("target-two", 2),
                    TimeSpan.FromHours(24),
                    CancellationToken.None);
        Assert.NotNull(generationTwo);
        Assert.Equal(2UL, generationTwo.Envelope.AggregateGeneration);
        AssertTakeover(generationTwo, "target-two", 2);
        generationTwo = BindAuthorityKeys(generationTwo);

        var generationThree =
            await ZLinkSpotRetireTargetRuntime
                .TryCommitTakeoverPublicationAsync(
                    store,
                    relocation,
                    generationTwo,
                    CreateTakeoverDescriptor("target-three", 3),
                    TimeSpan.FromHours(24),
                    CancellationToken.None);
        Assert.NotNull(generationThree);
        Assert.Equal(3UL, generationThree.Envelope.AggregateGeneration);
        AssertTakeover(generationThree, "target-three", 3);

        static ZLinkMeshNodeDescriptor CreateTakeoverDescriptor(
            string rid,
            long lease) =>
            new(
                "mesh",
                RoutingId.From(rid),
                checked((ulong)lease),
                1,
                "tcp://127.0.0.1:1",
                new Dictionary<string, int>(),
                string.Empty,
                $"{rid}-owner",
                lease,
                DateTimeOffset.UtcNow)
            {
                State = ZLinkFrameworkRuntimeState.Serving,
                ObjectRole = ZLinkMeshNodeObjectRole.Server,
                ApplicationVersion = 1
            };

        static void AssertTakeover(
            ZLinkRelocationRecoveryCandidate candidate,
            string rid,
            ulong generation)
        {
            var authority = Assert.Single(candidate.Authorities);
            Assert.True(
                ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
                    authority.Snapshot.Payload.Span,
                    out var projection));
            Assert.Equal(RoutingId.From(rid).ToHex(),
                projection.State.TargetNodeRid);
            Assert.Equal(generation, projection.AggregateGeneration);
            Assert.Equal(candidate.Reference.Reference,
                projection.RelocationReference);
        }

        static ZLinkRelocationRecoveryCandidate BindAuthorityKeys(
            ZLinkRelocationRecoveryCandidate candidate) =>
            candidate with
            {
                Envelope = candidate.Envelope with
                {
                    Participants = candidate.Envelope.Participants.Select(
                            participant => participant with
                            {
                                AuthorityKey =
                                    ZLinkCanonicalParticipantRecoveryCodec
                                        .Decode(
                                            participant.RecoveryPayload.Span)
                                        .AuthorityKey
                            })
                        .ToArray()
                }
            };
    }

    [Fact]
    public void Source_cleanup_retry_accepts_only_byte_exact_steady_authority()
    {
        var (stage, _) = CreateCanonicalPublishedReconciliationFixture(
            sourceCleanupState: 1);
        var participant = Assert.Single(stage.Envelope.Participants);
        var target = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("target"));
        var owner = new ZLinkLocationOwnerToken("target-owner", 1);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        var payload = ZLinkSpotRetireTargetRuntime.BuildTargetReadyPayload(
            participant.ObjectKind,
            recovery.AuthorityPayload,
            target.Rid,
            1,
            owner,
            stage.Envelope.AggregateId);
        var snapshot = new ZLinkAuthoritySnapshot(
            "v-steady",
            payload,
            participant.ObjectGeneration,
            checked(participant.AuthorityOwnerGeneration + 1),
            owner.OwnerId,
            owner.LeaseGeneration,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.Active,
                participant.ObjectKind,
                recovery.StableType,
                target,
                1,
                new ZLinkCapacityVector(0, 1, null)),
            null,
            DateTimeOffset.UtcNow);

        Assert.True(
            ZLinkAggregateRelocationCoordinator
                .IsExactNormalizedTargetAuthority(
                    snapshot,
                    participant,
                    stage.Envelope.AggregateId,
                    target,
                    1,
                    owner));
    }

    [Fact]
    public void Source_cleanup_retry_rejects_corrupt_steady_authority()
    {
        var (stage, _) = CreateCanonicalPublishedReconciliationFixture(
            sourceCleanupState: 1);
        var participant = Assert.Single(stage.Envelope.Participants);
        var target = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("target"));
        var owner = new ZLinkLocationOwnerToken("target-owner", 1);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            participant.RecoveryPayload.Span);
        var payload = ZLinkSpotRetireTargetRuntime.BuildTargetReadyPayload(
                participant.ObjectKind,
                recovery.AuthorityPayload,
                target.Rid,
                1,
                owner,
                stage.Envelope.AggregateId)
            .ToArray();
        payload[^1] ^= 0x01;
        var snapshot = new ZLinkAuthoritySnapshot(
            "v-corrupt",
            payload,
            participant.ObjectGeneration,
            checked(participant.AuthorityOwnerGeneration + 1),
            owner.OwnerId,
            owner.LeaseGeneration,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.Active,
                participant.ObjectKind,
                recovery.StableType,
                target,
                1,
                new ZLinkCapacityVector(0, 1, null)),
            null,
            DateTimeOffset.UtcNow);

        Assert.False(
            ZLinkAggregateRelocationCoordinator
                .IsExactNormalizedTargetAuthority(
                    snapshot,
                    participant,
                    stage.Envelope.AggregateId,
                    target,
                    1,
                    owner));
    }

    [Fact]
    public async Task Owned_takeover_fence_is_aborted_when_commit_is_rejected()
    {
        var store = new TakeoverCommitStore(
            ZLinkAggregateCommitResult.Stale);
        var fence = new ZLinkAggregateFence(Guid.NewGuid(), 7);

        var result = await ZLinkSpotRetireTargetRuntime
            .CommitTakeoverFenceAsync(
                store,
                fence,
                ownsPreparedFence: true,
                CancellationToken.None);

        Assert.Equal(ZLinkAggregateCommitResult.Stale, result);
        Assert.Equal(1, store.AbortCount);
        Assert.False(store.StagingHeld);
        Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await store.PrepareAggregateAsync(null!, CancellationToken.None));
    }

    [Fact]
    public async Task Already_prepared_takeover_fence_is_not_aborted_by_observer()
    {
        var store = new TakeoverCommitStore(
            ZLinkAggregateCommitResult.Stale);

        _ = await ZLinkSpotRetireTargetRuntime.CommitTakeoverFenceAsync(
            store,
            new ZLinkAggregateFence(Guid.NewGuid(), 7),
            ownsPreparedFence: false,
            CancellationToken.None);

        Assert.Equal(0, store.AbortCount);
        Assert.True(store.StagingHeld);
    }

    [Fact]
    public async Task Owned_takeover_fence_is_aborted_when_commit_is_cancelled()
    {
        var store = new TakeoverCommitStore(
            new OperationCanceledException());

        await Assert.ThrowsAsync<OperationCanceledException>(
            () => ZLinkSpotRetireTargetRuntime.CommitTakeoverFenceAsync(
                    store,
                    new ZLinkAggregateFence(Guid.NewGuid(), 7),
                    ownsPreparedFence: true,
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(1, store.AbortCount);
        Assert.False(store.StagingHeld);
    }

    [Fact]
    public void TargetStage_UsesEachActorsPreparedAuthorityGeneration()
    {
        var stage = CreateTargetStageForHeldJournal() with
        {
            ActorTargetAuthorityOwnerGenerations =
                new Dictionary<string, ulong>(StringComparer.Ordinal)
                {
                    ["actor-a"] = 41,
                    ["actor-b"] = 73
                }
        };

        Assert.Equal(
            41UL,
            stage.TargetActorAuthorityOwnerGeneration("actor-a"));
        Assert.Equal(
            73UL,
            stage.TargetActorAuthorityOwnerGeneration("actor-b"));
    }

    [Fact]
    public void RelocationTreeChunkBounds_KeepExact64MiBAnd256GiBLimits()
    {
        Assert.Equal(
            1,
            ZLinkRelocationTreeStore.CalculateChunkCount(
                ZLinkRelocationTreeStore.ChunkBytes));
        Assert.Equal(
            2,
            ZLinkRelocationTreeStore.CalculateChunkCount(
                ZLinkRelocationTreeStore.ChunkBytes + 1UL));
        Assert.Equal(
            ZLinkRelocationTreeStore.MaxChunks,
            ZLinkRelocationTreeStore.CalculateChunkCount(
                ZLinkRelocationTreeStore.MaxLogicalBytes));
        Assert.Throws<InvalidOperationException>(
            () => ZLinkRelocationTreeStore.CalculateChunkCount(
                ZLinkRelocationTreeStore.MaxLogicalBytes + 1));
    }

    [Fact]
    public async Task RelocationTreeReader_DecodesCanonicalGoldenManifestAndChunk()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        static byte[] ReadHex(string path)
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            return Convert.FromHexString(
                document.RootElement.GetProperty("encodedHex").GetString()!);
        }

        var manifest = ReadHex(Path.GetFullPath(
            "../../runtime/protocol/golden/relocation-manifest-v1.json",
            frameworkRoot));
        var chunk = ReadHex(Path.GetFullPath(
            "../../runtime/protocol/golden/relocation-data-chunk-v1.json",
            frameworkRoot));
        var store = new RecordingRelocationStore();
        store.Seed("chunk-0", chunk);
        store.Seed("manifest", manifest);

        var read = await ZLinkRelocationTreeStore.ReadAsync(
            store,
            "manifest",
            ZLinkCrc32C.Compute(manifest),
            CancellationToken.None);

        Assert.Equal(596, read.LogicalLength);
        Assert.Equal(1, read.ChunkCount);
        Assert.Equal(
            Enumerable.Range(0, 32).Select(static value => (byte)value),
            read.Envelope.InventoryDigest.ToArray());

        var writer = new RecordingRelocationStore();
        var stored = await ZLinkRelocationTreeStore.PutAsync(
            writer,
            read.Envelope,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var rewritten = await ZLinkRelocationTreeStore.ReadAsync(
            writer,
            stored.Root.Reference,
            stored.Root.ChecksumCrc32c,
            CancellationToken.None);
        Assert.Equal(
            read.Envelope.CanonicalLogicalStream,
            rewritten.Envelope.CanonicalLogicalStream);
    }

    [Fact]
    public void CanonicalRelocationEnvelopeProjectsJournalTimersAndTerminalCompletions()
    {
        var logical = ReadCanonicalRelocationGolden();

        var envelope = ZLinkRelocationEnvelopeCodec.Decode(logical);

        Assert.Equal(logical, ZLinkRelocationEnvelopeCodec.Encode(envelope));
        var spot = envelope.Participants[0];
        var journal = Assert.Single(spot.AcceptedJobs);
        Assert.Equal<ulong>(1, journal.AcceptedSequence);
        Assert.NotEmpty(journal.Payload.ToArray());
        var timer = Assert.Single(spot.LogicalTimers);
        Assert.Equal("heartbeat", timer.TimerId);
        Assert.Equal(1_000, timer.PeriodMilliseconds);
        Assert.Equal(1_760_000_000_000, timer.DueUnixTimeMilliseconds);
        Assert.True(timer.Payload.IsEmpty);
        var canonicalTimer = Assert.IsType<ZLinkCanonicalLogicalTimer>(
            timer.CanonicalTimer);
        Assert.Equal("HeartbeatTimer", canonicalTimer.HandlerType);
        Assert.Equal((byte)ZLinkTimerOverrunPolicy.SkipLateTicks,
            canonicalTimer.OverrunPolicy);
        Assert.Equal<ulong>(4, canonicalTimer.LastCompletedDeliveryIndex);
        Assert.Equal<ulong>(5, canonicalTimer.LastCompletedScheduledIndex);
        var pendingTick = Assert.IsType<ZLinkCanonicalPendingTimerTick>(
            canonicalTimer.PendingTick);
        Assert.Equal<ulong>(2, pendingTick.AcceptedSequence);
        Assert.Equal<ulong>(5, pendingTick.DeliveryIndex);
        Assert.Equal<ulong>(6, pendingTick.ScheduledIndex);
        Assert.Equal<ulong>(1, pendingTick.SkippedTicks);
        Assert.True(spot.CompletionPayload.IsEmpty);
        Assert.Equal<ulong>(1, spot.CanonicalParticipantId);
        Assert.Equal<ulong>(2, spot.AcceptedBoundary);
        Assert.Equal<ulong>(0, spot.ReplayCursor);
        var completion = Assert.Single(spot.TerminalCompletions);
        Assert.Equal<ulong>(1, completion.AcceptedSequence);
        Assert.Equal<uint>(0, completion.TerminalResult);
        Assert.Equal<uint>(0, completion.ErrorCode);
        Assert.Equal(1, completion.DeliveryState);
        Assert.Equal(0, spot.PendingRelayCount);
        Assert.Empty(envelope.Participants[1].AcceptedJobs);
        Assert.Empty(envelope.Participants[1].LogicalTimers);
        Assert.True(envelope.Participants[1].CompletionPayload.IsEmpty);
    }

    [Fact]
    public void CanonicalRelocationEnvelopeRejectsTrailingAndTruncatedVectorBytes()
    {
        var logical = ReadCanonicalRelocationGolden();
        var trailing = logical.Append((byte)0).ToArray();
        var truncated = logical[..^1];

        Assert.Throws<InvalidDataException>(
            () => ZLinkRelocationEnvelopeCodec.Decode(trailing));
        Assert.ThrowsAny<IOException>(
            () => ZLinkRelocationEnvelopeCodec.Decode(truncated));
    }

    [Fact]
    public void CanonicalReplayCursorEditorPreservesOpaqueSections()
    {
        var envelope = ZLinkRelocationEnvelopeCodec.Decode(
            ReadCanonicalRelocationGolden());
        var before = envelope.Participants[0];

        var successor = ZLinkRelocationEnvelopeCodec.AdvanceCanonicalReplayCursor(
            envelope,
            before.CanonicalParticipantId,
            replayCursor: 1);
        var after = successor.Participants[0];

        Assert.Equal<ulong>(1, after.ReplayCursor);
        Assert.Empty(after.AcceptedJobs);
        Assert.Equal(before.ApplicationState.ToArray(), after.ApplicationState.ToArray());
        Assert.Equal(before.LogicalTimers[0].Payload.ToArray(), after.LogicalTimers[0].Payload.ToArray());
        Assert.Equal(before.TerminalCompletions[0].RawRecord.ToArray(),
            after.TerminalCompletions[0].RawRecord.ToArray());
        Assert.Equal(
            ZLinkRelocationEnvelopeCodec.Encode(successor),
            ZLinkRelocationEnvelopeCodec.Encode(
                ZLinkRelocationEnvelopeCodec.Decode(
                    ZLinkRelocationEnvelopeCodec.Encode(successor))));
    }

    [Fact]
    public void CanonicalTerminalCompletionAppenderPreservesAndCountsPendingRelay()
    {
        var envelope = ZLinkRelocationEnvelopeCodec.Decode(
            ReadCanonicalRelocationGolden());
        var completion = ZLinkRelocationEnvelopeCodec.CreateCanonicalTerminalCompletion(
            operationHigh: 0,
            operationLow: 43,
            sourceOwnerId: "request-source",
            sourceOwnerLeaseGeneration: 6,
            sourceNodeRid: "n",
            sourceNodeGeneration: 1,
            participantId: 1,
            acceptedSequence: 2,
            terminalResult: 0,
            errorCode: 0,
            deliveryState: 0,
            payload: new ZLinkCanonicalApplicationPayload(
                "ChatReply",
                "application/json",
                "{\"ok\":true}"u8.ToArray()));

        var successor = ZLinkRelocationEnvelopeCodec.AppendCanonicalTerminalCompletion(
            envelope,
            completion);
        var spot = successor.Participants[0];

        Assert.Equal(2, spot.TerminalCompletions.Count);
        Assert.Equal(1, spot.PendingRelayCount);
        Assert.Equal(completion.RawRecord.ToArray(),
            spot.TerminalCompletions[1].RawRecord.ToArray());
        Assert.Equal(ZLinkRelocationEnvelopeCodec.Encode(successor),
            ZLinkRelocationEnvelopeCodec.Encode(
                ZLinkRelocationEnvelopeCodec.Decode(
                    ZLinkRelocationEnvelopeCodec.Encode(successor))));
        var acknowledged = ZLinkRelocationEnvelopeCodec
            .AcknowledgeCanonicalTerminalCompletion(
                successor, 0, 43, "request-source", 6, "n", 1);
        var duplicate = ZLinkRelocationEnvelopeCodec
            .AcknowledgeCanonicalTerminalCompletion(
                acknowledged, 0, 43, "request-source", 6, "n", 1);
        Assert.Equal(0, acknowledged.Participants.Sum(
            static participant => participant.PendingRelayCount));
        Assert.Equal(
            ZLinkRelocationEnvelopeCodec.Encode(acknowledged),
            ZLinkRelocationEnvelopeCodec.Encode(duplicate));
        var alreadyTerminal = ZLinkRelocationEnvelopeCodec
            .CompleteCanonicalTerminalDelivery(
                successor, 0, 43, "request-source", 6, "n", 1, 2);
        Assert.Equal(
            2,
            alreadyTerminal.Participants
                .SelectMany(static participant => participant.TerminalCompletions)
                .Single(candidate => candidate.OperationLow == 43)
                .DeliveryState);
    }

    [Fact]
    public void CanonicalTerminalCompletionAppenderOrdersConcurrentParticipants()
    {
        var source = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [
                new ZLinkRelocationParticipantEnvelope(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                    ZLinkPlacementObjectKind.UserSpot,
                    10,
                    11,
                    new byte[] { 1 },
                    [],
                    []),
                new ZLinkRelocationParticipantEnvelope(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey("actor"),
                    ZLinkPlacementObjectKind.Actor,
                    12,
                    13,
                    new byte[] { 2 },
                    [],
                    [])
            ]);
        var root = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            source,
            "spot",
            "SpotType",
            RoutingId.From("target"),
            12);
        var participants = root.Participants
            .OrderBy(static participant => participant.CanonicalParticipantId)
            .ToArray();
        var laterParticipant = ZLinkRelocationEnvelopeCodec
            .CreateCanonicalTerminalCompletion(
                0, 42, "request-source", 6, "n", 1,
                participants[1].CanonicalParticipantId, 1,
                0, 0, 0, null);
        var earlierParticipant = ZLinkRelocationEnvelopeCodec
            .CreateCanonicalTerminalCompletion(
                0, 43, "request-source", 6, "n", 1,
                participants[0].CanonicalParticipantId, 1,
                0, 0, 0, null);

        root = ZLinkRelocationEnvelopeCodec.AppendCanonicalTerminalCompletion(
            root,
            laterParticipant);
        root = ZLinkRelocationEnvelopeCodec.AppendCanonicalTerminalCompletion(
            root,
            earlierParticipant);

        var roundTripped = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(root));
        Assert.Equal(
            new ulong[] { participants[0].CanonicalParticipantId,
                participants[1].CanonicalParticipantId },
            roundTripped.Participants
                .SelectMany(static participant =>
                    participant.TerminalCompletions)
                .Select(static completion => completion.ParticipantId)
                .ToArray());
    }

    [Fact]
    public void CanonicalAuthorityRelocationStateDerivesRootCompletionCounts()
    {
        var root = ZLinkRelocationEnvelopeCodec.Decode(
            ReadCanonicalRelocationGolden());
        var pending = ZLinkRelocationEnvelopeCodec.CreateCanonicalTerminalCompletion(
            0, 43, "request-source", 6, "n", 1, 1, 2,
            0, 0, 0, null);
        root = ZLinkRelocationEnvelopeCodec.AppendCanonicalTerminalCompletion(
            root, pending);
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath(
            "../../runtime/protocol/golden/durable-authority-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        var authority = Convert.FromHexString(
            document.RootElement.GetProperty("encodedHex").GetString()!);
        var state = new ZLinkCanonicalRelocationAuthorityState(
            0, 9, 1, "n", 1, "source", 1,
            "m", 2, "target", 2, 1,
            "coordinator", 1, "n", 1, 5,
            "root", 42, 1, 1);

        var encoded = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(authority, state, root);
        var counts = ZLinkCanonicalRelocationAuthorityStateCodec.ReadCounts(encoded);

        Assert.Equal<uint>(2, counts.TerminalCompletionCount);
        Assert.Equal<uint>(1, counts.PendingRelayCount);
    }

    [Fact]
    public void CanonicalReplySourceLeaseExpiryIsAnExactDurableTerminalProof()
    {
        var root = ZLinkRelocationEnvelopeCodec.Decode(
            ReadCanonicalRelocationGolden());
        var completion = ZLinkRelocationEnvelopeCodec.CreateCanonicalTerminalCompletion(
            0, 43, "request-source", 6, "n", 1, 1, 2,
            0, 0, 0, null);
        root = ZLinkRelocationEnvelopeCodec.AppendCanonicalTerminalCompletion(
            root, completion);

        var expired = ZLinkRelocationEnvelopeCodec
            .ExpireCanonicalTerminalSourceLease(
                root, 0, 43, "request-source", 6, "n", 1);
        var durableCompletion = expired.Participants
            .SelectMany(static participant => participant.TerminalCompletions)
            .Single(candidate => candidate.OperationLow == 43);

        Assert.Equal(3, durableCompletion.DeliveryState);
        Assert.Equal(0, expired.Participants.Sum(
            static participant => participant.PendingRelayCount));
        var now = DateTimeOffset.UtcNow;
        Assert.True(ZLinkFrameworkRuntime.IsExactSourceLeaseExpired(
            new ZLinkOwnerLeaseReadResult.Missing(),
            completion));
        Assert.True(ZLinkFrameworkRuntime.IsExactSourceLeaseExpired(
            new ZLinkOwnerLeaseReadResult.Found(
                new ZLinkLocationOwnerToken("request-source", 7),
                now.AddMinutes(1),
                now),
            completion));
        Assert.True(ZLinkFrameworkRuntime.IsExactSourceLeaseExpired(
            new ZLinkOwnerLeaseReadResult.Found(
                new ZLinkLocationOwnerToken("request-source", 6),
                now,
                now),
            completion));
        Assert.False(ZLinkFrameworkRuntime.IsExactSourceLeaseExpired(
            new ZLinkOwnerLeaseReadResult.Found(
                new ZLinkLocationOwnerToken("request-source", 6),
                now.AddMinutes(1),
                now),
            completion));
    }

    [Fact]
    public void SpotReplyRelaySenderRejectsStaleDurableTargetAttempt()
    {
        var targetRid = RoutingId.From("reply-target");
        var targetOwner = new ZLinkLocationOwnerToken("target-owner", 17);
        var state = new ZLinkCanonicalRelocationAuthorityState(
            11,
            13,
            19,
            RoutingId.From("reply-source").ToHex(),
            23,
            "source-owner",
            29,
            targetRid.ToHex(),
            31,
            targetOwner.OwnerId,
            checked((ulong)targetOwner.LeaseGeneration),
            37,
            "coordinator-owner",
            41,
            RoutingId.From("reply-coordinator").ToHex(),
            43,
            7,
            "reply-root",
            47,
            53,
            0);
        var canonical = new ZLinkCanonicalRelocationAuthorityProjection(
            state.RelocationHigh,
            state.RelocationLow,
            state.TargetAttemptGeneration,
            state.TargetOwnerId,
            state.TargetOwnerLeaseGeneration,
            state.Phase,
            state.RelocationReference,
            state.RelocationChecksumCrc32c,
            state.ApplicationVersion,
            1,
            1,
            state.SourceCleanupState,
            ReadOnlyMemory<byte>.Empty,
            state);

        Assert.True(ZLinkFrameworkRuntime.IsExactCanonicalReplyRelayTarget(
            canonical,
            11,
            13,
            19,
            targetRid,
            31,
            targetOwner));
        Assert.False(ZLinkFrameworkRuntime.IsExactCanonicalReplyRelayTarget(
            canonical,
            11,
            13,
            20,
            targetRid,
            31,
            targetOwner));
    }

    [Fact]
    public void ProductionSpotWriterCreatesCanonicalInitialRoot()
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request, "mesh", "Ping");
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            header, new { Value = 1 }, typeof(object), null);
        byte[] journal;
        using (var received = new ZLinkBackendRouteReceived(
                   parts,
                   RoutingId.From("source"),
                   "caller",
                   7,
                   static (_, _) => SubmitResult.Ok,
                   operationId: new MeshOperationId(1, 7),
                   targetNodeGeneration: 3,
                   authorityOwnerGeneration: 4,
                   ownerLeaseGeneration: 5,
                   sourceNodeGeneration: 2,
                   requestSource: new ZLinkServiceWireCodec.RequestSourceFence(
                       "caller-owner", 6, RoutingId.From("source"), 2)))
            journal = ZLinkSpotAcceptedJournal.Encode(received, 7);
        var job = new ZLinkRelocationQueuedJob(1, journal)
        {
            RequestSource = new ZLinkCanonicalRequestSourceFence(
                "caller-owner", 6, RoutingId.From("source").ToHex(), 2)
        };
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Encode(
            new ZLinkCanonicalParticipantRecovery(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                ZLinkPlacementObjectKind.UserSpot,
                10,
                11,
                "42",
                "SpotType",
                new byte[] { 9 },
                ReadOnlyMemory<byte>.Empty));
        var source = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [new ZLinkRelocationParticipantEnvelope(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                ZLinkPlacementObjectKind.UserSpot,
                10,
                11,
                new byte[] { 1 },
                [job],
                [],
                RecoveryPayload: recovery,
                CompletionPayload: ZLinkSpotRetireCompletionMarker.CreatePending())]);

        var canonical = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            source, "spot", "SpotType", RoutingId.From("target"), 12);

        Assert.False(canonical.CanonicalLogicalStream.IsEmpty);
        Assert.Equal<ulong>(1, canonical.Participants[0].AcceptedBoundary);
        var restored = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(canonical));
        var request = Assert.Single(restored.Participants[0].AcceptedJobs)
            .CanonicalRequest;
        Assert.NotNull(request);
        Assert.Equal("caller-owner", request!.Source.OwnerId);
        Assert.Equal("Ping", request.ApplicationPayload.PacketName);
        Assert.Equal(new byte[] { 1 }, restored.Participants[0]
            .ApplicationState.ToArray());
        var restoredRecovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(
            restored.Participants[0].RecoveryPayload.Span);
        Assert.Equal("42", restoredRecovery.ExpectedStoreVersion);
        Assert.Equal("SpotType", restoredRecovery.StableType);
        Assert.Equal(new byte[] { 9 }, restoredRecovery.AuthorityPayload.ToArray());
    }

    [Fact]
    public void CanonicalSpotWriterPreservesMemberActorAcceptedJournal()
    {
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner",
            6,
            RoutingId.From("caller"),
            7);
        var header = ZLinkStreamProtocolDefaults.EncodeHeader(
            new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Raw,
                ZlinkStreamHeaderFlags.None,
                default,
                "ActorPing",
                ZlinkStreamMetadata.Empty));
        var sourceActor = new ZLinkBackendActorRef(
            RoutingId.From("source"),
            "actor-1",
            42);
        var frame = new ZLinkActorHandoffFrame(
            [],
            0,
            requestSource.NodeRid.ToBytes().ToArray(),
            [],
            1,
            0,
            header.ToArray(),
            [9],
            0,
            new ZLinkBackendActorRouteContext(
                new MeshOperationId(1, 2),
                0,
                3,
                4,
                5),
            requestSource.NodeGeneration,
            requestSource);
        var actorJob = new ZLinkRelocationQueuedJob(
            1,
            ZLinkCanonicalActorAcceptedJournal.Encode(
                new ZLinkActorAcceptedRecord(frame, requestSource),
                sourceActor));
        var source = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [
                new ZLinkRelocationParticipantEnvelope(
                    ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                    ZLinkPlacementObjectKind.UserSpot,
                    10,
                    11,
                    new byte[] { 1 },
                    [],
                    []),
                new ZLinkRelocationParticipantEnvelope(
                    ZLinkActorAuthorityPayloadCodec.AuthorityKey("actor-1"),
                    ZLinkPlacementObjectKind.Actor,
                    42,
                    12,
                    new byte[] { 2 },
                    [actorJob],
                    [])
                {
                    AcceptedBoundary = 1
                }
            ]);

        var canonical = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            source,
            "spot",
            "SpotType",
            RoutingId.From("target"),
            12);
        var restored = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(canonical));
        var actor = restored.Participants.Single(
            static participant => participant.CanonicalParticipantId == 2);
        var restoredJob = Assert.Single(actor.AcceptedJobs);

        Assert.Equal<ulong>(1, actor.AcceptedBoundary);
        Assert.NotNull(restoredJob.CanonicalRequest);
        Assert.Equal(
            sourceActor,
            ZLinkCanonicalActorAcceptedJournal.Decode(
                restoredJob.Payload.Span,
                1).TargetActor);
    }

    [Fact]
    public async Task CanonicalTreeRoundTripPreservesFrozenLogicalStream()
    {
        var logical = ReadCanonicalRelocationGolden();
        var canonical = ZLinkRelocationEnvelopeCodec.Decode(logical);
        var store = new RecordingRelocationStore();

        var stored = await ZLinkRelocationTreeStore.PutAsync(
            store,
            canonical,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var restored = await ZLinkRelocationTreeStore.GetAsync(
            store,
            stored.Root.Reference,
            stored.Root.ChecksumCrc32c,
            CancellationToken.None);

        Assert.Equal(
            logical,
            restored.CanonicalLogicalStream.ToArray());
        Assert.Equal(
            logical,
            ZLinkRelocationEnvelopeCodec.Encode(restored));

        var participant = restored.Participants[0];
        var advanced = ZLinkRelocationEnvelopeCodec
            .AdvanceCanonicalReplayCursor(
                restored,
                participant.CanonicalParticipantId,
                participant.AcceptedBoundary);
        Assert.Equal(
            ZLinkRelocationEnvelopeCodec.Encode(advanced),
            ZLinkRelocationEnvelopeCodec.Encode(
                ZLinkRelocationEnvelopeCodec.Decode(
                    ZLinkRelocationEnvelopeCodec.Encode(advanced))));
    }

    [Fact]
    public void CanonicalV2ParticipantStateRejectsInvalidRecoveryRecord()
    {
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Encode(
            new ZLinkCanonicalParticipantRecovery(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                ZLinkPlacementObjectKind.UserSpot,
                10,
                11,
                "42",
                "SpotType",
                new byte[] { 9 },
                ReadOnlyMemory<byte>.Empty));
        recovery[0] ^= 0xff;
        var source = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [new ZLinkRelocationParticipantEnvelope(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                ZLinkPlacementObjectKind.UserSpot,
                10,
                11,
                new byte[] { 1 },
                [],
                [],
                RecoveryPayload: recovery)]);

        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalSpotRelocationWriter.CreateInitial(
                source, "spot", "SpotType", RoutingId.From("target"), 12));
    }

    [Fact]
    public void CanonicalJournalRejectsDuplicateOperationWithinExactSourceFence()
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request, "mesh", "Ping");
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            header, new { Value = 1 }, typeof(object), null);
        byte[] journal;
        using (var received = new ZLinkBackendRouteReceived(
                   parts,
                   RoutingId.From("source"),
                   "caller",
                   7,
                   static (_, _) => SubmitResult.Ok,
                   operationId: new MeshOperationId(1, 7),
                   targetNodeGeneration: 3,
                   authorityOwnerGeneration: 4,
                   ownerLeaseGeneration: 5,
                   sourceNodeGeneration: 2,
                   requestSource: new ZLinkServiceWireCodec.RequestSourceFence(
                       "caller-owner", 6, RoutingId.From("source"), 2)))
            journal = ZLinkSpotAcceptedJournal.Encode(received, 7);
        var sourceFence = new ZLinkCanonicalRequestSourceFence(
            "caller-owner", 6, RoutingId.From("source").ToHex(), 2);
        var source = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [new ZLinkRelocationParticipantEnvelope(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                ZLinkPlacementObjectKind.UserSpot,
                10,
                11,
                new byte[] { 1 },
                [
                    new ZLinkRelocationQueuedJob(1, journal)
                        { RequestSource = sourceFence },
                    new ZLinkRelocationQueuedJob(2, journal)
                        { RequestSource = sourceFence }
                ],
                [],
                CompletionPayload: ZLinkSpotRetireCompletionMarker.CreatePending())]);

        Assert.Throws<InvalidDataException>(() =>
            ZLinkCanonicalSpotRelocationWriter.CreateInitial(
                source, "spot", "SpotType", RoutingId.From("target"), 12));
    }

    [Fact]
    public void CanonicalRelocationEnvelopeRejectsInvalidUtf8Identity()
    {
        var logical = ReadCanonicalRelocationGolden();
        const int objectIdentityStart = 20;
        Assert.NotEqual(0, logical[objectIdentityStart]);
        logical[objectIdentityStart] = 0xff;

        Assert.Throws<InvalidDataException>(
            () => ZLinkRelocationEnvelopeCodec.Decode(logical));
    }

    private static byte[] ReadCanonicalRelocationGolden()
    {
        var frameworkRoot = Common.FrameworkTestEnvironment.GetFrameworkRoot();
        var path = Path.GetFullPath(
            "../../runtime/protocol/golden/relocation-envelope-v1.json",
            frameworkRoot);
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        return Convert.FromHexString(
            document.RootElement.GetProperty("logicalHex").GetString()!);
    }

    [Fact]
    public async Task RelocationTreeStore_AwaitsAsyncOnlyStoreWithoutLargeEagerBuffer()
    {
        var store = new AsyncOnlyRelocationStore();
        var envelope = CreateEnvelope();

        var stored = await ZLinkRelocationTreeStore.PutAsync(
            store,
            envelope,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var restored = await ZLinkRelocationTreeStore.GetAsync(
            store,
            stored.Root.Reference,
            stored.Root.ChecksumCrc32c,
            CancellationToken.None);

        Assert.Equal(envelope.Participants.Count, stored.ChunkCount);
        Assert.True(store.YieldCount >= 4);
        Assert.Equal(
            ZLinkRelocationEnvelopeCodec.ComputeEncodedSha256(envelope),
            ZLinkRelocationEnvelopeCodec.ComputeEncodedSha256(restored));
        Assert.All(
            store.PayloadSizes,
            static size => Assert.InRange(
                size,
                1,
                ZLinkRelocationTreeStore.ChunkBytes + 64 * 1024));
    }

    [Fact]
    public async Task RelocationTreeRenew_RejectsPartialProviderRenew()
    {
        var store = new AsyncOnlyRelocationStore();
        var stored = await ZLinkRelocationTreeStore.PutAsync(
            store,
            CreateEnvelope(),
            TimeSpan.FromHours(24),
            CancellationToken.None);
        store.FailRenewAt = 2;

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            () => ZLinkRelocationTreeStore.RenewTreeAsync(
                    store,
                    stored.Root.Reference,
                    stored.Root.ChecksumCrc32c,
                    TimeSpan.FromHours(24),
                    CancellationToken.None)
                .AsTask());
    }

    [Fact]
    public async Task RelocationTreeReadTreatsMissingAcceptedDataAsDataLoss()
    {
        var store = new RecordingRelocationStore();
        var stored = await ZLinkRelocationTreeStore.PutAsync(
            store,
            CreateEnvelope(),
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var chunkReference = store.Payloads.Keys.First(
            reference => reference != stored.Root.Reference);
        store.Payloads.Remove(chunkReference);

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            () => ZLinkRelocationTreeStore.GetAsync(
                    store,
                    stored.Root.Reference,
                    stored.Root.ChecksumCrc32c,
                    CancellationToken.None)
                .AsTask());
    }

    [Fact]
    public void ActorRelocationAuthorityPhase_PreservesStoredRouteAndHidesIngress()
    {
        var application = ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                "player",
                "actor-1",
                "spot-1",
                5,
                ZLinkSpotKind.User,
                "owner-target",
                3,
                "play",
                RoutingId.From("target-node"),
                9));
        var route = new ZLinkRemoteActorBoundSessionRoute(
            RoutingId.From("session-node"),
            RoutingId.From("session-rid"),
            "binding-token",
            4,
            7,
            11,
            "play",
            9,
            3,
            3,
            19);
        var relocationId = Guid.NewGuid();
        var encoded = ZLinkActorRelocationAuthorityPayloadCodec.Encode(
            new ZLinkActorRelocationAuthorityPayload(
                relocationId,
                ZLinkActorRelocationAuthorityPhase.Completed,
                route,
                application));

        Assert.True(ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
            encoded,
            out var decoded));
        Assert.Equal(relocationId, decoded.RelocationId);
        Assert.Equal(ZLinkActorRelocationAuthorityPhase.Completed, decoded.Phase);
        Assert.Equal(route, decoded.BoundSessionRoute);
        Assert.False(ZLinkActorAuthorityPayloadCodec.TryDecode(encoded, out _));
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(
            encoded,
            out var actor));
        Assert.Equal("actor-1", actor.ActorId);

        var steady = ZLinkActorRelocationAuthorityPayloadCodec.Encode(
            decoded with
            {
                Phase = ZLinkActorRelocationAuthorityPhase.Steady
            });
        Assert.True(ZLinkActorAuthorityPayloadCodec.TryDecode(
            steady,
            out var visible));
        Assert.Equal("actor-1", visible.ActorId);

        encoded[^1] ^= 0xff;
        Assert.False(ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
            encoded,
            out _));
    }

    [Fact]
    public void ActorRelocationRequiresLocationRuntimeBeforeAuthorityPublication()
    {
        var error = Assert.Throws<ZLinkFrameworkException>(
            () => Zlink.Framework.Runtime.Host.ZLinkFrameworkRuntime
                .RequireActorRelocationLocationLifecycle(
                    lifecycle: null,
                    actorId: "actor-1"));

        Assert.Equal(ZLinkFrameworkErrorKind.NotFound, error.Kind);
    }

    [Fact]
    public void RelocationPermitAdmissionIsAllOrNothing()
    {
        var options = new ZLinkLocationOptions
        {
            MaxActiveOutboundRelocations = 2,
            MaxActiveInboundRelocations = 1,
            MaxConcurrentRelocationCaptures = 1,
            MaxConcurrentRelocationRestores = 1,
            MaxRelocationPayloadInFlightBytes = 100
        };
        var permits = new ZLinkRelocationPermitPool(options);
        Assert.True(permits.TryAcquire(
            new ZLinkRelocationPermitRequest(1, 1, 1, 1, 60),
            out var first));

        Assert.False(permits.TryAcquire(
            new ZLinkRelocationPermitRequest(1, 1, 0, 0, 40),
            out _));
        Assert.Equal(
            new ZLinkRelocationPermitSnapshot(1, 1, 1, 1, 60, false),
            permits.Snapshot());

        first.Dispose();
        Assert.Equal(default, permits.Snapshot());
    }

    [Fact]
    public void RelocationPermitLimitUpdatesApplyOnlyToNewAttempts()
    {
        var options = new ZLinkLocationOptions
        {
            MaxActiveOutboundRelocations = 2,
            MaxConcurrentRelocationCaptures = 2,
            MaxRelocationPayloadInFlightBytes = 100
        };
        var permits = new ZLinkRelocationPermitPool(options);
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(60, capture: true),
            out var existing));

        options.MaxActiveOutboundRelocations = 1;
        options.MaxConcurrentRelocationCaptures = 1;
        options.MaxRelocationPayloadInFlightBytes = 50;
        Assert.False(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(1, capture: true),
            out _));
        Assert.Equal(60, existing.ReservedPayloadBytes);

        existing.Dispose();
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(50, capture: true),
            out var afterUpdate));
        afterUpdate.Dispose();
    }

    [Fact]
    public void OversizedAggregateOwnsThePayloadWindowExclusively()
    {
        var options = new ZLinkLocationOptions
        {
            MaxActiveOutboundRelocations = 4,
            MaxActiveInboundRelocations = 4,
            MaxConcurrentRelocationCaptures = 4,
            MaxConcurrentRelocationRestores = 4,
            MaxRelocationPayloadInFlightBytes = 100
        };
        var permits = new ZLinkRelocationPermitPool(options);
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(40, capture: true),
            out var normal));
        Assert.False(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(
                101,
                capture: true,
                allowOversizedPayload: true),
            out _));
        normal.Dispose();

        Assert.False(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(101, capture: true),
            out _));
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(
                101,
                capture: true,
                allowOversizedPayload: true),
            out var oversized));
        Assert.True(permits.Snapshot().OversizedPayloadActive);
        Assert.False(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Inbound(1, restore: true),
            out _));

        oversized.Dispose();
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Inbound(100, restore: true),
            out var after));
        after.Dispose();
        Assert.Equal(default, permits.Snapshot());
    }

    [Fact]
    public void InstanceSpotPayloadCannotUseOversizedAggregateException()
    {
        var permits = new ZLinkRelocationPermitPool(new ZLinkLocationOptions
        {
            MaxActiveOutboundRelocations = 1,
            MaxActiveInboundRelocations = 1,
            MaxConcurrentRelocationCaptures = 1,
            MaxConcurrentRelocationRestores = 1,
            MaxRelocationPayloadInFlightBytes = 100
        });

        Assert.False(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(
                payloadBytes: 101,
                capture: false,
                allowOversizedPayload: false),
            out _));
    }

    [Fact]
    public void RelocationPermitPayloadCanOnlyShrinkAtomically()
    {
        var permits = new ZLinkRelocationPermitPool(new ZLinkLocationOptions
        {
            MaxRelocationPayloadInFlightBytes = 100
        });
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(80, capture: true),
            out var lease));

        Assert.False(lease.TryShrinkPayload(81));
        Assert.Equal(80, lease.ReservedPayloadBytes);
        Assert.True(lease.TryShrinkPayload(30));
        Assert.Equal(
            new ZLinkRelocationPermitSnapshot(1, 0, 1, 0, 30, false),
            permits.Snapshot());

        lease.Dispose();
        Assert.False(lease.TryShrinkPayload(0));
        Assert.Equal(default, permits.Snapshot());
    }

    [Fact]
    public void StandaloneActorPermitReservesBothJournalsBeforeExactShrink()
    {
        var recreate = ZLinkRemoteActorJoinPackets
            .MeasureStandaloneMaintenancePayloadUpperBound(snapshot: false);
        var snapshot = ZLinkRemoteActorJoinPackets
            .MeasureStandaloneMaintenancePayloadUpperBound(snapshot: true);
        Assert.Equal(
            recreate
            + ZLinkRemoteActorJoinPackets
                .SnapshotApplicationStateReservationBytes,
            snapshot);
        Assert.True(recreate > 64L * 1024);

        var permits = new ZLinkRelocationPermitPool(new ZLinkLocationOptions
        {
            MaxRelocationPayloadInFlightBytes = recreate
        });
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(
                recreate,
                capture: false),
            out var lease));

        // A valid Recreate root can exceed the old fixed 64 KiB guess while
        // remaining below the deterministic queue plus hold reservation.
        Assert.True(lease.TryShrinkPayload(
            17L * 1024 * 1024));
        Assert.Equal(17L * 1024 * 1024, lease.ReservedPayloadBytes);
        lease.Dispose();
        Assert.Equal(default, permits.Snapshot());
    }

    [Fact]
    public void OversizedRelocationRemainsExclusiveAfterPayloadShrink()
    {
        var permits = new ZLinkRelocationPermitPool(new ZLinkLocationOptions
        {
            MaxRelocationPayloadInFlightBytes = 100
        });
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(
                101,
                capture: true,
                allowOversizedPayload: true),
            out var oversized));

        Assert.True(oversized.TryShrinkPayload(10));
        Assert.True(permits.Snapshot().OversizedPayloadActive);
        Assert.False(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Inbound(1, restore: true),
            out _));

        oversized.Dispose();
        Assert.Equal(default, permits.Snapshot());
    }

    [Fact]
    public void RecreateHeavySpotReservesOnlyFrameworkOwnedPayload()
    {
        ZLinkAuthorityKey[] keys =
        [
            new("spot"),
            new("actor-1"),
            new("actor-2"),
            new("actor-3"),
            new("actor-4")
        ];
        var jobs = new[]
        {
            new ZLinkAcceptedWorkRecord(1, new byte[] { 1, 2, 3 })
        };
        var timers = new[]
        {
            new ZLinkRelocationLogicalTimer(
                "timer",
                1,
                2,
                new byte[] { 4, 5 })
        };
        var participants = keys.Select(
                (key, index) => new ZLinkRelocationParticipantEnvelope(
                    key,
                    index == 0
                        ? ZLinkPlacementObjectKind.UserSpot
                        : ZLinkPlacementObjectKind.Actor,
                    1,
                    1,
                    ReadOnlyMemory<byte>.Empty,
                    index == 0
                        ? [new ZLinkRelocationQueuedJob(1, jobs[0].Payload)]
                        : [],
                    index == 0 ? timers : [],
                    CompletionPayload: index == 0
                        ? ZLinkSpotRetireCompletionMarker.CreatePending()
                        : ReadOnlyMemory<byte>.Empty))
            .ToArray();
        var encoded = ZLinkRelocationEnvelopeCodec.Encode(
            new ZLinkRelocationEnvelope(
                Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
                1,
                new byte[32],
                participants));

        Assert.Equal(
            encoded.LongLength + 16L * 1024 * 1024
            + keys.LongLength * ZLinkCanonicalParticipantRecoveryCodec
                .MaximumEncodedBytesWithEmptyMembership,
            ZLinkSpotRetireScheduler.CalculatePayloadReservation(
                snapshotParticipantCount: 0,
                participantKeys: keys,
                captured: jobs,
                timers: timers));
        Assert.Equal(
            28,
            ZLinkSpotRetireCompletionMarker.CreatePending().Length);
    }

    [Fact]
    public void SnapshotSpotReservesSixtyFourMiBPerSnapshotParticipant()
    {
        ZLinkAuthorityKey[] keys =
        [
            new("spot"),
            new("actor-1"),
            new("actor-2"),
            new("actor-3"),
            new("actor-4")
        ];
        var frameworkBytes =
            ZLinkSpotRetireScheduler.CalculatePayloadReservation(
                snapshotParticipantCount: 0,
                participantKeys: keys,
                captured: [],
                timers: []);

        Assert.Equal(
            frameworkBytes + 5L * 64 * 1024 * 1024,
            ZLinkSpotRetireScheduler.CalculatePayloadReservation(
                snapshotParticipantCount: 5,
                participantKeys: keys,
                captured: [],
                timers: []));
    }

    [Fact]
    public void SnapshotPayloadEnforcesTheSixtyFourMiBBoundary()
    {
        ZLinkSpotRetireScheduler.ValidateSnapshotPayloadSize(
            new byte[64 * 1024 * 1024]);

        var error = Assert.Throws<ZLinkFrameworkException>(
            () => ZLinkSpotRetireScheduler.ValidateSnapshotPayloadSize(
                new byte[64 * 1024 * 1024 + 1]));

        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, error.Kind);
    }

    [Fact]
    public void SpotStageIdempotencyDigestIncludesSourceFenceAndActorPayload()
    {
        var request = new ZLinkCanonicalSpotStageContext(
            Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
            7,
            11,
            "play",
            RoutingId.From("source").ToHex(),
            3,
            "source-owner",
            5,
            RoutingId.From("target").ToHex(),
            4,
            "target-owner",
            6,
            "spot-1",
            "room",
            false,
            "root-1",
            17,
            [new ZLinkCanonicalSpotActorDescriptor(
                "actor-1",
                "player",
                new byte[] { 1, 2, 3 })]);
        var digest =
            ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(request);

        Assert.Equal(
            digest,
            ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(request));
        Assert.False(digest.SequenceEqual(
            ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(
                request with { SourceOwnerLeaseGeneration = 8 })));
        Assert.False(digest.SequenceEqual(
            ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(
                request with { TargetAttemptGeneration = 12 })));
        Assert.False(digest.SequenceEqual(
            ZLinkSpotRetireTargetRuntime.ComputeStageRequestDigest(
                request with
                {
                    Actors =
                    [
                        request.Actors[0] with
                        {
                            AuthorityPayload = new byte[] { 1, 2, 4 }
                        }
                    ]
                })));
    }

    [Fact]
    public void TargetStagingAcceptsOnlyAnExactJournalPrefixOfFinalManifest()
    {
        var staging = CreateEnvelope();
        var stagedSpot = staging.Participants.Single(static participant =>
            participant.ObjectKind == ZLinkPlacementObjectKind.UserSpot);
        var final = staging with
        {
            Participants = staging.Participants.Select(participant =>
                    participant == stagedSpot
                        ? participant with
                        {
                            AcceptedJobs = participant.AcceptedJobs
                                .Append(new ZLinkRelocationQueuedJob(
                                    43,
                                    new byte[] { 4, 3 }))
                                .ToArray()
                        }
                        : participant)
                .ToArray()
        };

        Assert.True(ZLinkSpotRetireTargetRuntime.IsStagingPrefix(
            staging,
            final));
        var changedState = final with
        {
            Participants = final.Participants.Select(participant =>
                    participant == final.Participants[0]
                        ? participant with
                        {
                            ApplicationState = new byte[] { 9 }
                        }
                        : participant)
                .ToArray()
        };
        Assert.False(ZLinkSpotRetireTargetRuntime.IsStagingPrefix(
            staging,
            changedState));
        var changedPrefix = final with
        {
            Participants = final.Participants.Select(participant =>
                    participant == final.Participants[0]
                        ? participant with
                        {
                            AcceptedJobs = participant.AcceptedJobs.Select(
                                    (job, index) => index == 0
                                        ? job with
                                        {
                                            Payload = new byte[] { 0 }
                                        }
                                        : job)
                                .ToArray()
                        }
                        : participant)
                .ToArray()
        };
        Assert.False(ZLinkSpotRetireTargetRuntime.IsStagingPrefix(
            staging,
            changedPrefix));

        var pending = staging with
        {
            Participants = staging.Participants.Select(participant =>
                    participant.ObjectKind
                    == ZLinkPlacementObjectKind.UserSpot
                        ? participant with
                        {
                            CompletionPayload =
                                ZLinkSpotRetireCompletionMarker.CreatePending()
                        }
                        : participant)
                .ToArray()
        };
        var completed = final with
        {
            AggregateGeneration = checked(staging.AggregateGeneration + 1),
            Participants = final.Participants.Select(participant =>
                    participant.ObjectKind
                    == ZLinkPlacementObjectKind.UserSpot
                        ? participant with
                        {
                            CompletionPayload =
                                ZLinkSpotRetireCompletionMarker
                                    .CreateCompleted()
                        }
                        : participant)
                .ToArray()
        };
        Assert.True(ZLinkSpotRetireTargetRuntime.IsStagingPrefix(
            pending,
            completed));
    }

    [Fact]
    public async Task InProcessPublishedRetryBindsCanonicalPendingCleanupFromAuthority()
    {
        var (stage, candidate) =
            CreateCanonicalPublishedReconciliationFixture(
                sourceCleanupState: 0);
        using var services = new ServiceCollection().BuildServiceProvider();
        var target = new ZLinkSpotRetireTargetRuntime(
            services,
            null!,
            new ZLinkFrameworkRegistration());
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => target.ReconcilePublishedStageAsync(
                    stage,
                    candidate,
                    cancellation.Token)
                .AsTask());

        Assert.Equal(1, Volatile.Read(ref stage.AuthorityPublished));
        Assert.Null(stage.GetFinalRoot());
    }

    [Fact]
    public void CanonicalStagingRejectsAuthorityGenerationOtherThanPreparedTarget()
    {
        var (stage, _) =
            CreateCanonicalPublishedReconciliationFixture(
                sourceCleanupState: 0);
        var participant = Assert.Single(stage.Envelope.Participants);
        Assert.False(ZLinkSpotRetireTargetRuntime.IsStagingPrefix(
            stage.Envelope,
            stage.Envelope,
            stage.TargetAuthorityOwnerGenerationFor));
        var wrong = stage.Envelope with
        {
            Participants =
            [
                participant with
                {
                    AuthorityOwnerGeneration =
                        checked(stage.TargetAuthorityOwnerGeneration + 1)
                }
            ]
        };

        Assert.False(ZLinkSpotRetireTargetRuntime.IsStagingPrefix(
            stage.Envelope,
            wrong,
            stage.TargetAuthorityOwnerGenerationFor));
    }

    [Fact]
    public async Task InProcessPublishedRetryBindsCanonicalCompletedCleanupFromAuthority()
    {
        var (stage, candidate) =
            CreateCanonicalPublishedReconciliationFixture(
                sourceCleanupState: 1);
        using var services = new ServiceCollection().BuildServiceProvider();
        var target = new ZLinkSpotRetireTargetRuntime(
            services,
            null!,
            new ZLinkFrameworkRegistration());
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => target.ReconcilePublishedStageAsync(
                    stage,
                    candidate,
                    cancellation.Token)
                .AsTask());

        Assert.Equal(1, Volatile.Read(ref stage.AuthorityPublished));
        Assert.Equal(
            ("published-root", 37U),
            stage.GetFinalRoot());
    }

    [Fact]
    public async Task ReconcilerAndLateRelayMergeOnlyOneHeldJournal()
    {
        var stage = CreateTargetStageForHeldJournal();
        ZLinkSpotRetireHeldRecord[] first =
        [new(43, new byte[] { 1 })];
        ZLinkSpotRetireHeldRecord[] conflicting =
        [new(43, new byte[] { 2 })];
        using var start = new ManualResetEventSlim();

        var reconciler = Task.Run(() =>
        {
            start.Wait();
            return ZLinkSpotRetireTargetRuntime.TrySetHeldRecords(
                stage,
                first);
        });
        var relay = Task.Run(() =>
        {
            start.Wait();
            return ZLinkSpotRetireTargetRuntime.TrySetHeldRecords(
                stage,
                conflicting);
        });
        start.Set();
        var attempts = await Task.WhenAll(reconciler, relay);

        Assert.Single(attempts, static accepted => accepted);
        var winner = Assert.Single(stage.HeldRecords);
        Assert.Equal<ulong>(43, winner.AcceptedSequence);
        Assert.True(
            winner.Payload.Span.SequenceEqual(first[0].Payload)
            || winner.Payload.Span.SequenceEqual(conflicting[0].Payload));
    }

    [Fact]
    public async Task ExpiryCleanupKeepsStageWhenAuthorityReadFails()
    {
        var stage = CreateTargetStageForHeldJournal();
        var durableAbort = false;
        var removed = false;
        var aborted = false;

        await Assert.ThrowsAsync<IOException>(
            () => ZLinkSpotRetireTargetRuntime.TryCleanupExpiredStageAsync(
                    stage,
                    FailAuthorityRead,
                    () =>
                    {
                        durableAbort = true;
                        return ValueTask.FromResult(
                            ZLinkAggregateAbortResult.Aborted);
                    },
                    () => removed = true,
                    () =>
                    {
                        aborted = true;
                        return ValueTask.CompletedTask;
                    })
                .AsTask());

        Assert.False(durableAbort);
        Assert.False(removed);
        Assert.False(aborted);

        static async ValueTask<ZLinkRelocationRecoveryCandidate?>
            FailAuthorityRead()
        {
            await Task.Yield();
            throw new IOException("authority read failed");
        }
    }

    [Fact]
    public async Task ExpiryCleanupRetainsStageWhenCommitWinsAbortRace()
    {
        var stage = CreateTargetStageForHeldJournal();
        var removed = false;
        var aborted = false;
        var reads = 0;
        var envelope = stage.Envelope;
        var publication = new ZLinkRelocationRecoveryCandidate(
            new ZLinkRelocationManifestReference(
                "final-root",
                7,
                envelope.AggregateId,
                envelope.AggregateGeneration,
                envelope.InventoryDigest),
            envelope,
            []);

        var cleaned = await ZLinkSpotRetireTargetRuntime
            .TryCleanupExpiredStageAsync(
                stage,
                () => ValueTask.FromResult<
                    ZLinkRelocationRecoveryCandidate?>(
                    Interlocked.Increment(ref reads) == 1
                        ? null
                        : publication),
                static () => ValueTask.FromResult(
                    ZLinkAggregateAbortResult.Stale),
                () => removed = true,
                () =>
                {
                    aborted = true;
                    return ValueTask.CompletedTask;
                });

        Assert.False(cleaned);
        Assert.False(removed);
        Assert.False(aborted);
        Assert.Equal(2, reads);
    }

    [Fact]
    public async Task PublishAckLossRejectsAbortAndKeepsSourceSealedUntilTargetCompletion()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var target = new ZLinkSpotRetireTargetRuntime(
            services, null!, new ZLinkFrameworkRegistration());
        var permit = new CountingDisposable();
        var stage = CreateTargetStageForHeldJournal() with
        {
            InboundPermit = permit
        };
        Volatile.Write(ref stage.AuthorityPublished, 1);
        var fence = new ZLinkAggregateFence(
            stage.Envelope.AggregateId, stage.Envelope.AggregateGeneration);
        Assert.True(target.TryTrackStage(fence, stage));

        var abort = await target.AbortInboundAsync(
            fence,
            stage.SourceNodeRid,
            CancellationToken.None);

        Assert.False(abort);
        Assert.Equal(1, target.ActiveStageCount);
        Assert.Equal(TargetStageAbortState.Staged, stage.AbortState);
        Assert.Equal(0, permit.DisposeCount);
        var sourceResumed = false;
        await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    null,
                    static () => ValueTask.CompletedTask,
                    () => ValueTask.FromException(new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "committed target rejected abort",
                        ZLinkRetryAdvice.RetryAfterBackoff)),
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    })
                .AsTask());
        Assert.False(sourceResumed);

        target.CompleteStage(stage, TargetStageTerminalOutcome.Completed);
        Assert.Equal(0, target.ActiveStageCount);
        Assert.Equal(1, target.TerminalTombstoneCount);
        Assert.Equal(1, permit.DisposeCount);
    }

    [Fact]
    public async Task ExpiryCleanupRemovesOnlyTheDurableAbortWinner()
    {
        var stage = CreateTargetStageForHeldJournal();
        var removed = false;
        var aborted = false;

        var cleaned = await ZLinkSpotRetireTargetRuntime
            .TryCleanupExpiredStageAsync(
                stage,
                static () => ValueTask.FromResult<
                    ZLinkRelocationRecoveryCandidate?>(null),
                static () => ValueTask.FromResult(
                    ZLinkAggregateAbortResult.Aborted),
                () => removed = true,
                () =>
                {
                    aborted = true;
                    return ValueTask.CompletedTask;
                });

        Assert.True(cleaned);
        Assert.True(removed);
        Assert.True(aborted);
    }

    [Fact]
    public async Task ExpiryCleanupDoesNotRemoveStageBeforeChildCleanupSucceeds()
    {
        var stage = CreateTargetStageForHeldJournal();
        var removed = false;

        await Assert.ThrowsAsync<IOException>(() =>
            ZLinkSpotRetireTargetRuntime.TryCleanupExpiredStageAsync(
                    stage,
                    static () => ValueTask.FromResult<
                        ZLinkRelocationRecoveryCandidate?>(null),
                    static () => ValueTask.FromResult(
                        ZLinkAggregateAbortResult.Aborted),
                    () => removed = true,
                    static () => ValueTask.FromException(
                        new IOException("child cleanup failed")))
                .AsTask());

        Assert.False(removed);
        Assert.Equal(TargetStageAbortState.Staged, stage.AbortState);
    }

    [Fact]
    public async Task TargetAbortFailureKeepsAbortingStageForDuplicateResume()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var target = new ZLinkSpotRetireTargetRuntime(
            services,
            null!,
            new ZLinkFrameworkRegistration());
        var permit = new CountingDisposable();
        var stage = CreateTargetStageForHeldJournal() with
        {
            InboundPermit = permit
        };
        var fence = new ZLinkAggregateFence(
            stage.Envelope.AggregateId,
            stage.Envelope.AggregateGeneration);
        Assert.True(target.TryTrackStage(fence, stage));
        var attempts = 0;

        await Assert.ThrowsAsync<IOException>(() =>
            stage.RunAbortCleanupAsync(
                    () => Interlocked.Increment(ref attempts) == 1
                        ? ValueTask.FromException(
                            new IOException("actor rollback failed"))
                        : ValueTask.CompletedTask,
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(TargetStageAbortState.Aborting, stage.AbortState);
        Assert.True(await stage.RunAbortCleanupAsync(
            () =>
            {
                Interlocked.Increment(ref attempts);
                return ValueTask.CompletedTask;
            },
            CancellationToken.None));
        Assert.Equal(2, attempts);
        Assert.Equal(TargetStageAbortState.Aborted, stage.AbortState);
        target.CompleteStage(stage, TargetStageTerminalOutcome.Aborted);
        target.CompleteStage(stage, TargetStageTerminalOutcome.Aborted);
        Assert.Equal(1, permit.DisposeCount);
        Assert.Equal(1, target.TerminalTombstoneCount);
        Assert.False(await stage.RunAbortCleanupAsync(
            static () => ValueTask.CompletedTask,
            CancellationToken.None));
    }

    [Fact]
    public async Task NormalizationResponseLossRetriesFinalRootDeletion()
    {
        var stage = CreateTargetStageForHeldJournal();
        stage.RememberFinalRoot("completed-root", 17);
        var authorityNormalized = false;
        var deleted = new List<string>();

        await Assert.ThrowsAsync<IOException>(
            () => ZLinkSpotRetireTargetRuntime
                .ReleaseFinalRootAfterNormalizationAsync(
                    stage,
                    authorityAlreadyNormalized: false,
                    () =>
                    {
                        authorityNormalized = true;
                        return new ValueTask<ZLinkAggregateCommitResult>(
                            Task.FromException<ZLinkAggregateCommitResult>(
                                new IOException(
                                    "normalization response lost")));
                    },
                    reference =>
                    {
                        deleted.Add(reference);
                        return ValueTask.CompletedTask;
                    })
                .AsTask());

        Assert.True(authorityNormalized);
        Assert.Empty(deleted);

        await ZLinkSpotRetireTargetRuntime
            .ReleaseFinalRootAfterNormalizationAsync(
                stage,
                authorityAlreadyNormalized: true,
                commitNormalization: null,
                reference =>
                {
                    deleted.Add(reference);
                    return ValueTask.CompletedTask;
                });

        Assert.Equal(["completed-root"], deleted);
        Assert.Equal(
            ("completed-root", 17U),
            stage.GetFinalRoot());
    }

    [Fact]
    public void TargetStageTracksTheLatestVerifiedSuccessorRoot()
    {
        var stage = CreateTargetStageForHeldJournal();

        stage.RememberFinalRoot("completed-root-1", 17);
        stage.RememberFinalRoot("completed-root-2", 23);

        Assert.Equal(("completed-root-2", 23U), stage.GetFinalRoot());
    }

    [Fact]
    public async Task NormalizationGenerationExhaustionPreservesFinalRoot()
    {
        var stage = CreateTargetStageForHeldJournal();
        stage.RememberFinalRoot("completed-root", 17);
        var deleted = false;

        await Assert.ThrowsAsync<ZLinkAuthorityGenerationExhaustedException>(() =>
            ZLinkSpotRetireTargetRuntime.ReleaseFinalRootAfterNormalizationAsync(
                    stage,
                    authorityAlreadyNormalized: false,
                    static () => ValueTask.FromResult(
                        ZLinkAggregateCommitResult.GenerationExhausted),
                    _ =>
                    {
                        deleted = true;
                        return ValueTask.CompletedTask;
                    })
                .AsTask());

        Assert.False(deleted);
        Assert.Equal(("completed-root", 17U), stage.GetFinalRoot());
    }

    [Fact]
    public async Task TargetCatalogPublishesBeforeAuthorityNormalization()
    {
        var stage = CreateTargetStageForHeldJournal();
        var events = new List<string>();

        await ZLinkFrameworkRuntime.PublishCatalogBeforeNormalizationAsync(
            stage,
            () => events.Add("catalog"),
            () =>
            {
                Assert.Equal(1, Volatile.Read(
                    ref stage.LocalCatalogPublished));
                events.Add("normalize");
                return ValueTask.CompletedTask;
            });
        await ZLinkFrameworkRuntime.PublishCatalogBeforeNormalizationAsync(
            stage,
            () => events.Add("duplicate-catalog"),
            () =>
            {
                events.Add("normalize-retry");
                return ValueTask.CompletedTask;
            });

        Assert.Equal(
            ["catalog", "normalize", "normalize-retry"],
            events);
    }

    [Fact]
    public async Task CanonicalTargetCanPublishBeforeSourceCleanupNormalization()
    {
        var stage = CreateTargetStageForHeldJournal();
        var publications = 0;

        await ZLinkFrameworkRuntime.PublishCatalogBeforeNormalizationAsync(
            stage,
            () => publications++,
            normalizeAuthority: null);
        await ZLinkFrameworkRuntime.PublishCatalogBeforeNormalizationAsync(
            stage,
            () => publications++,
            normalizeAuthority: null);

        Assert.Equal(1, publications);
        Assert.Equal(1, Volatile.Read(ref stage.LocalCatalogPublished));
    }

    [Fact]
    public void TargetAdmissionOpensOnlyAfterPublicationAndOnlyOnce()
    {
        var stage = CreateTargetStageForHeldJournal();
        var opens = 0;

        Assert.Throws<InvalidOperationException>(() =>
            ZLinkFrameworkRuntime.OpenTargetAdmissionOnce(
                stage,
                () =>
                {
                    opens++;
                    return true;
                }));
        Volatile.Write(ref stage.Published, 1);

        ZLinkFrameworkRuntime.OpenTargetAdmissionOnce(
            stage,
            () =>
            {
                opens++;
                return true;
            });
        ZLinkFrameworkRuntime.OpenTargetAdmissionOnce(
            stage,
            () =>
            {
                opens++;
                return true;
            });

        Assert.Equal(1, opens);
        Assert.Equal(1, Volatile.Read(ref stage.AdmissionOpened));
    }

    [Fact]
    public void CompletedTombstonesDoNotConsumeActiveStageSlots()
    {
        var slots = new ZLinkStageSlotPool(1_024);

        for (var index = 0; index < 1_025; index++)
        {
            Assert.True(slots.TryAcquire(out var active));
            active.Dispose();
        }

        Assert.Equal(0, slots.ActiveCount);
        var held = Enumerable.Range(0, 1_024)
            .Select(_ =>
            {
                Assert.True(slots.TryAcquire(out var active));
                return active;
            })
            .ToArray();
        Assert.False(slots.TryAcquire(out _));
        foreach (var active in held)
            active.Dispose();
        Assert.Equal(0, slots.ActiveCount);
    }

    [Fact]
    public void TargetRuntimeBoundsAndExpiresCompletedStageTombstones()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var target = new ZLinkSpotRetireTargetRuntime(
            services,
            null!,
            new ZLinkFrameworkRegistration());

        for (var index = 0; index < 2_048; index++)
        {
            var envelope = CreateEnvelope() with { AggregateId = Guid.NewGuid() };
            var stage = CreateTargetStageForHeldJournal() with
            {
                Envelope = envelope
            };
            var fence = new ZLinkAggregateFence(
                envelope.AggregateId,
                envelope.AggregateGeneration);
            Assert.True(target.TryTrackStage(fence, stage));
            target.CompleteStage(stage, TargetStageTerminalOutcome.Completed);
        }

        Assert.Equal(0, target.ActiveStageCount);
        Assert.InRange(target.TerminalTombstoneCount, 1, 1_024);

        target.RemoveExpiredTombstones(
            DateTimeOffset.UtcNow
            + ZLinkSpotRetireTargetRuntime.TombstoneRetention
            + TimeSpan.FromSeconds(1));

        Assert.Equal(0, target.TerminalTombstoneCount);
    }

    [Fact]
    public void HeldIngressRequiresStrictSequenceAndBoundedCapacity()
    {
        ZLinkSpotRetireTargetRuntime.ValidateHeldRecords(
        [
            new ZLinkSpotRetireHeldRecord(3, new byte[] { 1 }),
            new ZLinkSpotRetireHeldRecord(4, new byte[] { 2 })
        ]);

        Assert.Throws<InvalidDataException>(
            () => ZLinkSpotRetireTargetRuntime.ValidateHeldRecords(
            [
                new ZLinkSpotRetireHeldRecord(4, new byte[] { 1 }),
                new ZLinkSpotRetireHeldRecord(4, new byte[] { 2 })
            ]));
        Assert.Throws<ZLinkFrameworkException>(
            () => ZLinkSpotRetireTargetRuntime.ValidateHeldRecords(
                Enumerable.Range(1, 1_025)
                    .Select(static index =>
                        new ZLinkSpotRetireHeldRecord(
                            checked((ulong)index),
                            []))
                    .ToArray()));
    }

    [Fact]
    public void SourceCleanupMarkersRemainDistinctAcrossRootEncoding()
    {
        var envelope = new ZLinkRelocationEnvelope(
            Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
            2,
            new byte[32],
            [
                new ZLinkRelocationParticipantEnvelope(
                    new ZLinkAuthorityKey("spot"),
                    ZLinkPlacementObjectKind.UserSpot,
                    3,
                    4,
                    ReadOnlyMemory<byte>.Empty,
                    [new ZLinkRelocationQueuedJob(
                        5,
                        new byte[] { 1, 2 })],
                    [],
                    CompletionPayload:
                        ZLinkSpotRetireCompletionMarker.CreatePending())
            ]);

        var restored = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(envelope));
        var spot = Assert.Single(restored.Participants);

        Assert.True(ZLinkSpotRetireCompletionMarker.IsPending(
            spot.CompletionPayload.Span));
        Assert.False(ZLinkSpotRetireCompletionMarker.IsCompleted(
            spot.CompletionPayload.Span));
        Assert.Equal<ulong>(5, Assert.Single(spot.AcceptedJobs)
            .AcceptedSequence);

        var completed = envelope with
        {
            Participants =
            [
                spot with
                {
                    CompletionPayload =
                        ZLinkSpotRetireCompletionMarker.CreateCompleted()
                }
            ]
        };
        var completedSpot = Assert.Single(
            ZLinkRelocationEnvelopeCodec.Decode(
                    ZLinkRelocationEnvelopeCodec.Encode(completed))
                .Participants);
        Assert.True(ZLinkSpotRetireCompletionMarker.IsCompleted(
            completedSpot.CompletionPayload.Span));
    }

    [Fact]
    public async Task PrecommitAbortRestoresRoutesOnlyAfterDurableAbort()
    {
        var events = new List<string>();
        await ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
            () =>
            {
                events.Add("durable-aborted");
                return ValueTask.CompletedTask;
            },
            () =>
            {
                events.Add("route-restored");
                return ValueTask.CompletedTask;
            },
            () =>
            {
                events.Add("target-cleaned");
                return ValueTask.CompletedTask;
            },
            () =>
            {
                events.Add("source-resumed");
                return ValueTask.CompletedTask;
            });

        Assert.Equal(
            [
                "durable-aborted",
                "route-restored",
                "target-cleaned",
                "source-resumed"
            ],
            events);
    }

    [Fact]
    public async Task PrecommitAbortCrashBeforeDurableAckKeepsRoutesSealed()
    {
        var routeRestored = false;
        var sourceResumed = false;

        await Assert.ThrowsAsync<IOException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    () => new ValueTask(Task.FromException(
                        new IOException("durable abort outcome unknown"))),
                    () =>
                    {
                        routeRestored = true;
                        return ValueTask.CompletedTask;
                    },
                    static () => ValueTask.CompletedTask,
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    })
                .AsTask());

        Assert.False(routeRestored);
        Assert.False(sourceResumed);
    }

    [Fact]
    public async Task PrecommitAbortTargetCleanupNackKeepsSourceSealed()
    {
        var sourceResumed = false;
        await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    static () => ValueTask.CompletedTask,
                    static () => ValueTask.CompletedTask,
                    () => new ValueTask(Task.FromException(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            "target cleanup NACK",
                            ZLinkRetryAdvice.RetryAfterBackoff))),
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    })
                .AsTask());
        Assert.False(sourceResumed);
    }

    [Fact]
    public async Task PrecommitAbortTargetCleanupTimeoutKeepsSourceSealed()
    {
        var sourceResumed = false;
        await Assert.ThrowsAsync<TimeoutException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    static () => ValueTask.CompletedTask,
                    static () => ValueTask.CompletedTask,
                    () => new ValueTask(Task.FromException(
                        new TimeoutException("target cleanup timeout"))),
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    })
                .AsTask());
        Assert.False(sourceResumed);
    }

    [Fact]
    public async Task PrecommitAbortRouteRestoreFailureStillStopsTargetAttempt()
    {
        var targetCleaned = false;
        var sourceResumed = false;

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    static () => ValueTask.CompletedTask,
                    () => ValueTask.FromException(
                        new ZLinkRelocationDataLostException(
                            "session route seal was not restored")),
                    () =>
                    {
                        targetCleaned = true;
                        return ValueTask.CompletedTask;
                    },
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    })
                .AsTask());

        Assert.True(targetCleaned);
        Assert.False(sourceResumed);
    }

    [Fact]
    public async Task PrecommitAbortSourceSealRestoreFailureIsNotSuccess()
    {
        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    static () => ValueTask.CompletedTask,
                    static () => ValueTask.CompletedTask,
                    static () => ValueTask.CompletedTask,
                    () => new ValueTask(Task.FromException(
                        new ZLinkRelocationDataLostException(
                            "source seal token mismatch"))))
                .AsTask());
    }

    [Fact]
    public void PendingRecoveryDoesNotFenceAnExactLiveSourceLifecycle()
    {
        var rid = RoutingId.From("source-node");
        var exact = SourceDescriptor(
            rid,
            lifecycleGeneration: 7,
            ownerId: "source-owner",
            leaseGeneration: 11);

        Assert.True(
            ZLinkSpotRetireTargetRuntime.IsExactSourceLifecycleStillLive(
                [exact],
                rid,
                7,
                "source-owner",
                11));
        Assert.False(
            ZLinkSpotRetireTargetRuntime.IsExactSourceLifecycleStillLive(
                [exact with { LifecycleGeneration = 8 }],
                rid,
                7,
                "source-owner",
                11));
        Assert.False(
            ZLinkSpotRetireTargetRuntime.IsExactSourceLifecycleStillLive(
                [exact with { LeaseGeneration = 12 }],
                rid,
                7,
                "source-owner",
                11));
    }

    [Fact]
    public void SpotMessageFollowEnforcesMessageAndByteBounds()
    {
        var owner = new ZLinkLocationOwnerToken("owner", 3);
        var messages = new ZLinkSpotMessageFollow(
            RoutingId.From("target"),
            5,
            11,
            12,
            7,
            19,
            owner,
            owner,
            DateTimeOffset.UtcNow.AddSeconds(30));
        var messageLeases = new List<ZLinkSpotMessageFollow.AdmissionLease>();
        for (var index = 0; index < 1_024; index++)
        {
            Assert.True(messages.TryAcquire(0, out var lease));
            messageLeases.Add(lease!);
        }
        Assert.False(messages.TryAcquire(0, out _));
        foreach (var lease in messageLeases)
            lease.Dispose();

        var bytes = new ZLinkSpotMessageFollow(
            RoutingId.From("target"),
            5,
            11,
            12,
            7,
            8,
            owner,
            owner,
            DateTimeOffset.UtcNow.AddSeconds(30));
        Assert.True(bytes.TryAcquire(16L * 1024 * 1024, out var byteLease));
        Assert.False(bytes.TryAcquire(1, out _));
        byteLease!.Dispose();
    }

    [Fact]
    public async Task SpotMessageFollowExpiryClosesAdmissionAndWaitsForAcceptedRelay()
    {
        var owner = new ZLinkLocationOwnerToken("source-owner", 3);
        var messageFollow = new ZLinkSpotMessageFollow(
            RoutingId.From("target-node"),
            5,
            11,
            12,
            7,
            8,
            owner,
            new ZLinkLocationOwnerToken("target-owner", 4),
            DateTimeOffset.UtcNow - TimeSpan.FromMilliseconds(1));
        Assert.True(messageFollow.TryAcquire(64, out var lease));

        var drained = messageFollow
            .WaitForExpiryAndDrainAsync(CancellationToken.None)
            .AsTask();

        Assert.False(drained.IsCompleted);
        Assert.False(messageFollow.TryAcquire(1, out _));
        lease!.Dispose();
        await drained.WaitAsync(TimeSpan.FromSeconds(1));
        Assert.Equal((0, 0L), messageFollow.AdmissionSnapshot());
    }

    [Fact]
    public void SpotMessageFollowCountsWireHeaderMetadataAndPayloadAtByteBoundary()
    {
        var owner = new ZLinkLocationOwnerToken("source-owner", 3);
        var targetOwner = new ZLinkLocationOwnerToken("target-owner", 4);
        var messageFollow = new ZLinkSpotMessageFollow(
            RoutingId.From("target-node"),
            5,
            11,
            12,
            7,
            8,
            owner,
            targetOwner,
            DateTimeOffset.UtcNow.AddSeconds(30));
        var metadata = ZLinkMeshMetadataCodec.Encode(
            new ZLinkMessageMetadata(
                new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["trace"] = new string('x', 900)
                }));
        using var empty = new Message();
        var fixedBytes = ZLinkServiceWireCodec.MeasureSpotMessageFollowEncodedBytes(
            request: true,
            new MeshOperationId(1, 2),
            "source-spot",
            "target-spot",
            5,
            RoutingId.From("target-node"),
            12,
            8,
            4,
            1,
            [empty],
            metadata);
        using var payload = new Message(checked((int)(
            ZLinkBoundedIngressAdmission.SourceIngressHoldByteCapacity
            - fixedBytes)));
        var encodedBytes = ZLinkServiceWireCodec.MeasureSpotMessageFollowEncodedBytes(
            request: true,
            new MeshOperationId(1, 2),
            "source-spot",
            "target-spot",
            5,
            RoutingId.From("target-node"),
            12,
            8,
            4,
            1,
            [payload],
            metadata);

        Assert.Equal(
            ZLinkBoundedIngressAdmission.SourceIngressHoldByteCapacity,
            encodedBytes);
        Assert.True(messageFollow.TryAcquire(encodedBytes, out var lease));
        Assert.Equal((1, encodedBytes), messageFollow.AdmissionSnapshot());
        lease!.Dispose();
        Assert.False(messageFollow.TryAcquire(encodedBytes + 1, out _));
    }

    [Fact]
    public void SpotMessageFollowRejectsExpiredAndLoopedSourceRoutesAndClosesFullAdmission()
    {
        var owner = new ZLinkLocationOwnerToken("source-owner", 3);
        var targetOwner = new ZLinkLocationOwnerToken("target-owner", 4);
        var now = DateTimeOffset.UtcNow;
        var messageFollow = new ZLinkSpotMessageFollow(
            RoutingId.From("target-node"),
            5,
            11,
            12,
            7,
            8,
            owner,
            targetOwner,
            now.AddSeconds(30),
            new ZLinkBoundedIngressAdmission(1, 64));
        using var current = MessageFollowSpotReceived(messageFollowHopCount: 1);
        using var looped = MessageFollowSpotReceived(messageFollowHopCount: 8);

        Assert.True(messageFollow.MatchesSourceRoute(current, 5, owner, now));
        Assert.False(messageFollow.MatchesSourceRoute(current, 5, owner,
            now.AddSeconds(31)));
        Assert.False(messageFollow.MatchesSourceRoute(looped, 5, owner, now));
        Assert.True(messageFollow.TryAcquire(64, out var lease));
        Assert.False(messageFollow.TryAcquire(0, out _));
        lease!.Dispose();
    }

    [Fact]
    public void SpotMessageFollowKeepsActiveRouteAfterRejectedFrame()
    {
        var now = DateTimeOffset.UtcNow;
        var owner = new ZLinkLocationOwnerToken("source-owner", 3);
        var messageFollow = new ZLinkSpotMessageFollow(
            RoutingId.From("target-node"),
            5,
            11,
            12,
            7,
            8,
            owner,
            new ZLinkLocationOwnerToken("target-owner", 4),
            now.AddSeconds(30));

        Assert.False(messageFollow.ShouldRemoveAfterRejectedFrame(now));
        Assert.True(
            messageFollow.ShouldRemoveAfterRejectedFrame(
                now.AddSeconds(30)));
    }

    [Fact]
    public void SpotMessageFollowRequestSubmitExceptionReleasesAdmissionAndDisposesIngress()
    {
        var owner = new ZLinkLocationOwnerToken("source-owner", 3);
        var messageFollow = new ZLinkSpotMessageFollow(
            RoutingId.From("target-node"),
            5,
            11,
            12,
            7,
            8,
            owner,
            new ZLinkLocationOwnerToken("target-owner", 4),
            DateTimeOffset.UtcNow.AddSeconds(30),
            new ZLinkBoundedIngressAdmission(1, 64));
        Assert.True(messageFollow.TryAcquire(64, out var lease));
        var payload = new Message((ReadOnlySpan<byte>)new byte[] { 1, 2, 3 });
        var received = new ZLinkBackendRouteReceived(
            [payload],
            RoutingId.From("caller-node"),
            "source-spot",
            7,
            static (_, _) => SubmitResult.Ok,
            operationId: new MeshOperationId(1, 2));
        var failure = new ZlinkSubmitException(
            ZlinkSubmitException.ErrorCode.NotConnected);

        var thrown = Assert.Throws<ZlinkSubmitException>(
            () => ZLinkSpotActivation.SubmitSpotMessageFollowRequest(
                received,
                lease!,
                _ => throw failure));

        Assert.Same(failure, thrown);
        Assert.Equal((0, 0L), messageFollow.AdmissionSnapshot());
        Assert.ThrowsAny<Exception>(() => payload.ToArray());
    }

    [Fact]
    public void SpotMessageFollowPreservesRemainingDeadlineAndDropsLateReply()
    {
        var now = DateTimeOffset.FromUnixTimeMilliseconds(4_102_444_800_000);
        var deadline = checked((ulong)now.AddMilliseconds(1_250)
            .ToUnixTimeMilliseconds());
        using var timed = new ZLinkBackendRouteReceived(
            [new Message((ReadOnlySpan<byte>)new byte[] { 1 })],
            RoutingId.From("caller-node"),
            "source-spot",
            7,
            static (_, _) => SubmitResult.Ok,
            operationId: new MeshOperationId(1, 2),
            deadlineUnixMs: deadline);
        Assert.Equal(
            TimeSpan.FromMilliseconds(1_250),
            ZLinkSpotActivation.RemainingRequestTimeout(timed, now));
        Assert.Equal(
            TimeSpan.Zero,
            ZLinkSpotActivation.RemainingRequestTimeout(
                timed,
                now.AddMilliseconds(1_250)));

        var owner = new ZLinkLocationOwnerToken("source-owner", 3);
        var messageFollow = new ZLinkSpotMessageFollow(
            RoutingId.From("target-node"),
            5,
            11,
            12,
            7,
            8,
            owner,
            new ZLinkLocationOwnerToken("target-owner", 4),
            DateTimeOffset.UtcNow.AddSeconds(30),
            new ZLinkBoundedIngressAdmission(1, 64));
        Assert.True(messageFollow.TryAcquire(64, out var lease));
        var replyCount = 0;
        var expired = new ZLinkBackendRouteReceived(
            [new Message((ReadOnlySpan<byte>)new byte[] { 2 })],
            RoutingId.From("caller-node"),
            "source-spot",
            8,
            (_, _) =>
            {
                replyCount++;
                return SubmitResult.Ok;
            },
            operationId: new MeshOperationId(1, 3),
            deadlineUnixMs: checked((ulong)DateTimeOffset.UtcNow
                .AddSeconds(-1)
                .ToUnixTimeMilliseconds()));
        RequestCallback? callback = null;
        Assert.True(ZLinkSpotActivation.SubmitSpotMessageFollowRequest(
            expired,
            lease!,
            candidate =>
            {
                callback = candidate;
                return true;
            }));
        Assert.NotNull(callback);
        callback!(
            RequestResult.Ok,
            [new Message((ReadOnlySpan<byte>)new byte[] { 3 })]);

        Assert.Equal(0, replyCount);
        Assert.Equal((0, 0L), messageFollow.AdmissionSnapshot());
    }

    [Fact]
    public void StaleSpotMessageFollowReturnsTypedGenerationError()
    {
        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            "mesh",
            "Ping");
        var request = ZLinkEnvelopeCodec.EncodeParts(
            header,
            new { Value = 1 },
            typeof(object),
            null);
        ZLinkEnvelopeHeader? replyHeader = null;
        var received = new ZLinkBackendRouteReceived(
            request,
            RoutingId.From("caller-node"),
            "source-spot",
            7,
            (parts, _) =>
            {
                replyHeader = ZLinkEnvelopeCodec.DecodeHeader(parts);
                return SubmitResult.Ok;
            });

        ZLinkSpotActivationDispatcher.RejectApplicationRouteForStaleMessageFollow(
            received,
            "mesh");

        Assert.NotNull(replyHeader);
        Assert.Equal(
            nameof(ZLinkFrameworkErrorKind.InvalidOperation),
            replyHeader!.ErrorCode);
    }

    private static ZLinkBackendRouteReceived MessageFollowSpotReceived(
        byte messageFollowHopCount) => new(
        [new Message((ReadOnlySpan<byte>)new byte[] { 1 })],
        RoutingId.From("caller-node"),
        "source-spot",
        7,
        static (_, _) => SubmitResult.Ok,
        operationId: new MeshOperationId(1, 2),
        targetNodeGeneration: 11,
        authorityOwnerGeneration: 7,
        ownerLeaseGeneration: 3,
        messageFollowHopCount: messageFollowHopCount);

    [Fact]
    public void ActorRelocationUsesSeparateUnitCaptureAndMeasuredPayloadLeases()
    {
        var permits = new ZLinkRelocationPermitPool(new ZLinkLocationOptions
        {
            MaxActiveOutboundRelocations = 1,
            MaxActiveInboundRelocations = 1,
            MaxConcurrentRelocationCaptures = 1,
            MaxConcurrentRelocationRestores = 1,
            MaxRelocationPayloadInFlightBytes = 100
        });

        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.OutboundUnit(),
            out var unit));
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Capture(),
            out var capture));
        Assert.Equal(
            new ZLinkRelocationPermitSnapshot(1, 0, 1, 0, 0, false),
            permits.Snapshot());

        capture.Dispose();
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Payload(80),
            out var payload));
        Assert.False(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Payload(21),
            out _));
        Assert.Equal(
            new ZLinkRelocationPermitSnapshot(1, 0, 0, 0, 80, false),
            permits.Snapshot());

        payload.Dispose();
        unit.Dispose();
        Assert.Equal(default, permits.Snapshot());
    }

    [Fact]
    public async Task ExactActorRelocationAdapterCapturesAndRestoresSameActorInstance()
    {
        var adapter = new RecordingActorRelocationAdapter();
        await using var services = new ServiceCollection()
            .AddSingleton(adapter)
            .BuildServiceProvider();
        var relocation = new ZLinkObjectRelocationRegistration(
            typeof(TestRelocatableActor),
            new ZLinkObjectPlacementOptions(),
            PolicyKind: 2,
            typeof(RecordingActorRelocationAdapter),
            new ZLinkActorRelocationAdapterInvoker<TestRelocatableActor>(
                typeof(RecordingActorRelocationAdapter)));
        var actor = new TestRelocatableActor("actor-1", null!);

        var captured = await ZLinkActorRelocationRegistry.CaptureAsync(
            services,
            relocation,
            actor,
            CancellationToken.None);
        await using var restoreScope = services.CreateAsyncScope();
        await ZLinkActorRelocationRegistry.RestoreAsync(
            restoreScope.ServiceProvider,
            relocation,
            actor,
            captured,
            CancellationToken.None);

        Assert.Equal(new byte[] { 1, 2, 3 }, captured);
        Assert.Equal(new byte[] { 1, 2, 3 }, adapter.RestoredPayload);
    }

    [Fact]
    public void ActorRelocationEnvelopeUsesExactPolicyMarkerAndMeasuredBytes()
    {
        var relocation = new ZLinkObjectRelocationRegistration(
            typeof(TestRelocatableActor),
            new ZLinkObjectPlacementOptions(),
            PolicyKind: 2,
            typeof(TestActorRelocationAdapter),
            new ZLinkActorRelocationAdapterInvoker<TestRelocatableActor>(
                typeof(TestActorRelocationAdapter)));
        var request = new ZLinkRemoteActorJoinRequest(
            "actor-1",
            "Game.Actor",
            "handoff-1",
            null,
            null,
            ZLinkRemoteActorJoinPackets.SnapshotRelocationContentType,
            "root-1",
            17,
            Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
            8,
            new byte[32],
            "application/json",
            new byte[] { 4, 5 },
            [],
            "source-entry",
            new byte[] { 6 },
            7,
            8);

        Assert.Equal(
            new byte[] { 1, 2, 3 },
            ZLinkActorRelocationRegistry.ValidateIncomingPayload(
                    relocation,
                    request.ActorType,
                    request.RelocationContentType,
                    new byte[] { 1, 2, 3 })
                .ToArray());
        Assert.Equal(
            64 * 1024 + 2 + 1 + "root-1".Length * 3 + 32,
            ZLinkRemoteActorJoinPackets.MeasureRelocationPayloadBytes(request));
        Assert.False(
            ZLinkActorHandoffRequestIdentity.Matches(
                request,
                request with { ActorGeneration = request.ActorGeneration + 1 }));

        var recreate = relocation with
        {
            PolicyKind = 1,
            AdapterType = null,
            AdapterInvoker = null
        };
        Assert.Throws<InvalidDataException>(
            () => ZLinkActorRelocationRegistry.ValidateIncomingPayload(
                recreate,
                request.ActorType,
                request.RelocationContentType,
                new byte[] { 1, 2, 3 }));
    }

    [Fact]
    public void RelocationPermitLeaseReleasesExactlyOnce()
    {
        var permits = new ZLinkRelocationPermitPool(new ZLinkLocationOptions());
        Assert.True(permits.TryAcquire(
            ZLinkRelocationPermitRequest.Outbound(1, capture: true),
            out var lease));

        var copiedLease = lease;
        lease.Dispose();
        copiedLease.Dispose();

        Assert.Equal(default, permits.Snapshot());
    }

    [Fact]
    public async Task Authority_relocation_uses_exact_owner_and_capacity_fence_with_opaque_payloads()
    {
        var store = new ZLinkInMemoryLocationStore();
        var source = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "source-owner",
                TimeSpan.FromMinutes(1)));
        var target = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "target-owner",
                TimeSpan.FromMinutes(1)));
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("source", source.Token),
            ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("target", target.Token),
            ZLinkLocationWriteIntent.NewClaim);
        var key = new ZLinkAuthorityKey("actor:mesh:actor-1");
        var creating = new byte[] { 0x11, 0x00, 0xff };
        var ready = new byte[] { 0x22, 0x00, 0xfe };
        var reservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.Actor,
                    key,
                    "Game.Actor",
                    "intent-1",
                    SHA256.HashData("intent-1"u8),
                    8,
                    new ZLinkMeshNodeDescriptorKey(
                        "mesh",
                        RoutingId.From("source")),
                    1,
                    source.Token,
                    creating,
                    ActorCapacity())));
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(reservation.Reservation, ready));
        Assert.Equal(ready, committed.Snapshot.Payload.ToArray());
        Assert.Equal(
            (0L, 1L),
            store.GetPlacementCapacityUsage(
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("source")),
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));

        var capacity = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await store.ReserveRelocationCapacityAsync(
                new ZLinkRelocationCapacityReservationRequest(
                    Guid.NewGuid(),
                    key,
                    committed.Snapshot.StoreVersion,
                    ZLinkPlacementObjectKind.Actor,
                    "Game.Actor",
                    new ZLinkMeshNodeDescriptorKey(
                        "mesh",
                        RoutingId.From("source")),
                    1,
                    source.Token,
                    new ZLinkMeshNodeDescriptorKey(
                        "mesh",
                        RoutingId.From("target")),
                    1,
                    target.Token,
                    ActorCapacity())));
        Assert.Equal(
            (1L, 0L),
            store.GetPlacementCapacityUsage(
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("target")),
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
        var preparedPayload = new byte[] { 0x33, 0x00, 0xfd };
        var prepared = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                key,
                committed.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    preparedPayload,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    capacity.Fence)));
        Assert.Equal(source.Token.OwnerId, prepared.Snapshot.OwnerId);
        Assert.Equal(preparedPayload, prepared.Snapshot.Payload.ToArray());
        var opaque = new byte[] { 0xde, 0xad, 0x00, 0xbe, 0xef };
        var moved = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                key,
                prepared.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    opaque,
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    target.Token,
                    capacity.Fence)));

        Assert.Equal(opaque, moved.Snapshot.Payload.ToArray());
        Assert.Equal(target.Token.OwnerId, moved.Snapshot.OwnerId);
        Assert.Equal(
            target.Token.LeaseGeneration,
            moved.Snapshot.OwnerLeaseGeneration);
        Assert.Equal(
            ZLinkRelocationCapacityAbortResult.AlreadyCommitted,
            await store.AbortRelocationCapacityAsync(capacity.Fence));
        Assert.Equal(
            (0L, 0L),
            store.GetPlacementCapacityUsage(
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("source")),
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
        Assert.Equal(
            (0L, 1L),
            store.GetPlacementCapacityUsage(
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("target")),
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
    }

    [Fact]
    public async Task Creation_abort_and_delete_release_exact_capacity_bucket()
    {
        var store = new ZLinkInMemoryLocationStore();
        var owner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "capacity-owner",
                TimeSpan.FromMinutes(1)));
        var descriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("capacity-node"));
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("capacity-node", owner.Token),
            ZLinkLocationWriteIntent.NewClaim);

        var abortedReservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:aborted",
                    descriptor,
                    owner.Token,
                    capacityDelta: 1)));
        Assert.Equal(
            (1L, 0L),
            store.GetPlacementCapacityUsage(
                descriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
        Assert.IsType<ZLinkObjectAbortResult.Aborted>(
            await store.AbortAsync(abortedReservation.Reservation));
        Assert.Equal(
            (0L, 0L),
            store.GetPlacementCapacityUsage(
                descriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));

        var committedReservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:deleted",
                    descriptor,
                    owner.Token,
                    capacityDelta: 1)));
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                committedReservation.Reservation,
                new byte[] { 0x44 }));
        Assert.Equal(
            (0L, 1L),
            store.GetPlacementCapacityUsage(
                descriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await store.CompareExchangeAuthorityAsync(
                new ZLinkAuthorityKey("actor:mesh:deleted"),
                committed.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));
        Assert.Equal(
            (0L, 0L),
            store.GetPlacementCapacityUsage(
                descriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
    }

    [Fact]
    public async Task Creation_admission_checks_stable_type_and_capacity_limits()
    {
        var store = new ZLinkInMemoryLocationStore();
        var owner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "bounded-owner",
                TimeSpan.FromMinutes(1)));
        var descriptorKey = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("bounded-node"));
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor(
                "bounded-node",
                owner.Token,
                activeLimit: 1,
                pendingLimit: 1),
            ZLinkLocationWriteIntent.NewClaim);

        Assert.IsType<ZLinkObjectReserveResult.Conflict>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:wrong-profile",
                    descriptorKey,
                    owner.Token,
                    stableType: "Other.Actor")));

        var first = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:first",
                    descriptorKey,
                    owner.Token)));
        var pendingDescriptor = Assert.Single(
            (await store.ListMeshNodesAsync("mesh", default)).Items,
            value => value.Rid == RoutingId.From("bounded-node"));
        Assert.Equal(0, pendingDescriptor.Capacity.Actors.Active);
        Assert.Equal(1, pendingDescriptor.Capacity.Actors.Reserved);
        Assert.IsType<ZLinkObjectReserveResult.PlacementCapacityExhausted>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:pending-overflow",
                    descriptorKey,
                    owner.Token)));
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(first.Reservation, new byte[] { 0x01 }));
        var activeDescriptor = Assert.Single(
            (await store.ListMeshNodesAsync("mesh", default)).Items,
            value => value.Rid == RoutingId.From("bounded-node"));
        Assert.Equal(1, activeDescriptor.Capacity.Actors.Active);
        Assert.Equal(0, activeDescriptor.Capacity.Actors.Reserved);
        Assert.IsType<ZLinkObjectReserveResult.PlacementCapacityExhausted>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:active-overflow",
                    descriptorKey,
                    owner.Token)));
    }

    [Fact]
    public async Task Descriptor_renew_requires_exact_fence_revision_and_immutable_fields()
    {
        var store = new ZLinkInMemoryLocationStore();
        var owner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "descriptor-owner",
                TimeSpan.FromMinutes(1)));
        var descriptor = AuthorityDescriptor(
            "descriptor-node",
            owner.Token);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);

        Assert.Equal(
            ZLinkLocationWriteStatus.IgnoredStale,
            (await store.UpdateMeshNodeAsync(
                descriptor with { PlacementWeight = 75 },
                ZLinkLocationWriteIntent.Renew)).Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.IgnoredStale,
            (await store.UpdateMeshNodeAsync(
                descriptor with
                {
                    DescriptorRevision = 2,
                    ApplicationVersion = 2
                },
                ZLinkLocationWriteIntent.Renew)).Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(
                descriptor with
                {
                    DescriptorRevision = 2,
                    PlacementWeight = 75
                },
                ZLinkLocationWriteIntent.Renew)).Status);
        var current = Assert.Single(
            (await store.ListMeshNodesAsync("mesh", default)).Items,
            value => value.Rid == RoutingId.From("descriptor-node"));
        Assert.Equal(2UL, current.DescriptorRevision);
        Assert.Equal(75, current.PlacementWeight);
    }

    [Fact]
    public async Task Relocation_admission_checks_target_capability_and_capacity()
    {
        var store = new ZLinkInMemoryLocationStore();
        var source = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "relocation-source-owner",
                TimeSpan.FromMinutes(1)));
        var target = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "relocation-target-owner",
                TimeSpan.FromMinutes(1)));
        var sourceDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("relocation-source"));
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("relocation-target"));
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("relocation-source", source.Token),
            ZLinkLocationWriteIntent.NewClaim);
        var unavailableTarget = AuthorityDescriptor(
                "relocation-target",
                target.Token,
                activeLimit: 1,
                pendingLimit: 1) with
        {
            PlacementWeight = 0
        };
        await store.UpdateMeshNodeAsync(
            unavailableTarget,
            ZLinkLocationWriteIntent.NewClaim);

        var sourceReservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:relocate",
                    sourceDescriptor,
                    source.Token)));
        var sourceReady = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                sourceReservation.Reservation,
                new byte[] { 0x01 }));
        var relocation = RelocationReservation(
            sourceReady.Snapshot,
            sourceDescriptor,
            source.Token,
            targetDescriptor,
            target.Token);
        Assert.IsType<ZLinkRelocationCapacityReserveResult.TargetUnavailable>(
            await store.ReserveRelocationCapacityAsync(relocation));

        await store.UpdateMeshNodeAsync(
            unavailableTarget with
            {
                DescriptorRevision = 2,
                PlacementWeight = 100
            },
            ZLinkLocationWriteIntent.Renew);
        var occupyingReservation =
            Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await store.ReserveAsync(
                    ObjectReservation(
                        "actor:mesh:target-occupied",
                        targetDescriptor,
                        target.Token)));
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                occupyingReservation.Reservation,
                new byte[] { 0x02 }));

        Assert.IsType<
            ZLinkRelocationCapacityReserveResult.PlacementCapacityExhausted>(
            await store.ReserveRelocationCapacityAsync(
                relocation with { ReservationId = Guid.NewGuid() }));
    }

    [Fact]
    public async Task AcquiredRelocationReservationSurvivesTargetWeightZero()
    {
        var store = new ZLinkInMemoryLocationStore();
        var source = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "weight-source-owner",
                TimeSpan.FromMinutes(1)));
        var target = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "weight-target-owner",
                TimeSpan.FromMinutes(1)));
        var sourceDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("weight-source"));
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("weight-target"));
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("weight-source", source.Token),
            ZLinkLocationWriteIntent.NewClaim);
        var targetRow = AuthorityDescriptor("weight-target", target.Token);
        await store.UpdateMeshNodeAsync(
            targetRow,
            ZLinkLocationWriteIntent.NewClaim);

        var firstCreation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:relocate",
                    sourceDescriptor,
                    source.Token)));
        var firstReady = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                firstCreation.Reservation,
                new byte[] { 0x01 }));
        var acquired = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await store.ReserveRelocationCapacityAsync(
                RelocationReservation(
                    firstReady.Snapshot,
                    sourceDescriptor,
                    source.Token,
                    targetDescriptor,
                    target.Token)));

        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await store.UpdateMeshNodeAsync(
                targetRow with
                {
                    DescriptorRevision = targetRow.DescriptorRevision + 1,
                    PlacementWeight = 0
                },
                ZLinkLocationWriteIntent.Renew)).Status);

        var secondCreation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:weight-new",
                    sourceDescriptor,
                    source.Token)));
        var secondReady = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                secondCreation.Reservation,
                new byte[] { 0x02 }));
        Assert.IsType<ZLinkRelocationCapacityReserveResult.TargetUnavailable>(
            await store.ReserveRelocationCapacityAsync(
                RelocationReservation(
                    secondReady.Snapshot,
                    sourceDescriptor,
                    source.Token,
                    targetDescriptor,
                    target.Token) with
                {
                    Key = secondCreation.Reservation.Key
                }));

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                firstCreation.Reservation.Key,
                firstReady.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x03 },
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    target.Token,
                    acquired.Fence)));
        Assert.Equal(
            ZLinkRelocationCapacityAbortResult.AlreadyCommitted,
            await store.AbortRelocationCapacityAsync(acquired.Fence));
        Assert.Equal(
            (0L, 1L),
            store.GetPlacementCapacityUsage(
                targetDescriptor,
                targetRow.LifecycleGeneration,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
    }

    [Fact]
    public async Task Relocation_capacity_reservation_generation_is_used_without_source_plus_one_assumption()
    {
        var store = new ZLinkInMemoryLocationStore();
        var source = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "generation-source",
                TimeSpan.FromMinutes(1)));
        var target = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "generation-target",
                TimeSpan.FromMinutes(1)));
        var sourceDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("generation-source"));
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("generation-target"));
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("generation-source", source.Token),
            ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("generation-target", target.Token),
            ZLinkLocationWriteIntent.NewClaim);
        var creation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    "actor:mesh:relocate",
                    sourceDescriptor,
                    source.Token)));
        var ready = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                creation.Reservation,
                new byte[] { 0x01 }));

        var first = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await store.ReserveRelocationCapacityAsync(
                RelocationReservation(
                    ready.Snapshot,
                    sourceDescriptor,
                    source.Token,
                    targetDescriptor,
                    target.Token)));
        Assert.Equal(
            ZLinkRelocationCapacityAbortResult.Aborted,
            await store.AbortRelocationCapacityAsync(first.Fence));

        var second = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await store.ReserveRelocationCapacityAsync(
                RelocationReservation(
                    ready.Snapshot,
                    sourceDescriptor,
                    source.Token,
                    targetDescriptor,
                    target.Token) with
                {
                    ReservationId = Guid.NewGuid()
                }));
        Assert.Equal(
            ready.Snapshot.AuthorityOwnerGeneration + 2,
            second.TargetAuthorityOwnerGeneration);

        var committed = Assert.IsType<
            ZLinkAuthorityCompareExchangeResult.Stored>(
            await store.CompareExchangeAuthorityAsync(
                creation.Reservation.Key,
                ready.Snapshot.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x02 },
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    target.Token,
                    second.Fence)));
        Assert.Equal(
            second.TargetAuthorityOwnerGeneration,
            committed.Snapshot.AuthorityOwnerGeneration);
    }

    [Fact]
    public async Task Aggregate_prepare_is_idempotent_only_for_exact_canonical_request()
    {
        var store = new ZLinkInMemoryLocationStore();
        var source = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "aggregate-source",
                TimeSpan.FromMinutes(1)));
        var target = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "aggregate-target",
                TimeSpan.FromMinutes(1)));
        var sourceDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("aggregate-source"));
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("aggregate-target"));
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("aggregate-source", source.Token),
            ZLinkLocationWriteIntent.NewClaim);
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("aggregate-target", target.Token),
            ZLinkLocationWriteIntent.NewClaim);

        var key = new ZLinkAuthorityKey("actor:mesh:aggregate-1");
        var creation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    key.Value,
                    sourceDescriptor,
                    source.Token,
                    capacityDelta: 1)));
        var ready = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                creation.Reservation,
                new byte[] { 0x01 }));
        var participant = new ZLinkAggregateParticipant(
            key,
            ready.Snapshot.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            new byte[] { 0x02 },
            new byte[] { 0x03 });
        var request = new ZLinkAggregatePrepareRequest(
            Guid.NewGuid(),
            1,
            [participant],
            Enumerable.Repeat((byte)0x5a, 32).ToArray(),
            targetDescriptor,
            1,
            ActorCapacity(),
            target.Token);

        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await store.PrepareAggregateAsync(request));
        Assert.True(
            prepared.TargetAuthorityOwnerGenerations.TryGetValue(
                key,
                out var reservedTargetGeneration));
        Assert.True(
            reservedTargetGeneration
            > ready.Snapshot.AuthorityOwnerGeneration);
        Assert.IsType<ZLinkAggregatePrepareResult.AlreadyPrepared>(
            await store.PrepareAggregateAsync(
                request with
                {
                    Participants =
                    [
                        participant with
                        {
                            AuthorityPayload =
                                participant.AuthorityPayload.ToArray(),
                            MembershipMutation =
                                participant.MembershipMutation.ToArray()
                        }
                    ],
                    InventoryDigest = request.InventoryDigest.ToArray()
                }));
        Assert.IsType<ZLinkAggregatePrepareResult.Conflict>(
            await store.PrepareAggregateAsync(
                request with
                {
                    Participants =
                    [
                        participant with
                        {
                            AuthorityPayload = new byte[] { 0xff }
                        }
                    ]
                }));

        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await store.CommitAggregateAsync(prepared.Fence));
        var recovered =
            Assert.IsType<ZLinkAggregatePrepareResult.AlreadyPrepared>(
                await store.PrepareAggregateAsync(request));
        Assert.Equal(
            reservedTargetGeneration,
            recovered.TargetAuthorityOwnerGenerations[key]);
        var committedAuthority =
            Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await store.ReadAuthorityAsync(key));
        Assert.Equal(
            reservedTargetGeneration,
            committedAuthority.Snapshot.AuthorityOwnerGeneration);
        Assert.Equal(
            (0L, 0L),
            store.GetPlacementCapacityUsage(
                sourceDescriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
        Assert.Equal(
            (0L, 1L),
            store.GetPlacementCapacityUsage(
                targetDescriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
    }

    [Fact]
    public async Task Aggregate_preserve_normalization_accepts_only_zero_capacity_delta()
    {
        var store = new ZLinkInMemoryLocationStore();
        var owner = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await store.ClaimOwnerLeaseAsync(
                "aggregate-owner",
                TimeSpan.FromMinutes(1)));
        var descriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("aggregate-owner"));
        await store.UpdateMeshNodeAsync(
            AuthorityDescriptor("aggregate-owner", owner.Token),
            ZLinkLocationWriteIntent.NewClaim);

        var key = new ZLinkAuthorityKey("actor:mesh:aggregate-preserve");
        var reservation = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await store.ReserveAsync(
                ObjectReservation(
                    key.Value,
                    descriptor,
                    owner.Token,
                    capacityDelta: 1)));
        var ready = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await store.CommitAsync(
                reservation.Reservation,
                new byte[] { 0x01 }));
        var request = new ZLinkAggregatePrepareRequest(
            Guid.NewGuid(),
            1,
            [
                new ZLinkAggregateParticipant(
                    key,
                    ready.Snapshot.StoreVersion,
                    ZLinkAuthorityGenerationTransition.Preserve,
                    new byte[] { 0x02 },
                    ReadOnlyMemory<byte>.Empty)
            ],
            Enumerable.Repeat((byte)0x5a, 32).ToArray(),
            descriptor,
            1,
            new ZLinkCapacityVector(0, 0, null),
            owner.Token);

        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() =>
            store.PrepareAggregateAsync(
                    request with
                    {
                        Participants =
                        [
                            request.Participants[0] with
                            {
                                MembershipMutation = new byte[] { 0x01 }
                            }
                        ]
                    })
                .AsTask());

        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await store.PrepareAggregateAsync(request));
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await store.CommitAggregateAsync(prepared.Fence));
        var normalized = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await store.ReadAuthorityAsync(key));
        Assert.Equal(new byte[] { 0x02 }, normalized.Snapshot.Payload.ToArray());
        Assert.Equal(owner.Token.OwnerId, normalized.Snapshot.OwnerId);
        Assert.Equal(
            (0L, 1L),
            store.GetPlacementCapacityUsage(
                descriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
    }

    [Fact]
    public void PublicRelocationContractsMatchTargetShape()
    {
        Assert.Contains(
            typeof(IZLinkRelocationRepository).GetMethods(),
            static method => method.Name == "PutRelocationAsync");
        Assert.Contains(
            typeof(IZLinkLocationRepository).GetMethods(),
            static method => method.Name == "PrepareAggregateAsync");
        Assert.Contains(
            typeof(IZLinkFrameworkOptions).GetMethods(),
            static method => method.Name == "AddRelocationStore");
        Assert.Equal(
            typeof(ValueTask<byte[]>),
            typeof(IZLinkActorRelocationAdapter<>)
                .GetMethod("CaptureAsync")!
                .ReturnType);
        Assert.Equal(
            typeof(ValueTask),
            typeof(IZLinkSpotRelocationAdapter<>)
                .GetMethod("RestoreAsync")!
                .ReturnType);
        Assert.Contains(
            typeof(IZLinkMeshObjectServerBuilder).GetMethods(),
            static method => method.Name == "AddSpotFactory"
                             && method.GetParameters().Length == 2);
        Assert.True(
            typeof(IZLinkActorFactory).IsAssignableFrom(
                typeof(IZLinkActorFactory<TestRelocatableActor>)));
    }

    [Fact]
    public void RelocationStoreRegistrationIsSeparateAndSingle()
    {
        var registration = new ZLinkFrameworkRegistration();
        var options = new ZLinkFrameworkOptionsBuilder(registration);
        var relocation = new RecordingRelocationStore();

        options.AddRelocationStore(relocation);

        Assert.Same(relocation, registration.Locations.ResolveRelocationStore());
        Assert.Null(registration.Locations.StoreInstance);
        Assert.Throws<ZLinkConfigurationException>(
            () => options.AddRelocationStore(new RecordingRelocationStore()));
    }

    [Fact]
    public void InstanceSpotFactoryRequiresRelocationStoreBeforeRuntimeStartup()
    {
        var registration = NewObjectServerRegistration(out var server);
        var options = new ZLinkFrameworkOptionsBuilder(registration);
        options.UseTestLocationStore();
        server.AddInstanceSpotFactory<TestInstanceSpot>(
            "Game.Session", factory => factory.DisableRelocation());

        var failure = Assert.Throws<ZLinkConfigurationException>(
            () => ZLinkFrameworkRegistrationValidator.Validate(registration));

        Assert.Contains("requires exactly one Relocation Store", failure.Message);
    }

    [Fact]
    public void RecreateAndSnapshotPoliciesRequireRelocationStoreButDisabledDoesNot()
    {
        var disabled = NewObjectServerRegistration(out var disabledServer);
        var disabledOptions = new ZLinkFrameworkOptionsBuilder(disabled);
        disabledOptions.UseTestLocationStore();
        disabledServer.AddSpotFactory<TestRelocatableSpot>(
            "Game.DisabledRoom", factory => factory.DisableRelocation());
        ZLinkFrameworkRegistrationValidator.Validate(disabled);

        foreach (var policyKind in new byte[] { 1, 2 })
        {
            var registration = NewObjectServerRegistration(out var server);
            var options = new ZLinkFrameworkOptionsBuilder(registration);
            options.UseTestLocationStore();
            server.AddActorFactory<
                TestRelocatableActor,
                TestRelocatableActorFactory>(
                $"Game.Actor.{policyKind}",
                factory =>
                {
                    if (policyKind == 1)
                        factory.RecreateOnRelocation();
                    else
                        factory.PreserveStateWith<TestActorRelocationAdapter>();
                });

            var failure = Assert.Throws<ZLinkConfigurationException>(
                () => ZLinkFrameworkRegistrationValidator.Validate(registration));
            Assert.Contains("requires exactly one Relocation Store", failure.Message);
        }
    }

    [Fact]
    public void RegisteredRelocationStoreSatisfiesDurableObjectServerRequirement()
    {
        var registration = NewObjectServerRegistration(out var server);
        var options = new ZLinkFrameworkOptionsBuilder(registration);
        options.UseTestLocationStore();
        options.AddRelocationStore(new RecordingRelocationStore());
        server.AddInstanceSpotFactory<TestInstanceSpot>(
            "Game.Session", factory => factory.DisableRelocation());

        ZLinkFrameworkRegistrationValidator.Validate(registration);
    }

    private static ZLinkFrameworkRegistration NewObjectServerRegistration(
        out IZLinkMeshObjectServerBuilder server)
    {
        var registration = new ZLinkFrameworkRegistration();
        var node = new ZLinkFrameworkOptionsBuilder(registration)
            .AddRouteMesh("objects")
            .Listen("inproc://objects");
        server = node.Objects().Server();
        return registration;
    }

    [Fact]
    public void ObjectServerRegistrationKeepsExecutionModePolicyAndAdapterTogether()
    {
        var registration = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "objects"
        };
        IZLinkMeshObjectServerBuilder builder = new ZLinkMeshNodeBuilder(registration)
            .Objects()
            .Server();

        builder.AddSpotFactory<TestRelocatableSpot>(
            "room",
            factory => factory
                .StableTypeLimit(100)
                .ExecutionMode(ZLinkUserSpotExecutionMode.SpotWide)
                .PreserveStateWith<TestSpotRelocationAdapter>());

        var relocation = registration.SpotRelocations["room"];
        Assert.Equal(typeof(TestRelocatableSpot), relocation.InstanceType);
        Assert.Equal((byte)2, relocation.PolicyKind);
        Assert.Equal(typeof(TestSpotRelocationAdapter), relocation.AdapterType);
        Assert.Equal(100, relocation.Placement.MaxActiveObjects);
        var factoryOptions = registration.UserSpotFactoryOptions[typeof(TestRelocatableSpot)];
        Assert.Equal(100, factoryOptions.StableTypeLimit);
        Assert.Equal(ZLinkUserSpotExecutionMode.SpotWide, factoryOptions.ExecutionMode);
    }

    [Fact]
    public void DescriptorConfigurationKeepsHostAndNodePlacementFields()
    {
        var registration = new ZLinkFrameworkRegistration();
        var options = new ZLinkFrameworkOptionsBuilder(registration)
        {
            ApplicationVersion = 42,
            MaintenanceWave = "wave-blue"
        };
        var node = options.AddRouteMesh("objects");
        node.SetPlacementWeight(75)
            .SetActorLimit(500)
            .SetSpotLimit(500)
            .SetActivationConcurrency(25);
        node.Objects().Server().AddActorFactory<
            TestRelocatableActor,
            TestRelocatableActorFactory>(
            "Game.Actor", factory => factory.RecreateOnRelocation());

        Assert.Equal(42, registration.ApplicationVersion);
        Assert.Equal("wave-blue", registration.MaintenanceWave);
        var configured = registration.SpotNodes["objects"];
        Assert.Equal(ZLinkMeshNodeObjectRole.Server, configured.ObjectRole);
        Assert.Equal(75, configured.PlacementWeight);
        Assert.Equal(500, configured.ActorLimit);
        Assert.Equal(500, configured.SpotLimit);
        Assert.Equal(25, configured.ActivationConcurrencyLimit);
        Assert.Throws<ZLinkConfigurationException>(
            () => node.Objects().Client());
    }

    [Fact]
    public void ExactCapacityLimitsAllowUnlimitedPopulationAndRejectInvalidValues()
    {
        var registration = new ZLinkFrameworkRegistration();
        var options = new ZLinkFrameworkOptionsBuilder(registration);
        var node = options.AddRouteMesh("objects");

        node.SetActorLimit(0);
        node.SetSpotLimit(0);
        Assert.Throws<ZLinkConfigurationException>(
            () => node.SetActorLimit(-1));
        Assert.Throws<ZLinkConfigurationException>(
            () => node.SetSpotLimit(-1));
        Assert.Throws<ZLinkConfigurationException>(
            () => node.SetActivationConcurrency(0));
        Assert.Throws<ZLinkConfigurationException>(
            () => node.SetActivationConcurrency(-1));

        var configured = registration.SpotNodes["objects"];
        Assert.Equal(0, configured.ActorLimit);
        Assert.Equal(0, configured.SpotLimit);
    }

    [Fact]
    public void AggregateEnvelopePreservesAcceptedQueueAndLogicalTimers()
    {
        var envelope = CreateEnvelope();

        var restored = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(envelope));

        Assert.Equal(envelope.AggregateId, restored.AggregateId);
        Assert.Equal(envelope.AggregateGeneration, restored.AggregateGeneration);
        Assert.Equal(2, restored.Participants.Count);
        var spot = restored.Participants[0];
        Assert.Equal(ZLinkPlacementObjectKind.UserSpot, spot.ObjectKind);
        Assert.Equal(
            new ulong[] { 41, 42 },
            spot.AcceptedJobs.Select(static job => job.AcceptedSequence));
        Assert.Equal(
            new byte[] { 4, 1 },
            spot.AcceptedJobs[0].Payload.ToArray());
        Assert.Equal("heartbeat", spot.LogicalTimers[0].TimerId);
        Assert.Equal(5_000, spot.LogicalTimers[0].PeriodMilliseconds);
        Assert.Equal(
            new byte[] { 7, 7 },
            restored.Participants[1].ApplicationState.ToArray());
    }

    [Fact]
    public void AggregateEnvelopeRoundTripsTenThousandParticipants()
    {
        var envelope = CreateLargeEnvelope(10_000);

        var encoded = ZLinkRelocationEnvelopeCodec.Encode(envelope);
        var restored = ZLinkRelocationEnvelopeCodec.Decode(encoded);

        Assert.Equal(10_000, restored.Participants.Count);
        Assert.Equal(
            "actor:bulk:0",
            restored.Participants[0].AuthorityKey.Value);
        Assert.Equal(
            "actor:bulk:9999",
            restored.Participants[^1].AuthorityKey.Value);
        Assert.Equal(
            envelope.InventoryDigest,
            restored.InventoryDigest);
    }

    [Fact]
    public void AggregateEnvelopeRejectsParticipantCountBeyondRemainingBytes()
    {
        var encoded = ZLinkRelocationEnvelopeCodec.Encode(
            CreateLargeEnvelope(1));
        const int participantCountOffset =
            sizeof(uint) + sizeof(ushort) + 16 + sizeof(ulong)
            + sizeof(int) + 32;
        BinaryPrimitives.WriteInt32LittleEndian(
            encoded.AsSpan(participantCountOffset, sizeof(int)),
            int.MaxValue);

        Assert.Throws<InvalidDataException>(
            () => ZLinkRelocationEnvelopeCodec.Decode(encoded));
    }

    [Fact]
    public void SpotAcceptedJournalPreservesRouteIdentityMetadataAndParts()
    {
        using var received = new ZLinkBackendRouteReceived(
            [
                new Message((ReadOnlySpan<byte>)new byte[] { 1, 2 }),
                new Message((ReadOnlySpan<byte>)new byte[] { 3 })
            ],
            RoutingId.From("source-node"),
            "spot-7",
            44,
            reply: null,
            metadata: new ZLinkMessageMetadata(
                new Dictionary<string, string>(StringComparer.Ordinal)
                {
                    ["trace"] = "abc"
                }),
            operationId: new MeshOperationId(11, 44),
            targetNodeGeneration: 12,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 14,
            messageFollowHopCount: 2,
            sourceNodeGeneration: 15,
            requestSource: new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner", 16, RoutingId.From("source-node"), 15));

        var restored = ZLinkSpotAcceptedJournal.Decode(
            ZLinkSpotAcceptedJournal.Encode(received));

        Assert.Equal(RoutingId.From("source-node"), restored.SourceNodeRid);
        Assert.Equal<ulong>(15, restored.SourceNodeGeneration);
        Assert.Equal(
            new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner", 16, RoutingId.From("source-node"), 15),
            restored.RequestSource);
        Assert.Equal("spot-7", restored.SpotId);
        Assert.Equal<ulong?>(44, restored.RequestSequence);
        Assert.Equal<ulong>(0, restored.ReplyRouteId);
        Assert.Equal(new MeshOperationId(11, 44), restored.OperationId);
        Assert.Equal<ulong>(12, restored.TargetNodeGeneration);
        Assert.Equal<ulong>(13, restored.AuthorityOwnerGeneration);
        Assert.Equal<ulong>(14, restored.OwnerLeaseGeneration);
        Assert.Equal<byte>(2, restored.MessageFollowHopCount);
        Assert.Equal("abc", restored.Metadata.Find("trace"));
        Assert.Equal(new byte[] { 1, 2 }, restored.Parts[0].ToArray());
        Assert.Equal(new byte[] { 3 }, restored.Parts[1].ToArray());
    }

    [Fact]
    public void SpotAcceptedJournalTreatsNodeOriginAsAbsentSourceSpot()
    {
        using var received = new ZLinkBackendRouteReceived(
            [Message.From(new byte[] { 1 })],
            RoutingId.From("source-node"),
            string.Empty,
            requestSeq: null,
            reply: null,
            operationId: new MeshOperationId(11, 44),
            targetNodeGeneration: 12,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 14,
            sourceNodeGeneration: 15,
            requestSource: new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner",
                16,
                RoutingId.From("source-node"),
                15));

        var restored = ZLinkSpotAcceptedJournal.Decode(
            ZLinkSpotAcceptedJournal.Encode(received));

        Assert.Null(restored.SpotId);
        Assert.Equal(new byte[] { 1 }, restored.Parts.Single().ToArray());
    }

    [Fact]
    public void SpotAcceptedJournalRejectsReceiverLocalReplyCorrelation()
    {
        var rejectedPart = Message.From(new byte[] { 1 });
        var rejected = new ZLinkBackendRouteReceived(
            [rejectedPart],
            RoutingId.From("source-node"),
            "spot-7",
            44,
            static (_, _) => SubmitResult.Ok,
            operationId: new MeshOperationId(11, 44),
            targetNodeGeneration: 12,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 14,
            sourceNodeGeneration: 15,
            requestSource: new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner", 16, RoutingId.From("source-node"), 15));

        Assert.Throws<InvalidOperationException>(() =>
            ZLinkSpotAcceptedJournal.CaptureOrDispose(rejected, 45));
        Assert.Throws<ObjectDisposedException>(() =>
            rejectedPart.AsReadOnlySpan());

        using var received = new ZLinkBackendRouteReceived(
            [Message.From(new byte[] { 2 })],
            RoutingId.From("source-node"),
            "spot-7",
            44,
            static (_, _) => SubmitResult.Ok,
            operationId: new MeshOperationId(11, 44),
            targetNodeGeneration: 12,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 14,
            sourceNodeGeneration: 15,
            requestSource: new ZLinkServiceWireCodec.RequestSourceFence(
                "source-owner", 16, RoutingId.From("source-node"), 15));
        var restored = ZLinkSpotAcceptedJournal.Decode(
            ZLinkSpotAcceptedJournal.CaptureOrDispose(received, 44));
        Assert.Equal<ulong>(44, restored.ReplyRouteId);
        Assert.Equal(new MeshOperationId(11, 44), restored.OperationId);
    }

    [Fact]
    public void SpotAcceptedJournalRejectsMissingIngressSourceFence()
    {
        using var received = new ZLinkBackendRouteReceived(
            [Message.From(new byte[] { 2 })],
            RoutingId.From("source-node"),
            "spot-7",
            44,
            static (_, _) => SubmitResult.Ok,
            operationId: new MeshOperationId(11, 44),
            targetNodeGeneration: 12,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 14,
            sourceNodeGeneration: 15);

        Assert.Throws<InvalidOperationException>(() =>
            ZLinkSpotAcceptedJournal.Encode(received, 44));
    }

    [Fact]
    public void CanonicalAcceptedJobsPreserveIngressFenceWhenDescriptorChanges()
    {
        var ingress = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner", 16, RoutingId.From("source-node"), 15);
        var journal = CaptureAcceptedSpotJournal(ingress);

        var changedDescriptor = ingress with
        {
            OwnerId = "replacement-owner",
            LeaseGeneration = 17
        };
        var job = Assert.Single(
            ZLinkSpotRetireScheduler.ResolveFrozenAcceptedJobs(
                [new ZLinkAcceptedWorkRecord(1, journal)]));

        Assert.NotEqual(changedDescriptor.OwnerId, job.RequestSource!.OwnerId);
        Assert.Equal(ingress.OwnerId, job.RequestSource.OwnerId);
        Assert.Equal(ingress.LeaseGeneration,
            job.RequestSource.OwnerLeaseGeneration);
    }

    [Fact]
    public void CanonicalAcceptedJobsDoNotRequireDescriptorAfterIngress()
    {
        var ingress = new ZLinkServiceWireCodec.RequestSourceFence(
            "source-owner", 16, RoutingId.From("source-node"), 15);
        var journal = CaptureAcceptedSpotJournal(ingress);
        ZLinkServiceWireCodec.RequestSourceFence? currentDescriptor = null;

        var job = Assert.Single(
            ZLinkSpotRetireScheduler.ResolveFrozenAcceptedJobs(
                [new ZLinkAcceptedWorkRecord(1, journal)]));

        Assert.Null(currentDescriptor);
        Assert.Equal(ingress.OwnerId, job.RequestSource!.OwnerId);
        Assert.Equal(ingress.NodeGeneration,
            job.RequestSource.NodeGeneration);
    }

    private static byte[] CaptureAcceptedSpotJournal(
        ZLinkServiceWireCodec.RequestSourceFence requestSource)
    {
        using var received = new ZLinkBackendRouteReceived(
            [Message.From(new byte[] { 2 })],
            requestSource.NodeRid,
            "spot-7",
            44,
            static (_, _) => SubmitResult.Ok,
            operationId: new MeshOperationId(11, 44),
            targetNodeGeneration: 12,
            authorityOwnerGeneration: 13,
            ownerLeaseGeneration: 14,
            sourceNodeGeneration: requestSource.NodeGeneration,
            requestSource: requestSource);
        return ZLinkSpotAcceptedJournal.Encode(received, 44);
    }

    [Fact]
    public void RelocationReplyRouteRetentionCoversLateCompletionRecovery()
    {
        Assert.Equal(
            TimeSpan.FromHours(24),
            ZLinkRelocationReplyLifetime.TerminalRetention);
    }

    [Fact]
    public async Task ImmutableRootIsVerifiedBeforeAuthorityCasAndRecoverableAfterPublish()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            authority,
            relocation);
        var request = CreateRequest(CreateEnvelope());

        var published = await coordinator.PublishAsync(request);
        var recovered = await coordinator.RecoverAsync(request.AuthorityKey);

        Assert.NotNull(recovered);
        var orderedEvents = relocation.Events.Concat(authority.Events)
            .OrderBy(static item => item.Sequence)
            .ToArray();
        var casSequence = Assert.Single(
            orderedEvents.Where(static item => item.Name == "cas")).Sequence;
        Assert.All(
            orderedEvents.Where(static item => item.Name == "put"),
            item => Assert.True(item.Sequence < casSequence));
        Assert.Contains(
            orderedEvents,
            item => item.Name == "get" && item.Sequence < casSequence);
        Assert.Equal("target-owner", published.Authority.OwnerId);
        Assert.Equal(9, published.Authority.OwnerLeaseGeneration);
        Assert.Equal(1UL, published.Authority.ObjectGeneration);
        Assert.Equal(1UL, published.Authority.AuthorityOwnerGeneration);
        Assert.Equal(
            request.Envelope.InventoryDigest.ToArray(),
            recovered!.Envelope.InventoryDigest.ToArray());
    }

    [Fact]
    public async Task CasConflictDeletesUnpublishedRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore { Conflict = true };
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            authority,
            relocation);

        await Assert.ThrowsAsync<ZLinkRelocationPublicationConflictException>(
            async () => await coordinator.PublishAsync(CreateRequest(CreateEnvelope())));

        Assert.DoesNotContain(
            relocation.Payloads.Values,
            static payload => payload.Take(4).SequenceEqual(
                "ZLTM"u8.ToArray()));
        Assert.Contains(
            relocation.Events,
            static item => item.Name == "delete");
    }

    [Fact]
    public async Task ExceptionAfterCommittedCasReconcilesWithoutDeletingPublishedRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore { ThrowAfterCommit = true };
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            authority,
            relocation);

        var published = await coordinator.PublishAsync(
            CreateRequest(CreateEnvelope()));

        Assert.NotNull(published.Authority);
        Assert.NotEmpty(relocation.Payloads);
        Assert.DoesNotContain(
            relocation.Events,
            static item => item.Name == "delete");
    }

    [Fact]
    public async Task MissingPublishedRootIsNonRetriableDataLoss()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            authority,
            relocation);
        var request = CreateRequest(CreateEnvelope());
        var published = await coordinator.PublishAsync(request);
        relocation.Payloads.Remove(published.Relocation.Reference);

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            async () => await coordinator.RecoverAsync(request.AuthorityKey));
    }

    [Fact]
    public async Task AggregateRelocationPublishesWholeSpotParticipantsWithOneCommit()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateEnvelope();
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            envelope.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        $"v-{participant.AuthorityKey.Value}",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        new byte[] { 6 },
                        new byte[] { 7 }))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    1)),
            new ZLinkLocationOwnerToken("aggregate-target", 17));

        var published = await coordinator.PublishAsync(request);

        Assert.Equal(envelope.AggregateId, published.Fence.AggregateId);
        Assert.Equal(2, authority.PublishedCount);
        var ordered = relocation.Events.Concat(authority.Events)
            .OrderBy(static item => item.Sequence)
            .ToArray();
        var prepareSequence = ordered.Single(
            static item => item.Name == "prepare").Sequence;
        Assert.All(
            ordered.Where(static item => item.Name == "put"),
            item => Assert.True(item.Sequence < prepareSequence));
        Assert.True(
            ordered.Single(static item => item.Name == "commit").Sequence
            > prepareSequence);
        Assert.Equal(
            ZLinkAggregateInventoryDigest.Compute(request.Participants),
            published.Envelope.InventoryDigest.ToArray());
    }

    [Fact]
    public async Task AggregateRelocationCoordinatorPublishesTenThousandParticipants()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateLargeEnvelope(10_000);
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            envelope.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        $"v-{participant.AuthorityKey.Value}",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        ReadOnlyMemory<byte>.Empty,
                        ReadOnlyMemory<byte>.Empty))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(10_000, 0, null),
            new ZLinkLocationOwnerToken("aggregate-target", 17));

        var published = await coordinator.PublishAsync(request);

        Assert.Equal(10_000, authority.PublishedCount);
        Assert.Equal(10_000, published.Envelope.Participants.Count);
        Assert.Equal(
            ZLinkAggregateInventoryDigest.Compute(request.Participants),
            published.Envelope.InventoryDigest.ToArray());
    }

    [Fact]
    public async Task AggregatePublicationProbeIsBoundedAndParallelForTenThousandParticipants()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult =
                new ZLinkAggregatePrepareResult.Conflict(),
            ReadDelay = TimeSpan.FromMilliseconds(1)
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateLargeEnvelope(10_000);
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            envelope.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        $"v-{participant.AuthorityKey.Value}",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        ReadOnlyMemory<byte>.Empty,
                        ReadOnlyMemory<byte>.Empty))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(10_000, 0, null),
            new ZLinkLocationOwnerToken("aggregate-target", 17));

        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await coordinator.PrepareAsync(request));

        Assert.InRange(authority.MaximumConcurrentReads, 2, 64);
        Assert.Contains(
            authority.Events,
            static item => item.Name == "abort");
        Assert.Contains(
            relocation.Events,
            static item => item.Name == "delete");
    }

    [Fact]
    public async Task AggregatePrepareResponseLossReconstructsExactOwnerGenerations()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            ThrowAfterPublishingAggregatePrepare = true
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var request = CreateAggregateRelocationRequest(CreateEnvelope());

        var prepared = await coordinator.PrepareAsync(request);

        Assert.Equal(request.Participants.Count,
            prepared.TargetAuthorityOwnerGenerations.Count);
        for (var index = 0; index < request.Participants.Count; index++)
        {
            Assert.Equal(
                checked((ulong)(100 + index)),
                prepared.TargetAuthorityOwnerGeneration(
                    request.Participants[index].Envelope.AuthorityKey));
        }
    }

    [Fact]
    public async Task ConcurrentCommitDuringPrepareProbeReturnsExactOwnerGenerations()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            ThrowBeforeConcurrentAggregateCommit = true
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var request = CreateAggregateRelocationRequest(CreateEnvelope());

        var prepared = await coordinator.PrepareAsync(request);

        Assert.Equal(request.Participants.Count,
            prepared.TargetAuthorityOwnerGenerations.Count);
        Assert.All(
            request.Participants.Select((participant, index) =>
                (participant, index)),
            item => Assert.Equal(
                checked((ulong)(100 + item.index)),
                prepared.TargetAuthorityOwnerGeneration(
                    item.participant.Envelope.AuthorityKey)));
    }

    [Fact]
    public async Task AggregatePublicationProbeTimeoutPreservesUnknownRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult =
                new ZLinkAggregatePrepareResult.Conflict(),
            ReadDelay = TimeSpan.FromSeconds(30)
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateEnvelope();
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            envelope.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        $"v-{participant.AuthorityKey.Value}",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        ReadOnlyMemory<byte>.Empty,
                        ReadOnlyMemory<byte>.Empty))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    1)),
            new ZLinkLocationOwnerToken("aggregate-target", 17));

        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await coordinator.PrepareAsync(request));

        Assert.DoesNotContain(
            authority.Events,
            static item => item.Name == "abort");
        Assert.DoesNotContain(
            relocation.Events,
            static item => item.Name == "delete");
        Assert.NotEmpty(relocation.Payloads);
    }

    [Fact]
    public async Task AggregatePublicationProbeMixedResultPreservesUnknownRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult =
                new ZLinkAggregatePrepareResult.Conflict(),
            PublishFirstParticipantBeforePrepareConflict = true
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateEnvelope();
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            envelope.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        $"v-{participant.AuthorityKey.Value}",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        ReadOnlyMemory<byte>.Empty,
                        ReadOnlyMemory<byte>.Empty))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    1)),
            new ZLinkLocationOwnerToken("aggregate-target", 17));

        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await coordinator.PrepareAsync(request));

        Assert.Equal(1, authority.PublishedCount);
        Assert.DoesNotContain(
            authority.Events,
            static item => item.Name == "abort");
        Assert.DoesNotContain(
            relocation.Events,
            static item => item.Name == "delete");
        Assert.NotEmpty(relocation.Payloads);
    }

    [Fact]
    public async Task AggregateTargetPrepareReusesAcceptedOpaqueRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateEnvelope();
        var participants = envelope.Participants.Select(
                participant => new ZLinkAggregateRelocationParticipant(
                    participant,
                    $"v-{participant.AuthorityKey.Value}",
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    new byte[] { 6 },
                    new byte[] { 7 }))
            .ToArray();
        var canonical = envelope with
        {
            InventoryDigest =
                ZLinkAggregateInventoryDigest.Compute(participants)
        };
        var source = await ZLinkRelocationTreeStore.PutAsync(
            relocation,
            canonical,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var putsBeforeTargetPrepare = relocation.Events.Count(
            static item => item.Name == "put");
        var accepted = source.Root with
        {
            ExpiresAt = DateTimeOffset.UtcNow + TimeSpan.FromHours(24),
            StoreNow = DateTimeOffset.UtcNow
        };
        var request = new ZLinkAggregateRelocationRequest(
            canonical.AggregateId,
            canonical.AggregateGeneration,
            participants,
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    1)),
            new ZLinkLocationOwnerToken("aggregate-target", 17),
            canonical);

        var prepared = await coordinator.PrepareExistingAsync(
            request,
            accepted,
            CancellationToken.None);

        Assert.Equal(accepted.Reference, prepared.Relocation.Reference);
        Assert.Equal(
            accepted.ChecksumCrc32c,
            prepared.Relocation.ChecksumCrc32c);
        Assert.Equal(
            putsBeforeTargetPrepare,
            relocation.Events.Count(static item => item.Name == "put"));
    }

    [Fact]
    public async Task SourceCleanupCompletionCasIsDurableAndIdempotent()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var source = CreateEnvelope();
        var pending = source with
        {
            Participants = source.Participants.Select(
                    static participant =>
                        participant.ObjectKind
                        == ZLinkPlacementObjectKind.UserSpot
                            ? participant with
                            {
                                CompletionPayload =
                                    ZLinkSpotRetireCompletionMarker
                                        .CreatePending()
                            }
                            : participant)
                .ToArray()
        };
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("aggregate-target"));
        var targetOwner = new ZLinkLocationOwnerToken(
            "aggregate-target",
            17);
        var request = new ZLinkAggregateRelocationRequest(
            pending.AggregateId,
            pending.AggregateGeneration,
            pending.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        $"v-{participant.AuthorityKey.Value}",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        new byte[] { 6 },
                        ReadOnlyMemory<byte>.Empty))
                .ToArray(),
            targetDescriptor,
            1,
            new ZLinkCapacityVector(1, 1, null),
            targetOwner);
        var published = await coordinator.PublishAsync(request);

        Assert.True(await coordinator.TryCompleteSourceCleanupAsync(
            published,
            targetDescriptor,
            1,
            targetOwner,
            CancellationToken.None));
        var commitsAfterCompletion = authority.Events.Count(
            static item => item.Name == "commit");
        Assert.True(await coordinator.TryCompleteSourceCleanupAsync(
            published,
            targetDescriptor,
            1,
            targetOwner,
            CancellationToken.None));

        Assert.Equal(
            commitsAfterCompletion,
            authority.Events.Count(static item => item.Name == "commit"));
        foreach (var participant in pending.Participants)
        {
            var read = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await authority.ReadAuthorityAsync(
                    participant.AuthorityKey));
            Assert.True(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                read.Snapshot.Payload.Span,
                out var publication));
            Assert.Equal(
                pending.AggregateGeneration + 1,
                publication.AggregateGeneration);
            var durable = await ZLinkRelocationTreeStore.GetAsync(
                relocation,
                publication.Reference,
                publication.ChecksumCrc32c,
                CancellationToken.None);
            var durableSpot = durable.Participants.Single(
                static value => value.ObjectKind
                    == ZLinkPlacementObjectKind.UserSpot);
            Assert.True(ZLinkSpotRetireCompletionMarker.IsCompleted(
                durableSpot.CompletionPayload.Span));
        }
    }

    [Fact]
    public async Task CanonicalReplayRelayAcknowledgementClearsPendingBeforeSourceCleanup()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var caller = new ZLinkManagedMeshNode(
            context,
            "mesh",
            maxPendingOperations: 1);
        caller.SetRoutingId(RoutingId.From("source"));
        await using var oldOwner = NewNode(context, "old-owner");
        await using var target = NewNode(context, "target");
        caller.SetLocalOwnerLeaseGeneration(6);
        target.SetLocalOwnerLeaseGeneration(17);
        var suffix = Guid.NewGuid().ToString("N");
        var callerEndpoint = $"inproc://canonical-reply-caller-{suffix}";
        var oldOwnerEndpoint = $"inproc://canonical-reply-old-owner-{suffix}";
        var targetEndpoint = $"inproc://canonical-reply-target-{suffix}";
        caller.SetBind(callerEndpoint);
        oldOwner.SetBind(oldOwnerEndpoint);
        target.SetBind(targetEndpoint);
        caller.ConnectPeer(oldOwnerEndpoint, oldOwner.RoutingId);
        oldOwner.ConnectPeer(callerEndpoint, caller.RoutingId);
        caller.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(callerEndpoint, caller.RoutingId);
        var requestSource = new ZLinkServiceWireCodec.RequestSourceFence(
            "caller-owner",
            6,
            caller.RoutingId,
            caller.Status().LifecycleGeneration);
        caller.SetLocalRequestSourceFence(requestSource);
        caller.SetRelocationReplyRelayTarget(
            new ZLinkRelocationReplyTarget(
                new Zlink.Framework.Runtime.Backend.DotNet.Wrappers
                    .ZLinkBackendSpotNodeWrapper(caller)));
        caller.Start();
        oldOwner.Start();
        target.Start();
        await WaitUntilAsync(() =>
            caller.Status().AdmittedPeerCount == 2
            && oldOwner.Status().AdmittedPeerCount == 1
            && target.Status().AdmittedPeerCount == 1);
        DrainAndDispose(caller);
        DrainAndDispose(oldOwner);
        DrainAndDispose(target);

        var actor = oldOwner.CreateActor("canonical-reply-actor");
        DrainAndDispose(oldOwner);
        Assert.True(oldOwner.TryGetActorAuthority(
            actor,
            out var actorAuthorityGeneration,
            out var actorOwnerLeaseGeneration));
        caller.ObserveActorAuthority(
            actor,
            oldOwner.Status().LifecycleGeneration,
            actorAuthorityGeneration,
            actorOwnerLeaseGeneration);
        using var requestPayload = Message.From([31]);
        Assert.Equal(
            SubmitResult.Ok,
            caller.RequestToActor(
                actor,
                [requestPayload],
                out var operation,
                TimeSpan.FromSeconds(3)));
        await WaitUntilAsync(() =>
            oldOwner.Status().PendingApplicationMessages > 0);
        using (var requestReady = new MeshReadyBatch())
        {
            oldOwner.DrainReady(
                MeshReadyDomains.Application,
                requestReady,
                RecvFlags.DontWait);
            using var requestClaim = requestReady.TakeClaim(0);
            using var requestBatch = new MeshReceiveBatch();
            Assert.True(requestClaim.Receive(
                requestBatch,
                RecvFlags.DontWait));
            Assert.Equal(MeshRecordKind.ActorRequest, requestBatch[0].Kind);
            Assert.Equal(operation, requestBatch[0].OperationId);
            Assert.Equal(operation.Low, requestBatch[0].ReplyRouteId);
        }

        var header = ZLinkClientCallCodec.CreateEnvelope(
            ZLinkMessageKind.Request,
            "mesh",
            "Ping");
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            header,
            new { Value = 1 },
            typeof(object),
            null);
        byte[] journal;
        using (var received = new ZLinkBackendRouteReceived(
                   parts,
                   caller.RoutingId,
                   "caller",
                   operation.Low,
                   static (_, _) => SubmitResult.Ok,
                   operationId: operation,
                   targetNodeGeneration: 3,
                   authorityOwnerGeneration: 4,
                   ownerLeaseGeneration: 5,
                   sourceNodeGeneration: requestSource.NodeGeneration,
                   requestSource: requestSource))
            journal = ZLinkSpotAcceptedJournal.Encode(
                received,
                operation.Low);
        var queued = new ZLinkRelocationQueuedJob(1, journal)
        {
            RequestSource = new ZLinkCanonicalRequestSourceFence(
                requestSource.OwnerId,
                requestSource.LeaseGeneration,
                requestSource.NodeRid.ToHex(),
                requestSource.NodeGeneration)
        };
        var sourceParticipant = new ZLinkRelocationParticipantEnvelope(
            ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
            ZLinkPlacementObjectKind.UserSpot,
            10,
            11,
            new byte[] { 1 },
            [queued],
            [],
            CompletionPayload: ZLinkSpotRetireCompletionMarker.CreatePending());
        var sourceAuthority = ZLinkUserSpotAuthorityPayloadCodec.Encode(
            new ZLinkUserSpotAuthorityPayload(
                ZLinkUserSpotAuthorityState.Ready,
                "SpotType",
                "spot",
                "source-owner",
                8,
                "mesh",
                oldOwner.RoutingId,
                oldOwner.Status().LifecycleGeneration));
        var sourceRequestParticipant = new ZLinkAggregateRelocationParticipant(
            sourceParticipant,
            "v-source",
            ZLinkAuthorityGenerationTransition.NewOwner,
            sourceAuthority,
            ReadOnlyMemory<byte>.Empty);
        var source = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            ZLinkAggregateInventoryDigest.Compute([sourceRequestParticipant]),
            [sourceParticipant]);
        var targetDescriptor = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            target.RoutingId);
        var targetOwner = new ZLinkLocationOwnerToken("target-owner", 17);
        var targetNodeGeneration = target.Status().LifecycleGeneration;
        var canonical = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            source,
            "spot",
            "SpotType",
            targetDescriptor.Rid,
            12);
        var request = new ZLinkAggregateRelocationRequest(
            canonical.AggregateId,
            canonical.AggregateGeneration,
            [sourceRequestParticipant with { Envelope = canonical.Participants[0] }],
            targetDescriptor,
            targetNodeGeneration,
            new ZLinkCapacityVector(0, 1, null),
            targetOwner,
            canonical);
        var published = await coordinator.PublishAsync(request);
        var participant = Assert.Single(canonical.Participants);
        var accepted = Assert.IsType<ZLinkCanonicalAcceptedRequest>(
            Assert.Single(participant.AcceptedJobs).CanonicalRequest);
        var completion = ZLinkRelocationEnvelopeCodec.CreateCanonicalTerminalCompletion(
            accepted.OperationHigh,
            accepted.OperationLow,
            accepted.Source.OwnerId,
            accepted.Source.OwnerLeaseGeneration,
            accepted.Source.NodeRid,
            accepted.Source.NodeGeneration,
            participant.CanonicalParticipantId,
            1,
            0,
            0,
            0,
            new ZLinkCanonicalApplicationPayload(
                "Pong",
                "application/json",
                "{\"ok\":true}"u8.ToArray()));

        var advanced = await coordinator.AdvanceCanonicalReplayAsync(
            canonical,
            participant.CanonicalParticipantId,
            1,
            completion,
            targetDescriptor,
            targetNodeGeneration,
            targetOwner,
            CancellationToken.None);

        var advancedParticipant = Assert.Single(advanced.Participants);
        Assert.Equal<ulong>(1, advancedParticipant.ReplayCursor);
        Assert.Empty(advancedParticipant.AcceptedJobs);
        Assert.Equal(1, advancedParticipant.PendingRelayCount);
        Assert.Single(advancedParticipant.TerminalCompletions);
        Assert.False(await coordinator.TryCompleteSourceCleanupAsync(
            published,
            targetDescriptor,
            targetNodeGeneration,
            targetOwner,
            CancellationToken.None));
        var pendingRead = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await authority.ReadAuthorityAsync(sourceParticipant.AuthorityKey));
        Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            pendingRead.Snapshot.Payload.Span,
            out var pendingProjection));
        Assert.Equal<uint>(1, pendingProjection.TerminalCompletionCount);
        Assert.Equal<uint>(1, pendingProjection.PendingRelayCount);
        Assert.Equal<uint>(0, pendingProjection.SourceCleanupState);

        var relay = new ZLinkServiceWireCodec.ReplyRelayRecord(
            operation,
            accepted.ReplyRouteId,
            new ZLinkServiceWireCodec.RelocationWireId(
                canonical.CanonicalRelocationHigh,
                canonical.CanonicalRelocationLow),
            canonical.AggregateGeneration,
            new ZLinkServiceWireCodec.RelocationCoordinatorFence(
                targetOwner.OwnerId,
                checked((ulong)targetOwner.LeaseGeneration),
                target.RoutingId,
                targetNodeGeneration,
                pendingRead.Snapshot.StoreVersion),
            participant.CanonicalParticipantId,
            completion.AcceptedSequence,
            completion.TerminalResult,
            (ServiceWireConstants.FrameworkErrorCode)completion.ErrorCode);
        var relayPayload = new[] { Message.From([37]) };
        ZLinkServiceWireCodec.ReplyRelayAckRecord replyAck;
        try
        {
            replyAck = await target.RelayRelocationReplyAsync(
                caller.RoutingId,
                relay,
                requestSource,
                relayPayload,
                TimeSpan.FromSeconds(3),
                CancellationToken.None);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(relayPayload);
        }
        Assert.Equal(
            (byte)ZLinkRelocationReplyCompletionState.TerminalReceived,
            replyAck.Status);
        Assert.Equal(relay.RelocationId, replyAck.RelocationId);
        Assert.Equal(relay.Coordinator, replyAck.Coordinator);
        Assert.Equal(operation, replyAck.OperationId);
        Assert.Equal(accepted.ReplyRouteId, replyAck.ReplyRouteId);
        Assert.Equal(requestSource, replyAck.RequestSource);
        await WaitUntilAsync(() =>
            caller.Status().PendingInfrastructureMessages > 0);
        Assert.Single(DrainRecords(caller).Where(record =>
            record.Kind == MeshRecordKind.Completion
            && record.OperationId == operation));

        var acknowledged = await coordinator.AcknowledgeCanonicalReplyAsync(
            canonical,
            completion,
            replyAck.Status,
            targetDescriptor,
            targetNodeGeneration,
            targetOwner,
            CancellationToken.None);

        var delivered = Assert.Single(
            Assert.Single(acknowledged.Participants).TerminalCompletions);
        Assert.Equal(1, delivered.DeliveryState);
        Assert.Equal(0, Assert.Single(acknowledged.Participants).PendingRelayCount);
        Assert.True(await coordinator.TryCompleteSourceCleanupAsync(
            published,
            targetDescriptor,
            targetNodeGeneration,
            targetOwner,
            CancellationToken.None));
        var read = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await authority.ReadAuthorityAsync(sourceParticipant.AuthorityKey));
        Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            read.Snapshot.Payload.Span,
            out var projection));
        Assert.Equal<uint>(1, projection.TerminalCompletionCount);
        Assert.Equal<uint>(0, projection.PendingRelayCount);
        Assert.Equal<uint>(1, projection.SourceCleanupState);
        Assert.Equal<uint>(8, projection.Phase);
        var durable = await ZLinkRelocationTreeStore.GetAsync(
            relocation,
            projection.RelocationReference,
            projection.RelocationChecksumCrc32c,
            CancellationToken.None);
        Assert.Empty(Assert.Single(durable.Participants).AcceptedJobs);
        Assert.True(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
            read.Snapshot.Payload.Span,
            out var publication));
        var stagedByParticipantId = canonical.Participants.ToDictionary(
            static value => value.CanonicalParticipantId);
        var normalizedDurable = durable with
        {
            Participants = durable.Participants.Select(value =>
                value with
                {
                    AuthorityKey = stagedByParticipantId[
                        value.CanonicalParticipantId].AuthorityKey,
                    CompletionPayload =
                        value.ObjectKind is
                            ZLinkPlacementObjectKind.UserSpot
                            or ZLinkPlacementObjectKind.InstanceSpot
                            ? ZLinkSpotRetireCompletionMarker
                                .CreateCompleted()
                            : value.CompletionPayload
                }).ToArray()
        };
        Assert.True(ZLinkSpotRetireTargetRuntime
            .CanonicalCompletionMatchesTargetStaging(
                canonical,
                normalizedDurable,
                publication,
                stagedParticipant => normalizedDurable.Participants.Single(
                        candidate => candidate.CanonicalParticipantId
                                     == stagedParticipant
                                         .CanonicalParticipantId)
                    .AuthorityOwnerGeneration));
    }

    [Fact]
    public async Task AggregateAuthorityIsNotVisibleUntilTargetStagingCommits()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateEnvelope();
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            envelope.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        $"v-{participant.AuthorityKey.Value}",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        new byte[] { 6 },
                        new byte[] { 7 }))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    1)),
            new ZLinkLocationOwnerToken("aggregate-target", 17));

        var prepared = await coordinator.PrepareAsync(request);

        Assert.Equal(0, authority.PublishedCount);
        var preparedEvents = relocation.Events.Concat(authority.Events)
            .OrderBy(static item => item.Sequence)
            .ToArray();
        var preparedSequence = preparedEvents.Single(
            static item => item.Name == "prepare").Sequence;
        Assert.All(
            preparedEvents.Where(static item => item.Name == "put"),
            item => Assert.True(item.Sequence < preparedSequence));

        await coordinator.CommitAsync(prepared);

        Assert.Equal(2, authority.PublishedCount);
        Assert.Equal("commit", authority.Events[^1].Name);
    }

    [Fact]
    public async Task FailedTargetStagingAbortsPreparedAggregateAndDeletesRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore();
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateEnvelope();
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            envelope.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        $"v-{participant.AuthorityKey.Value}",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        ReadOnlyMemory<byte>.Empty,
                        ReadOnlyMemory<byte>.Empty))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    1)),
            new ZLinkLocationOwnerToken("aggregate-target", 17));

        var prepared = await coordinator.PrepareAsync(request);
        await coordinator.AbortAsync(prepared);

        Assert.Equal(0, authority.PublishedCount);
        Assert.DoesNotContain(
            relocation.Payloads.Values,
            static payload => payload.Take(4).SequenceEqual(
                "ZLTM"u8.ToArray()));
        Assert.Equal("abort", authority.Events[^1].Name);
        Assert.Equal("delete", relocation.Events[^1].Name);
    }

    [Fact]
    public async Task AggregatePrepareConflictDeletesUnpublishedRoot()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult = new ZLinkAggregatePrepareResult.Conflict()
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var envelope = CreateEnvelope();
        var request = new ZLinkAggregateRelocationRequest(
            envelope.AggregateId,
            envelope.AggregateGeneration,
            envelope.Participants.Select(
                    participant => new ZLinkAggregateRelocationParticipant(
                        participant,
                        "v1",
                        ZLinkAuthorityGenerationTransition.NewOwner,
                        ReadOnlyMemory<byte>.Empty,
                        ReadOnlyMemory<byte>.Empty))
                .ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("aggregate-target")),
            1,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    1)),
            new ZLinkLocationOwnerToken("aggregate-target", 17));

        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await coordinator.PublishAsync(request));

        Assert.DoesNotContain(
            relocation.Payloads.Values,
            static payload => payload.Take(4).SequenceEqual(
                "ZLTM"u8.ToArray()));
    }

    [Fact]
    public async Task AggregatePrepareGenerationExhaustionIsImmediateAndStable()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregatePrepareResult =
                new ZLinkAggregatePrepareResult.GenerationExhausted()
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);

        await Assert.ThrowsAsync<ZLinkAuthorityGenerationExhaustedException>(
            () => coordinator.PrepareAsync(
                    CreateAggregateRelocationRequest(CreateEnvelope()))
                .AsTask());

        Assert.Equal(1, authority.Events.Count(
            static item => item.Name == "prepare"));
        Assert.Equal(0, authority.PublishedCount);
        Assert.Equal("delete", relocation.Events[^1].Name);
    }

    [Fact]
    public async Task AggregateCommitGenerationExhaustionDoesNotRetry()
    {
        var relocation = new RecordingRelocationStore();
        var authority = new RecordingAuthorityStore
        {
            AggregateCommitResult = ZLinkAggregateCommitResult.GenerationExhausted
        };
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            authority,
            relocation);
        var prepared = await coordinator.PrepareAsync(
            CreateAggregateRelocationRequest(CreateEnvelope()));

        await Assert.ThrowsAsync<ZLinkAuthorityGenerationExhaustedException>(
            () => coordinator.CommitAsync(prepared).AsTask());

        Assert.Equal(1, authority.Events.Count(
            static item => item.Name == "commit"));
        Assert.Equal(0, authority.PublishedCount);
    }

    [Fact]
    public async Task RemoteStatefulDispatchRequiresObservedOwnerGeneration()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = NewNode(context, "authority-source");
        await using var target = NewNode(context, "authority-target");
        source.SetLocalOwnerLeaseGeneration(21);
        target.SetLocalOwnerLeaseGeneration(21);
        var suffix = Guid.NewGuid().ToString("N");
        var sourceEndpoint = $"inproc://authority-source-{suffix}";
        var targetEndpoint = $"inproc://authority-target-{suffix}";
        source.SetBind(sourceEndpoint);
        target.SetBind(targetEndpoint);
        source.ConnectPeer(targetEndpoint, target.RoutingId);
        target.ConnectPeer(sourceEndpoint, source.RoutingId);
        source.Start();
        target.Start();
        await WaitUntilAsync(
            () => source.Status().AdmittedPeerCount == 1
                  && target.Status().AdmittedPeerCount == 1);

        var actor = target.CreateActor("authority-actor");
        DrainAndDispose(target);
        using var payload = Message.From(new byte[] { 9 });
        Assert.Equal(SubmitResult.NotFound, source.SendToActor(actor, [payload]));

        Assert.True(target.TryGetActorAuthority(
            actor,
            out var ownerGeneration,
            out var ownerLeaseGeneration));
        Assert.Equal(21UL, ownerLeaseGeneration);
        source.ObserveActorAuthority(
            actor,
            target.Status().LifecycleGeneration,
            ownerGeneration + 1,
            21);
        Assert.Equal(SubmitResult.Ok, source.SendToActor(actor, [payload]));
        await Task.Delay(50);
        using (var ready = new MeshReadyBatch())
        {
            target.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            Assert.Equal(0, ready.Count);
        }

        source.ObserveActorAuthority(
            actor,
            target.Status().LifecycleGeneration,
            ownerGeneration,
            20);
        Assert.Equal(SubmitResult.Ok, source.SendToActor(actor, [payload]));
        await Task.Delay(50);
        using (var ready = new MeshReadyBatch())
        {
            target.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            Assert.Equal(0, ready.Count);
        }

        source.ObserveActorAuthority(
            actor,
            target.Status().LifecycleGeneration,
            ownerGeneration,
            21);
        Assert.Equal(SubmitResult.Ok, source.SendToActor(actor, [payload]));
        await WaitUntilAsync(() =>
        {
            using var ready = new MeshReadyBatch();
            target.DrainReady(
                MeshReadyDomains.Application,
                ready,
                RecvFlags.DontWait);
            return ready.Count == 1;
        });
    }

    private static ZLinkRelocationEnvelope CreateEnvelope()
    {
        var digest = Enumerable.Range(0, 32).Select(static value => (byte)value).ToArray();
        return new ZLinkRelocationEnvelope(
            Guid.Parse("9f952e1b-df66-42bd-84ee-47d48962937a"),
            3,
            digest,
            [
                new ZLinkRelocationParticipantEnvelope(
                    new ZLinkAuthorityKey("spot:mesh:room"),
                    ZLinkPlacementObjectKind.UserSpot,
                    5,
                    11,
                    new byte[] { 1, 2, 3 },
                    [
                        new ZLinkRelocationQueuedJob(41, new byte[] { 4, 1 }),
                        new ZLinkRelocationQueuedJob(42, new byte[] { 4, 2 })
                    ],
                    [
                        new ZLinkRelocationLogicalTimer(
                            "heartbeat",
                            1_900_000_000_000,
                            5_000,
                            new byte[] { 5 })
                    ]),
                new ZLinkRelocationParticipantEnvelope(
                    new ZLinkAuthorityKey("actor:mesh:user-7"),
                    ZLinkPlacementObjectKind.Actor,
                    8,
                    13,
                    new byte[] { 7, 7 },
                    [],
                    [])
            ]);
    }

    private static ZLinkRelocationEnvelope CreateLargeEnvelope(
        int participantCount)
    {
        var participants =
            new ZLinkRelocationParticipantEnvelope[participantCount];
        for (var index = 0; index < participantCount; index++)
        {
            participants[index] = new ZLinkRelocationParticipantEnvelope(
                new ZLinkAuthorityKey($"actor:bulk:{index}"),
                ZLinkPlacementObjectKind.Actor,
                checked((ulong)index + 1),
                checked((ulong)index + 1),
                new byte[] { (byte)(index % 251) },
                [],
                []);
        }
        return new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            SHA256.HashData([0x42, 0x55, 0x4c, 0x4b]),
            participants);
    }

    private static ZLinkMeshNodeDescriptor SourceDescriptor(
        RoutingId rid,
        ulong lifecycleGeneration,
        string ownerId,
        long leaseGeneration) =>
        new(
            "mesh",
            rid,
            lifecycleGeneration,
            1,
            "tcp://127.0.0.1:5000",
            new Dictionary<string, int>(),
            "source-security",
            ownerId,
            leaseGeneration,
            DateTimeOffset.UtcNow)
        {
            State = ZLinkFrameworkRuntimeState.Serving,
            ObjectRole = ZLinkMeshNodeObjectRole.Server
        };

    private static ZLinkRelocationPublicationRequest CreateRequest(
        ZLinkRelocationEnvelope envelope) =>
        new(
            new ZLinkAuthorityKey("spot:mesh:room"),
            "v0",
            ZLinkAuthorityGenerationTransition.Preserve,
            "target-owner",
            9,
            new byte[] { 8, 8 },
            null,
            envelope);

    private static ZLinkPlacementAllocation TestAllocation() =>
        new(
            ZLinkPlacementAllocationState.Active,
            ZLinkPlacementObjectKind.Actor,
            "Test.Actor",
            new ZLinkMeshNodeDescriptorKey(
                "mesh",
                RoutingId.From("target")),
            1,
            ActorCapacity());

    private static ZLinkObjectReservationRequest ObjectReservation(
        string authorityKey,
        ZLinkMeshNodeDescriptorKey descriptor,
        ZLinkLocationOwnerToken owner,
        int capacityDelta = 1,
        string stableType = "Game.Actor") =>
        new(
            ZLinkPlacementObjectKind.Actor,
            new ZLinkAuthorityKey(authorityKey),
            stableType,
            $"intent:{authorityKey}",
            SHA256.HashData(
                System.Text.Encoding.UTF8.GetBytes(authorityKey)),
            System.Text.Encoding.UTF8.GetByteCount(authorityKey),
            descriptor,
            1,
            owner,
            new byte[] { 0x10 },
            new ZLinkCapacityVector(capacityDelta, 0, null));

    private static ZLinkMeshNodeDescriptor AuthorityDescriptor(
        string rid,
        ZLinkLocationOwnerToken owner,
        string stableType = "Game.Actor",
        int? activeLimit = null,
        int? pendingLimit = null) =>
        new(
            "mesh",
            RoutingId.From(rid),
            LifecycleGeneration: 1,
            DescriptorRevision: 1,
            $"inproc://{rid}",
            new Dictionary<string, int>(StringComparer.Ordinal)
            {
                ["mesh"] = 100
            },
            SecurityIdentity: string.Empty,
            OwnerId: owner.OwnerId,
            LeaseGeneration: owner.LeaseGeneration,
            UpdatedAt: DateTimeOffset.UtcNow)
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    stableType,
                    ZLinkObjectMaintenancePolicyKind.Recreate,
                    HasSnapshotAdapter: false,
                    Limit: 0)
            ],
            State = ZLinkFrameworkRuntimeState.Serving,
            EntrySpotId =
                $"{rid}-entry-00000000-0000-4000-8000-000000000001",
            Capacity = new(
                new ZLinkPopulationCapacity(
                    0,
                    0,
                    activeLimit ?? 10_000),
                new ZLinkPopulationCapacity(0, 0, 0),
                Array.Empty<ZLinkSpotTypeCapacity>())
        };

    private static ZLinkRelocationCapacityReservationRequest
        RelocationReservation(
            ZLinkAuthoritySnapshot source,
            ZLinkMeshNodeDescriptorKey sourceDescriptor,
            ZLinkLocationOwnerToken sourceOwner,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            ZLinkLocationOwnerToken targetOwner) =>
        new(
            Guid.NewGuid(),
            new ZLinkAuthorityKey("actor:mesh:relocate"),
            source.StoreVersion,
            source.Allocation.ObjectKind,
            source.Allocation.StableType,
            sourceDescriptor,
            source.Allocation.DescriptorLifecycleGeneration,
            sourceOwner,
            targetDescriptor,
            1,
            targetOwner,
            source.Allocation.Capacity);

    private static ZLinkCapacityVector ActorCapacity() =>
        new(1, 0, null);

    private static ZLinkManagedMeshNode NewNode(
        IContext context,
        string name)
    {
        var node = new ZLinkManagedMeshNode(context, "mesh");
        node.SetRoutingId(RoutingId.From(name));
        node.AddChannel("mesh");
        return node;
    }

    private static void DrainAndDispose(ZLinkManagedMeshNode node)
        => _ = DrainRecords(node);

    private static List<MeshReceiveRecord> DrainRecords(
        ZLinkManagedMeshNode node)
    {
        var records = new List<MeshReceiveRecord>();
        using var ready = new MeshReadyBatch();
        node.DrainReady(MeshReadyDomains.All, ready, RecvFlags.DontWait);
        for (var index = 0; index < ready.Count; index++)
        {
            using var claim = ready.TakeClaim(index);
            using var received = new MeshReceiveBatch();
            while (claim.Receive(received, RecvFlags.DontWait))
            {
                for (var record = 0; record < received.Count; record++)
                    records.Add(received[record]);
                received.Reset();
            }
        }
        return records;
    }

    private static async Task WaitUntilAsync(Func<bool> predicate)
    {
        var deadline = Stopwatch.GetTimestamp()
                       + (long)(Stopwatch.Frequency * 5);
        while (!predicate())
        {
            if (Stopwatch.GetTimestamp() >= deadline)
                throw new TimeoutException();
            await Task.Delay(10);
        }
    }

    private sealed class RecordingRelocationStore : IZLinkRelocationRepository
    {
        internal Dictionary<string, byte[]> Payloads { get; } =
            new(StringComparer.Ordinal);

        internal List<(long Sequence, string Name)> Events { get; } = [];

        internal void Seed(string reference, byte[] payload) =>
            Payloads.Add(reference, payload);

        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var bytes = payload.ToArray();
            var reference = Convert.ToHexString(
                    System.Security.Cryptography.SHA256.HashData(bytes))
                .ToLowerInvariant();
            Payloads[reference] = bytes;
            Events.Add((EventClock.Next(), "put"));
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(bytes),
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
            Events.Add((EventClock.Next(), "put"));
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult(new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(bytes),
                now + retention,
                now));
        }

        public ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Events.Add((EventClock.Next(), "get"));
            return ValueTask.FromResult<ZLinkRelocationReadResult>(
                Payloads.TryGetValue(reference, out var payload)
                    ? new ZLinkRelocationReadResult.Found(payload)
                    : new ZLinkRelocationReadResult.Missing());
        }

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
            CancellationToken cancellationToken = default)
        {
            Events.Add((EventClock.Next(), "delete"));
            return ValueTask.FromResult(
                Payloads.Remove(reference)
                    ? ZLinkRelocationDeleteResult.Deleted
                    : ZLinkRelocationDeleteResult.Missing);
        }
    }

    private sealed class AsyncOnlyRelocationStore : IZLinkRelocationRepository
    {
        private readonly ConcurrentDictionary<string, byte[]> _payloads =
            new(StringComparer.Ordinal);
        private int _yieldCount;

        internal int YieldCount => Volatile.Read(ref _yieldCount);
        internal ConcurrentQueue<int> PayloadSizes { get; } = [];
        internal int FailRenewAt { get; set; }
        private int _renewCount;

        public async ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            await YieldAsync(cancellationToken);
            var bytes = payload.ToArray();
            var reference = Convert.ToHexString(SHA256.HashData(bytes));
            _payloads[reference] = bytes;
            PayloadSizes.Enqueue(bytes.Length);
            var now = DateTimeOffset.UtcNow;
            return new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(bytes),
                now + retention,
                now);
        }

        public async ValueTask<ZLinkRelocationStored> PutRelocationAtAsync(
            string reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            await YieldAsync(cancellationToken);
            var bytes = payload.ToArray();
            if (_payloads.TryGetValue(reference, out var current)
                && !current.AsSpan().SequenceEqual(bytes))
                throw new InvalidDataException("Relocation reference collision.");
            _payloads[reference] = bytes;
            PayloadSizes.Enqueue(bytes.Length);
            var now = DateTimeOffset.UtcNow;
            return new ZLinkRelocationStored(
                reference,
                ZLinkCrc32C.Compute(bytes),
                now + retention,
                now);
        }

        public async ValueTask<ZLinkRelocationReadResult> GetRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default)
        {
            await YieldAsync(cancellationToken);
            return _payloads.TryGetValue(reference, out var payload)
                ? new ZLinkRelocationReadResult.Found(payload)
                : new ZLinkRelocationReadResult.Missing();
        }

        public async ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            await YieldAsync(cancellationToken);
            if (FailRenewAt > 0
                && Interlocked.Increment(ref _renewCount) == FailRenewAt)
                return new ZLinkRelocationRenewResult.Missing();
            var now = DateTimeOffset.UtcNow;
            return _payloads.ContainsKey(reference)
                ? new ZLinkRelocationRenewResult.Renewed(now + retention, now)
                : new ZLinkRelocationRenewResult.Missing();
        }

        public async ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default)
        {
            await YieldAsync(cancellationToken);
            return _payloads.TryRemove(reference, out _)
                ? ZLinkRelocationDeleteResult.Deleted
                : ZLinkRelocationDeleteResult.Missing;
        }

        private async ValueTask YieldAsync(CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            await Task.Yield();
            Interlocked.Increment(ref _yieldCount);
        }
    }


    private sealed class RecordingAuthorityStore : ZLinkLocationStoreTestDouble
    {
        private readonly ConcurrentDictionary<string, ZLinkAuthoritySnapshot>
            _snapshots =
            new(StringComparer.Ordinal);
        private ZLinkAggregatePrepareRequest? _prepared;

        internal bool Conflict { get; init; }

        internal bool ThrowAfterCommit { get; init; }

        internal ZLinkAggregatePrepareResult? AggregatePrepareResult { get; init; }

        internal ZLinkAggregateCommitResult? AggregateCommitResult { get; init; }

        internal bool PublishFirstParticipantBeforePrepareConflict
        {
            get;
            init;
        }

        internal bool ThrowAfterPublishingAggregatePrepare { get; init; }

        internal bool ThrowBeforeConcurrentAggregateCommit { get; init; }

        private int _concurrentAggregateCommitPublished;
        private readonly object _aggregatePublicationGate = new();

        internal int PublishedCount => _snapshots.Count;
        internal TimeSpan ReadDelay { get; init; }
        private int _activeReads;
        private int _maximumConcurrentReads;

        internal int MaximumConcurrentReads =>
            Volatile.Read(ref _maximumConcurrentReads);

        internal List<(long Sequence, string Name)> Events { get; } = [];

        public override async ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default)
        {
            if (ThrowBeforeConcurrentAggregateCommit)
            {
                lock (_aggregatePublicationGate)
                {
                    if (_concurrentAggregateCommitPublished == 0)
                    {
                        Assert.NotNull(_prepared);
                        PublishAggregate(_prepared!);
                        _concurrentAggregateCommitPublished = 1;
                    }
                }
            }
            var active = Interlocked.Increment(ref _activeReads);
            var maximum = Volatile.Read(ref _maximumConcurrentReads);
            while (maximum < active)
            {
                var previous = Interlocked.CompareExchange(
                    ref _maximumConcurrentReads,
                    active,
                    maximum);
                if (previous == maximum) break;
                maximum = previous;
            }
            lock (Events)
                Events.Add((EventClock.Next(), "read"));
            try
            {
                if (ReadDelay > TimeSpan.Zero)
                    await Task.Delay(ReadDelay, cancellationToken);
                return
                !_snapshots.TryGetValue(key.Value, out var snapshot)
                    ? new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UtcNow)
                    : new ZLinkAuthorityReadResult.Found(snapshot);
            }
            finally
            {
                Interlocked.Decrement(ref _activeReads);
            }
        }

        public override ValueTask<ZLinkAuthorityCompareExchangeResult>
            CompareExchangeAuthorityAsync(
                ZLinkAuthorityKey key,
                string expectedStoreVersion,
                ZLinkAuthorityMutation mutation,
                CancellationToken cancellationToken = default)
        {
            Events.Add((EventClock.Next(), "cas"));
            if (Conflict)
                return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                    new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UtcNow)));
            var put = Assert.IsType<ZLinkAuthorityMutation.Put>(mutation);
            var targetOwner = put.TargetOwner
                              ?? new ZLinkLocationOwnerToken(
                                  "target-owner",
                                  9);
            var snapshot = new ZLinkAuthoritySnapshot(
                "v1",
                put.Payload,
                1,
                1,
                targetOwner.OwnerId,
                targetOwner.LeaseGeneration,
                TestAllocation(),
                null,
                DateTimeOffset.UtcNow);
            _snapshots[key.Value] = snapshot;
            if (ThrowAfterCommit)
                throw new IOException("commit outcome unknown");
            return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                new ZLinkAuthorityCompareExchangeResult.Stored(snapshot));
        }

        public override ValueTask<ZLinkAggregatePrepareResult> PrepareAggregateAsync(
            ZLinkAggregatePrepareRequest request,
            CancellationToken cancellationToken = default)
        {
            Events.Add((EventClock.Next(), "prepare"));
            if (AggregatePrepareResult is { } configured)
            {
                if (PublishFirstParticipantBeforePrepareConflict)
                {
                    var participant = request.Participants[0];
                    Assert.True(
                        ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                            participant.AuthorityPayload.Span,
                            out var publication));
                    _snapshots[participant.Key.Value] =
                        new ZLinkAuthoritySnapshot(
                            $"v-{participant.Key.Value}-partial",
                            participant.AuthorityPayload,
                            1,
                            1,
                            publication.TargetOwnerId,
                            publication.TargetOwnerLeaseGeneration,
                            TestAllocation(),
                            null,
                            DateTimeOffset.UtcNow);
                }
                return ValueTask.FromResult(configured);
            }
            _prepared = request;
            if (ThrowAfterPublishingAggregatePrepare)
            {
                PublishAggregate(request);
                throw new IOException(
                    "aggregate prepare response was lost");
            }
            if (ThrowBeforeConcurrentAggregateCommit)
                throw new IOException(
                    "aggregate prepare outcome was ambiguous");
            var generations = request.Participants
                .Select((participant, index) =>
                    new KeyValuePair<ZLinkAuthorityKey, ulong>(
                        participant.Key,
                        checked((ulong)(100 + index))))
                .ToDictionary(
                    static pair => pair.Key,
                    static pair => pair.Value);
            return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                new ZLinkAggregatePrepareResult.Prepared(
                    new ZLinkAggregateFence(
                        request.AggregateId,
                        request.AggregateGeneration))
                {
                    TargetAuthorityOwnerGenerations = generations
                });
        }

        public override ValueTask<ZLinkAggregateCommitResult> CommitAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default)
        {
            Events.Add((EventClock.Next(), "commit"));
            Assert.NotNull(_prepared);
            if (AggregateCommitResult is { } configured)
                return ValueTask.FromResult(configured);
            foreach (var participant in _prepared!.Participants)
            {
                Assert.True(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    participant.AuthorityPayload.Span,
                    out var publication));
                _snapshots[participant.Key.Value] = new ZLinkAuthoritySnapshot(
                    $"v-{participant.Key.Value}-next",
                    participant.AuthorityPayload,
                    1,
                    1,
                    publication.TargetOwnerId,
                    publication.TargetOwnerLeaseGeneration,
                    TestAllocation(),
                    null,
                    DateTimeOffset.UtcNow);
            }
            return ValueTask.FromResult(ZLinkAggregateCommitResult.Committed);
        }

        private void PublishAggregate(ZLinkAggregatePrepareRequest request)
        {
            for (var index = 0; index < request.Participants.Count; index++)
            {
                var participant = request.Participants[index];
                Assert.True(ZLinkRelocationAuthorityPayloadCodec.TryDecode(
                    participant.AuthorityPayload.Span,
                    out var publication));
                _snapshots[participant.Key.Value] =
                    new ZLinkAuthoritySnapshot(
                        $"v-{participant.Key.Value}-published",
                        participant.AuthorityPayload,
                        1,
                        checked((ulong)(100 + index)),
                        publication.TargetOwnerId,
                        publication.TargetOwnerLeaseGeneration,
                        TestAllocation(),
                        null,
                        DateTimeOffset.UtcNow);
            }
        }

        public override ValueTask<ZLinkAggregateAbortResult> AbortAggregateAsync(
            ZLinkAggregateFence fence,
            CancellationToken cancellationToken = default)
        {
            Events.Add((EventClock.Next(), "abort"));
            _prepared = null;
            return ValueTask.FromResult(ZLinkAggregateAbortResult.Aborted);
        }
    }

    private static class EventClock
    {
        private static long _sequence;

        internal static long Next() => Interlocked.Increment(ref _sequence);
    }

    private sealed class TestRelocatableSpot(IZLinkSpotContext context) : IZLinkSpot
    {
        public IZLinkSpotContext Context { get; } = context;
    }

    private sealed class TestInstanceSpot : IZLinkInstanceSpot
    {
        public IZLinkInstanceSpotContext Context => null!;
    }

    private sealed class TestSpotRelocationAdapter
        : IZLinkSpotRelocationAdapter<TestRelocatableSpot>
    {
        public ValueTask<byte[]> CaptureAsync(
            TestRelocatableSpot spot,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            TestRelocatableSpot spot,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class TestRelocatableActor(
        string actorId,
        IZLinkActorContext context) : IZLinkActor
    {
        public string ActorId { get; } = actorId;

        public IZLinkActorContext Context { get; } = context;
    }

    private sealed class TestRelocatableActorFactory
        : IZLinkActorFactory<TestRelocatableActor>
    {
        public ValueTask<TestRelocatableActor> CreateAsync(
            IZLinkActorContext context,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(
                new TestRelocatableActor(context.ActorId, context));
    }

    private sealed class TestActorRelocationAdapter
        : IZLinkActorRelocationAdapter<TestRelocatableActor>
    {
        public ValueTask<byte[]> CaptureAsync(
            TestRelocatableActor actor,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(Array.Empty<byte>());

        public ValueTask RestoreAsync(
            TestRelocatableActor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken) =>
            ValueTask.CompletedTask;
    }

    private sealed class RecordingActorRelocationAdapter
        : IZLinkActorRelocationAdapter<TestRelocatableActor>
    {
        public byte[]? RestoredPayload { get; private set; }

        public ValueTask<byte[]> CaptureAsync(
            TestRelocatableActor actor,
            CancellationToken cancellationToken) =>
            ValueTask.FromResult(new byte[] { 1, 2, 3 });

        public ValueTask RestoreAsync(
            TestRelocatableActor actor,
            ReadOnlyMemory<byte> payload,
            CancellationToken cancellationToken)
        {
            RestoredPayload = payload.ToArray();
            return ValueTask.CompletedTask;
        }
    }

    private static ZLinkAggregateRelocationRequest CreateAggregateRelocationRequest(
        ZLinkRelocationEnvelope envelope) => new(
        envelope.AggregateId,
        envelope.AggregateGeneration,
        envelope.Participants.Select(participant =>
                new ZLinkAggregateRelocationParticipant(
                    participant,
                    $"v-{participant.AuthorityKey.Value}",
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    ReadOnlyMemory<byte>.Empty,
                    ReadOnlyMemory<byte>.Empty))
            .ToArray(),
        new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("aggregate-target")),
        1,
        new ZLinkCapacityVector(1, 1, null),
        new ZLinkLocationOwnerToken("aggregate-target", 17));

    private static TargetStage CreateTargetStageForHeldJournal() =>
        new(
            null!,
            null!,
            CreateEnvelope(),
            [],
            "room",
            "mesh",
            RoutingId.From("source"),
            1,
            new ZLinkLocationOwnerToken("source-owner", 1),
            "staging-root",
            1,
            DateTimeOffset.MaxValue,
            [],
            null!,
            new NoopDisposable(),
            null,
            new Dictionary<string, ulong>(StringComparer.Ordinal),
            1,
            2,
            "mesh",
            1,
            1,
            1);

    private static (
        TargetStage Stage,
        ZLinkRelocationRecoveryCandidate Candidate)
        CreateCanonicalPublishedReconciliationFixture(
            byte sourceCleanupState)
    {
        var sourceRid = RoutingId.From("source");
        var targetRid = RoutingId.From("target");
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("room");
        var authorityPayload = ZLinkUserSpotAuthorityPayloadCodec.Encode(
            new ZLinkUserSpotAuthorityPayload(
                ZLinkUserSpotAuthorityState.Ready,
                "Game.Room",
                "room",
                "source-owner",
                1,
                "mesh",
                sourceRid,
                1));
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Encode(
            new ZLinkCanonicalParticipantRecovery(
                key,
                ZLinkPlacementObjectKind.UserSpot,
                5,
                11,
                "v-source",
                "Game.Room",
                authorityPayload,
                ReadOnlyMemory<byte>.Empty));
        var participant = new ZLinkRelocationParticipantEnvelope(
            key,
            ZLinkPlacementObjectKind.UserSpot,
            5,
            11,
            new byte[] { 1 },
            [],
            [],
            recovery,
            ZLinkSpotRetireCompletionMarker.CreatePending());
        var relocationParticipant = new ZLinkAggregateRelocationParticipant(
            participant,
            "v-source",
            ZLinkAuthorityGenerationTransition.NewOwner,
            authorityPayload,
            ReadOnlyMemory<byte>.Empty);
        var source = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            ZLinkAggregateInventoryDigest.Compute([relocationParticipant]),
            [participant]);
        var canonical = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            source,
            "room",
            "Game.Room",
            targetRid,
            1);
        var decoded = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(canonical));
        Assert.True(
            Assert.Single(decoded.Participants).CompletionPayload.IsEmpty);
        var staged = decoded with
        {
            AggregateGeneration = 1,
            Participants =
            [
                decoded.Participants[0] with
                {
                    AuthorityKey = key,
                    ObjectKind = ZLinkPlacementObjectKind.UserSpot,
                    ObjectGeneration = 5,
                    AuthorityOwnerGeneration = 11,
                    CompletionPayload =
                        ZLinkSpotRetireCompletionMarker.CreatePending()
                }
            ]
        };
        var publishedAuthority =
            ZLinkCanonicalRelocationAuthorityStateCodec
                .ReplaceRelocationState(
                    authorityPayload,
                    new ZLinkCanonicalRelocationAuthorityState(
                        canonical.CanonicalRelocationHigh,
                        canonical.CanonicalRelocationLow,
                        1,
                        sourceRid.ToHex(),
                        1,
                        "source-owner",
                        1,
                        targetRid.ToHex(),
                        1,
                        "target-owner",
                        1,
                        1,
                        "target-owner",
                        1,
                        targetRid.ToHex(),
                        1,
                        5,
                        "published-root",
                        37,
                        canonical.CanonicalApplicationVersion,
                        sourceCleanupState),
                    decoded);
        var allocation = new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.Active,
            ZLinkPlacementObjectKind.UserSpot,
            "Game.Room",
            new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
            1,
            new ZLinkCapacityVector(
                0,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "Game.Room",
                    1)));
        var authority = new ZLinkAuthorityEntry(
            key,
            new ZLinkAuthoritySnapshot(
                "v-published",
                publishedAuthority,
                5,
                12,
                "target-owner",
                1,
                allocation,
                null,
                DateTimeOffset.UtcNow));
        var stage = CreateTargetStageForHeldJournal() with
        {
            Envelope = staged,
            SourceMeshName = "mesh",
            SourceNodeRid = sourceRid,
            SourceNodeLifecycleGeneration = 1,
            SourceOwner = new ZLinkLocationOwnerToken(
                "source-owner",
                1),
            SourceAuthorityOwnerGeneration = 11,
            TargetAuthorityOwnerGeneration = 12
        };
        var candidate = new ZLinkRelocationRecoveryCandidate(
            new ZLinkRelocationManifestReference(
                "published-root",
                37,
                decoded.AggregateId,
                decoded.AggregateGeneration,
                decoded.InventoryDigest),
            decoded,
            [authority]);
        return (stage, candidate);
    }

    private sealed class NoopDisposable : IDisposable
    {
        public void Dispose()
        {
        }
    }

    private sealed class TakeoverCommitStore : ZLinkLocationStoreTestDouble
    {
        private readonly ZLinkAggregateCommitResult? _result;
        private readonly Exception? _error;

        internal TakeoverCommitStore(ZLinkAggregateCommitResult result)
        {
            _result = result;
        }

        internal TakeoverCommitStore(Exception error)
        {
            _error = error;
        }

        internal int AbortCount { get; private set; }
        internal bool StagingHeld { get; private set; } = true;

        public override ValueTask<ZLinkAggregatePrepareResult>
            PrepareAggregateAsync(
                ZLinkAggregatePrepareRequest request,
                CancellationToken cancellationToken = default)
        {
            StagingHeld = true;
            return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                new ZLinkAggregatePrepareResult.Prepared(
                    new ZLinkAggregateFence(Guid.NewGuid(), 8)));
        }

        public override ValueTask<ZLinkAggregateCommitResult>
            CommitAggregateAsync(
                ZLinkAggregateFence fence,
                CancellationToken cancellationToken = default) =>
            _error is null
                ? ValueTask.FromResult(_result!.Value)
                : ValueTask.FromException<ZLinkAggregateCommitResult>(_error);

        public override ValueTask<ZLinkAggregateAbortResult>
            AbortAggregateAsync(
                ZLinkAggregateFence fence,
                CancellationToken cancellationToken = default)
        {
            AbortCount++;
            StagingHeld = false;
            return ValueTask.FromResult(ZLinkAggregateAbortResult.Aborted);
        }
    }

    private sealed class TakeoverRepositoryStore
        : ZLinkLocationStoreTestDouble
    {
        private readonly Dictionary<ZLinkAuthorityKey, ZLinkAuthoritySnapshot>
            _authorities;
        private ZLinkAggregatePrepareRequest? _prepared;

        internal TakeoverRepositoryStore(
            IReadOnlyList<ZLinkAuthorityEntry> authorities)
        {
            _authorities = authorities.ToDictionary(
                static entry => entry.Key,
                static entry => entry.Snapshot);
        }

        public override ValueTask<ZLinkAuthorityReadResult>
            ReadAuthorityAsync(
                ZLinkAuthorityKey key,
                CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ZLinkAuthorityReadResult>(
                _authorities.TryGetValue(key, out var snapshot)
                    ? new ZLinkAuthorityReadResult.Found(snapshot)
                    : new ZLinkAuthorityReadResult.Missing(
                        DateTimeOffset.UtcNow));

        public override ValueTask<ZLinkAggregatePrepareResult>
            PrepareAggregateAsync(
                ZLinkAggregatePrepareRequest request,
                CancellationToken cancellationToken = default)
        {
            _prepared = request;
            return ValueTask.FromResult<ZLinkAggregatePrepareResult>(
                new ZLinkAggregatePrepareResult.Prepared(
                    new ZLinkAggregateFence(
                        request.AggregateId,
                        request.AggregateGeneration)));
        }

        public override ValueTask<ZLinkAggregateCommitResult>
            CommitAggregateAsync(
                ZLinkAggregateFence fence,
                CancellationToken cancellationToken = default)
        {
            var request = Assert.IsType<ZLinkAggregatePrepareRequest>(
                _prepared);
            Assert.Equal(request.AggregateId, fence.AggregateId);
            Assert.Equal(
                request.AggregateGeneration,
                fence.AggregateGeneration);
            foreach (var mutation in request.Participants)
            {
                var current = _authorities[mutation.Key];
                _authorities[mutation.Key] = current with
                {
                    StoreVersion =
                        $"v-{request.AggregateGeneration}-{mutation.Key.Value}",
                    Payload = mutation.AuthorityPayload,
                    AuthorityOwnerGeneration =
                        checked(current.AuthorityOwnerGeneration + 1),
                    OwnerId = request.TargetOwner.OwnerId,
                    OwnerLeaseGeneration =
                        request.TargetOwner.LeaseGeneration,
                    Allocation = current.Allocation with
                    {
                        Descriptor = request.TargetDescriptor,
                        DescriptorLifecycleGeneration =
                            request.TargetDescriptorLifecycleGeneration
                    }
                };
            }
            return ValueTask.FromResult(
                ZLinkAggregateCommitResult.Committed);
        }
    }

    private sealed class CountingDisposable : IDisposable
    {
        private int _disposeCount;

        internal int DisposeCount => Volatile.Read(ref _disposeCount);

        public void Dispose() => Interlocked.Increment(ref _disposeCount);
    }
}
