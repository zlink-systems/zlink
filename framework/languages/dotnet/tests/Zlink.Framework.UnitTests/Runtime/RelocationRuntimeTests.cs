using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Framework.Runtime.Protocol;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Configuration.Builders;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;
using Zlink.Framework.Runtime.Timers;

namespace Zlink.Framework.UnitTests;

public sealed class RelocationRuntimeTests
{
    [Fact]
    public void Target_observation_accepts_only_byte_exact_steady_authority()
    {
        var (stage, _) = CreateCanonicalPublishedReconciliationFixture();
        var participant = Assert.Single(stage.Envelope.Participants) with
        {
            RecoveryPayload = CanonicalFixtureRecovery()
        };
        var target = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("target"));
        var owner = new ZLinkLocationOwnerToken("target-owner", 1);
        var authorityPayload = CanonicalFixtureAuthorityPayload();
        var payload = ZLinkSpotRetireTargetRuntime.BuildTargetReadyPayload(
            participant.ObjectKind,
            authorityPayload,
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
                "Game.Room",
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
    public void Target_observation_rejects_corrupt_steady_authority()
    {
        var (stage, _) = CreateCanonicalPublishedReconciliationFixture();
        var participant = Assert.Single(stage.Envelope.Participants) with
        {
            RecoveryPayload = CanonicalFixtureRecovery()
        };
        var target = new ZLinkMeshNodeDescriptorKey(
            "mesh",
            RoutingId.From("target"));
        var owner = new ZLinkLocationOwnerToken("target-owner", 1);
        var authorityPayload = CanonicalFixtureAuthorityPayload();
        var payload = ZLinkSpotRetireTargetRuntime.BuildTargetReadyPayload(
                participant.ObjectKind,
                authorityPayload,
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
                "Game.Room",
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
    public void TargetStage_UsesEachActorsPreparedAuthorityGeneration()
    {
        var stage = CreateTargetStageForHeldJournal() with
        {
            ActorTargetAuthorityOwnerGenerations =
                new Dictionary<ZLinkActorId, ulong>
                {
                    [ZLinkActorId.FromBoundary("actor-a", "actorId")] = 41,
                    [ZLinkActorId.FromBoundary("actor-b", "actorId")] = 73
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

        Assert.Equal(435, read.LogicalLength);
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
    public void CanonicalRelocationEnvelopeProjectsSavedWorkAndTimers()
    {
        var logical = ReadCanonicalRelocationGolden();

        var envelope = ZLinkRelocationEnvelopeCodec.Decode(logical);

        Assert.Equal(logical, ZLinkRelocationEnvelopeCodec.Encode(envelope));
        Assert.Collection(
            envelope.Participants,
            participant => Assert.Equal<ulong>(1, participant.CanonicalParticipantId),
            participant => Assert.Equal<ulong>(2, participant.CanonicalParticipantId),
            participant => Assert.Equal<ulong>(3, participant.CanonicalParticipantId));

        var savedWorkParticipant = Assert.Single(
            envelope.Participants,
            static participant => participant.CanonicalParticipantId == 2);
        var savedWork = Assert.Single(savedWorkParticipant.AcceptedJobs);
        Assert.Equal<ulong>(1, savedWork.AcceptedSequence);
        Assert.NotEmpty(savedWork.Payload.ToArray());
        var timerParticipant = Assert.Single(
            envelope.Participants,
            static participant => participant.CanonicalParticipantId == 3);
        var timer = Assert.Single(timerParticipant.LogicalTimers);
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
        Assert.True(savedWorkParticipant.CompletionPayload.IsEmpty);
        Assert.True(timerParticipant.CompletionPayload.IsEmpty);
        var stateOnlyParticipant = Assert.Single(
            envelope.Participants,
            static participant => participant.CanonicalParticipantId == 1);
        Assert.Empty(stateOnlyParticipant.AcceptedJobs);
        Assert.Empty(stateOnlyParticipant.LogicalTimers);
        Assert.True(stateOnlyParticipant.CompletionPayload.IsEmpty);
    }

    [Theory]
    [InlineData(ZLinkTimerOverrunPolicy.SkipLateTicks, 0, 1)]
    [InlineData(ZLinkTimerOverrunPolicy.CatchUpBounded, 7, 7)]
    [InlineData(ZLinkTimerOverrunPolicy.DelayNextTick, -3, 1)]
    public void CanonicalRelocationTimerNormalizesOnlyIgnoredCatchUpBounds(
        ZLinkTimerOverrunPolicy policy,
        int configuredBound,
        int expectedBound)
    {
        var logicalTimer = ZLinkSpotTimerRelocationCodec.Encode(
            new ZLinkSpotLogicalTimerSnapshot(
                typeof(RelocationRuntimeTests),
                typeof(RelocationRuntimeTests),
                new ZLinkTimerLogicalSnapshot(
                    "timer",
                    TimeSpan.FromSeconds(1),
                    new ZLinkTimerOptions
                    {
                        OverrunPolicy = policy,
                        MaxCatchUpTicks = configuredBound
                    },
                    DateTimeOffset.FromUnixTimeMilliseconds(1_000),
                    2,
                    3,
                    DateTimeOffset.FromUnixTimeMilliseconds(2_000),
                    null)));
        var inventory = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [new ZLinkRelocationParticipantEnvelope(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("spot"),
                ZLinkPlacementObjectKind.UserSpot,
                1,
                1,
                ReadOnlyMemory<byte>.Empty,
                [],
                [logicalTimer])]);

        var canonical = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            inventory,
            "spot",
            nameof(RelocationRuntimeTests),
            RoutingId.From("target"),
            1);
        var transported = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(canonical));
        var transportedTimer = Assert.Single(
            Assert.Single(transported.Participants).LogicalTimers);
        var canonicalTimer = Assert.IsType<ZLinkCanonicalLogicalTimer>(
            transportedTimer.CanonicalTimer);
        var restored = ZLinkSpotTimerRelocationCodec.Decode(
            transportedTimer,
            typeof(RelocationRuntimeTests));

        Assert.Equal((byte)policy, canonicalTimer.OverrunPolicy);
        Assert.Equal((ulong)expectedBound, canonicalTimer.MaxCatchUpTicks);
        Assert.Equal(policy, restored.Timer.Options.OverrunPolicy);
        Assert.Equal(expectedBound, restored.Timer.Options.MaxCatchUpTicks);
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
            "coordinator-owner",
            41,
            RoutingId.From("reply-coordinator").ToHex(),
            43,
            7,
            "reply-root",
            47,
            53);
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
                RecoveryPayload: recovery)]);

        var canonical = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            source, "spot", "SpotType", RoutingId.From("target"), 12);

        Assert.False(canonical.CanonicalLogicalStream.IsEmpty);
        var restored = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(canonical));
        var request = Assert.Single(restored.Participants[0].AcceptedJobs)
            .CanonicalRequest;
        Assert.NotNull(request);
        Assert.Equal("caller-owner", request!.Source.OwnerId);
        Assert.Equal("Ping", request.ApplicationPayload.PacketName);
        Assert.Equal(new byte[] { 1 }, restored.Participants[0]
            .ApplicationState.ToArray());
        Assert.True(restored.Participants[0].RecoveryPayload.IsEmpty);
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

    }

    [Fact]
    public void CanonicalEnvelopeDoesNotSerializePrivateParticipantRecovery()
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

        var canonical = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            source, "spot", "SpotType", RoutingId.From("target"), 12);
        var restored = ZLinkRelocationEnvelopeCodec.Decode(
            ZLinkRelocationEnvelopeCodec.Encode(canonical));

        Assert.True(Assert.Single(restored.Participants).RecoveryPayload.IsEmpty);
    }

    [Fact]
    public void CanonicalSavedWorkRejectsDuplicateOperationWithinExactSourceFence()
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
                [])]);

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

        Assert.Equal(
            (ushort)5,
            BinaryPrimitives.ReadUInt16LittleEndian(encoded.AsSpan(4)));
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

        var obsoleteCounters = new byte[encoded.Length + 3 * sizeof(uint)];
        encoded.AsSpan(0, encoded.Length - sizeof(uint)).CopyTo(obsoleteCounters);
        BinaryPrimitives.WriteUInt32LittleEndian(
            obsoleteCounters.AsSpan(obsoleteCounters.Length - sizeof(uint)),
            ZLinkCrc32C.Compute(obsoleteCounters.AsSpan(
                0,
                obsoleteCounters.Length - sizeof(uint))));
        Assert.False(ZLinkActorRelocationAuthorityPayloadCodec.TryDecode(
            obsoleteCounters,
            out _));

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
    public void LocationOptionsDoNotExposeRelocationSpecificQuotaSettings()
    {
        var names = typeof(ZLinkLocationOptions)
            .GetProperties()
            .Select(static property => property.Name)
            .ToHashSet(StringComparer.Ordinal);

        Assert.DoesNotContain("MaxActiveOutboundRelocations", names);
        Assert.DoesNotContain("MaxActiveInboundRelocations", names);
        Assert.DoesNotContain("MaxConcurrentRelocationCaptures", names);
        Assert.DoesNotContain("MaxConcurrentRelocationRestores", names);
        Assert.DoesNotContain("MaxRelocationPayloadInFlightBytes", names);
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

    }

    [Fact]
    public async Task PublishedTargetBindsFinalRootWithoutSourceCompletion()
    {
        var (stage, candidate) =
            CreateCanonicalPublishedReconciliationFixture();
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
    public async Task CanonicalTargetStagingKeepsSourceCasRecoveryOutsideThePublishedRoot()
    {
        var (stage, candidate) = CreateCanonicalPublishedReconciliationFixture();
        var key = Assert.Single(stage.Envelope.Participants).AuthorityKey;
        var source = candidate.Authorities[0].Snapshot with
        {
            StoreVersion = "source-cas-version",
            Payload = CanonicalFixtureAuthorityPayload(),
            AuthorityOwnerGeneration = stage.SourceAuthorityOwnerGeneration
        };
        var store = new RecordingAuthorityStore();
        store.Seed(key, source);
        var request = new ZLinkCanonicalSpotStageContext(
            stage.Envelope.AggregateId, 1, 1, "mesh",
            stage.SourceNodeRid.ToHex(), 1, "source-owner", 1,
            RoutingId.From("target").ToHex(), 1, "target-owner", 1,
            "room", "Game.Room", false, "", 37, []);

        var (envelope, recoveries) = await ZLinkSpotRetireTargetRuntime
            .BindCanonicalAuthorityInventoryAsync(
                store, candidate.Envelope, request, CancellationToken.None);
        Assert.True(Assert.Single(envelope.Participants).RecoveryPayload.IsEmpty);
        var recovery = ZLinkCanonicalParticipantRecoveryCodec.Decode(recoveries[key].Span);
        Assert.Equal(source.StoreVersion, recovery.ExpectedStoreVersion);
        Assert.Equal(source.AuthorityOwnerGeneration, recovery.AuthorityOwnerGeneration);
        Assert.Equal(source.Payload.ToArray(), recovery.AuthorityPayload.ToArray());
        stage = stage with { Envelope = envelope, SourceRecoveries = recoveries };
        using var services = new ServiceCollection().BuildServiceProvider();
        var target = new ZLinkSpotRetireTargetRuntime(
            services, null!, new ZLinkFrameworkRegistration());
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            target.ReconcilePublishedStageAsync(stage, candidate, cancellation.Token).AsTask());
        Assert.Equal(1, Volatile.Read(ref stage.AuthorityPublished));
        Assert.Equal(("published-root", 37U), stage.GetFinalRoot());
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SourceObservesTargetAuthorityWithItsOwnRootOrAfterNormalization(bool normalized)
    {
        var (stage, candidate) = CreateCanonicalPublishedReconciliationFixture();
        var participant = Assert.Single(stage.Envelope.Participants) with
        {
            RecoveryPayload = CanonicalFixtureRecovery()
        };
        var snapshot = candidate.Authorities[0].Snapshot;
        var descriptor = snapshot.Allocation.Descriptor;
        var owner = new ZLinkLocationOwnerToken(snapshot.OwnerId, snapshot.OwnerLeaseGeneration);
        if (normalized)
            snapshot = snapshot with
            {
                Payload = ZLinkSpotRetireTargetRuntime.BuildTargetReadyPayload(
                    participant.ObjectKind, CanonicalFixtureAuthorityPayload(),
                    descriptor.Rid, 1, owner, stage.Envelope.AggregateId)
            };
        var store = new RecordingAuthorityStore();
        store.Seed(participant.AuthorityKey, snapshot);
        using var services = new ServiceCollection().BuildServiceProvider();
        var target = new ZLinkSpotRetireTargetRuntime(
            services, null!, new ZLinkFrameworkRegistration());
        var sourcePublication = new ZLinkAggregateRelocationPublished(
            new ZLinkAggregateFence(stage.Envelope.AggregateId, 1),
            new ZLinkRelocationStored("source-staging-root", 19, default, DateTimeOffset.UtcNow),
            stage.Envelope with { Participants = [participant] });
        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(1));

        Assert.Equal(stage.TargetAuthorityOwnerGeneration,
            await target.ReconcilePublishedAuthorityAsync(
                store,
                new ZLinkSpotRetireReservation(null!, descriptor, 1,
                    new ZLinkCapacityVector(0, 0, null), owner),
                sourcePublication, cancellation.Token));
        Assert.Single(store.Events);
    }

    [Fact]
    public void CanonicalStagingRejectsAuthorityGenerationOtherThanPreparedTarget()
    {
        var (stage, _) = CreateCanonicalPublishedReconciliationFixture();
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
        var stage = CreateTargetStageForHeldJournal();
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
        var sourceResumed = false;
        await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    null,
                    () => ValueTask.FromException(new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.Unavailable,
                        "committed target rejected abort",
                        ZLinkRetryAdvice.RetryAfterBackoff)),
                    static () => ValueTask.CompletedTask,
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    },
                    static () => ValueTask.CompletedTask)
                .AsTask());
        Assert.False(sourceResumed);

        target.CompleteStage(stage, TargetStageTerminalOutcome.Completed);
        Assert.Equal(0, target.ActiveStageCount);
        Assert.Equal(1, target.TerminalTombstoneCount);
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
        var stage = CreateTargetStageForHeldJournal();
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

        // Published means "queue publication complete": restore, replay,
        // catalog, and the ready callback. Admission cannot open before it.
        Assert.Throws<InvalidOperationException>(() =>
            ZLinkFrameworkRuntime.OpenTargetAdmissionOnce(
                stage,
                () =>
                {
                    opens++;
                    return true;
                }));
        Volatile.Write(ref stage.Published, 1);

        // Queue publication alone is insufficient. Command 44 and application
        // admission both require the exact target authority publication.
        Assert.Equal(0, Volatile.Read(ref stage.AuthorityPublished));
        Assert.Equal(0, Volatile.Read(ref stage.SessionRoutesConverged));
        Assert.Throws<InvalidOperationException>(() =>
            ZLinkFrameworkRuntime.OpenTargetAdmissionOnce(
                stage,
                () => ++opens > 0));
        Volatile.Write(ref stage.AuthorityPublished, 1);
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
    public async Task PublishedStageSurvivesExpiryUntilSessionRoutesConverge()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration();
        registration.Locations.UseInMemoryStores = true;
        var target = new ZLinkSpotRetireTargetRuntime(
            services,
            null!,
            registration);
        var stage = CreateTargetStageForHeldJournal() with
        {
            ExpiresAt = DateTimeOffset.UtcNow - TimeSpan.FromMinutes(1)
        };
        Volatile.Write(ref stage.Published, 1);
        Volatile.Write(ref stage.AuthorityPublished, 1);
        var fence = new ZLinkAggregateFence(
            stage.Envelope.AggregateId,
            stage.Envelope.AggregateGeneration);
        Assert.True(target.TryTrackStage(fence, stage));
        var unknownFence = new ZLinkAggregateFence(Guid.NewGuid(), 1);

        // A published stage with route convergence pending must outlive its
        // expiry so the reconciliation poller can keep re-driving the route
        // commit.
        Assert.False(await target.PublishInboundAsync(
            unknownFence,
            stage.SourceNodeRid,
            CancellationToken.None));
        Assert.Equal(1, target.ActiveStageCount);

        // Once the session routes converged, expiry reconciliation completes
        // the published stage as a Completed tombstone.
        Volatile.Write(ref stage.SessionRoutesConverged, 1);
        Assert.False(await target.PublishInboundAsync(
            unknownFence,
            stage.SourceNodeRid,
            CancellationToken.None));
        Assert.Equal(0, target.ActiveStageCount);
        Assert.Equal(1, target.TerminalTombstoneCount);
    }

    [Fact]
    public void SessionRouteConvergenceIsSingleFlightAndDecoupledFromAdmission()
    {
        var stage = CreateTargetStageForHeldJournal();

        Assert.True(stage.TryBeginSessionRouteConvergence());
        Assert.False(stage.TryBeginSessionRouteConvergence());
        stage.EndSessionRouteConvergence();
        Assert.True(stage.TryBeginSessionRouteConvergence());
        stage.EndSessionRouteConvergence();

        using var services = CreateRuntimeServices();
        var runtime = CreateBareRuntime(services);
        // Route convergence only runs after queue publication.
        runtime.ScheduleRelocationSessionRouteConvergence(stage);
        Assert.Equal(0, Volatile.Read(ref stage.SessionRoutesConverged));

        // Queue publication alone cannot start command 44 before the exact
        // target authority is published.
        Volatile.Write(ref stage.Published, 1);
        runtime.ScheduleRelocationSessionRouteConvergence(stage);
        Assert.Equal(0, Volatile.Read(ref stage.SessionRoutesConverged));

        // A published target without bound-session actors has no route work.
        Volatile.Write(ref stage.AuthorityPublished, 1);
        runtime.ScheduleRelocationSessionRouteConvergence(stage);
        Assert.Equal(1, Volatile.Read(ref stage.SessionRoutesConverged));
    }

    [Fact]
    public async Task SourceCleanupRacingAlreadyOpenAdmissionDoesNotReopenAdmission()
    {
        using var services = CreateRuntimeServices();
        var runtime = CreateBareRuntime(services);
        var stage = CreateTargetStageForHeldJournal();
        Volatile.Write(ref stage.AuthorityPublished, 1);
        Volatile.Write(ref stage.Published, 1);
        Volatile.Write(ref stage.AdmissionOpened, 1);

        // Target publication re-enters admission idempotently: racing an
        // already-open admission never re-opens the seal.
        await runtime.OpenInboundSpotAggregateAdmissionAsync(stage);
        var drain = Assert.IsAssignableFrom<Task>(stage.AdmissionDrainTask);
        await runtime.OpenInboundSpotAggregateAdmissionAsync(stage);

        Assert.Same(drain, stage.AdmissionDrainTask);
        Assert.Equal(1, Volatile.Read(ref stage.AdmissionOpened));
    }

    private static ServiceProvider CreateRuntimeServices()
    {
        var services = new ServiceCollection();
        var registration = new ZLinkFrameworkRegistration();
        services.AddSingleton(registration);
        return services.BuildServiceProvider();
    }

    private static ZLinkFrameworkRuntime CreateBareRuntime(
        ServiceProvider services)
    {
        var registration =
            services.GetRequiredService<ZLinkFrameworkRegistration>();
        return new ZLinkFrameworkRuntime(
            services,
            new ZLinkDotNetBackendAdapterFactory(),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
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
        // The spec bounds completed-stage retention by time (up to 5 minutes,
        // 21-location-runtime), not by a fixed tombstone count. With no time
        // elapsed every completed stage is still retained as a terminal
        // tombstone, so all 2048 are present until RemoveExpiredTombstones runs.
        Assert.Equal(2_048, target.TerminalTombstoneCount);

        target.RemoveExpiredTombstones(
            DateTimeOffset.UtcNow
            + ZLinkSpotRetireTargetRuntime.TombstoneRetention
            + TimeSpan.FromSeconds(1));

        Assert.Equal(0, target.TerminalTombstoneCount);
    }

    [Fact]
    public void HeldIngressRequiresStrictSequenceWithoutRelocationSpecificCapacity()
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

        var moreThanFormerCountLimit = Enumerable.Range(1, 1_025)
            .Select(static index => new ZLinkSpotRetireHeldRecord(
                checked((ulong)index),
                []))
            .ToArray();
        ZLinkSpotRetireTargetRuntime.ValidateHeldRecords(
            moreThanFormerCountLimit);

        var sharedPayload = new byte[(1024 * 1024) + 1];
        var moreThanFormerByteLimit = Enumerable.Range(1, 17)
            .Select(index => new ZLinkSpotRetireHeldRecord(
                checked((ulong)index),
                sharedPayload))
            .ToArray();
        Assert.True(
            moreThanFormerByteLimit.Sum(static record =>
                record.Payload.LongLength)
            > 16L * 1024 * 1024);
        ZLinkSpotRetireTargetRuntime.ValidateHeldRecords(
            moreThanFormerByteLimit);
    }

    [Fact]
    public void HeldIngressBeyondFormerCodecLimitRoundTripsAndRestores()
    {
        const int recordCount = 65_537;
        var frozenRecord = CreateMinimalCanonicalFrozenRecord();
        Assert.True(ZLinkRelocationEnvelopeCodec
            .TryValidateCanonicalFrozenRecord(frozenRecord));
        var accepted = Enumerable.Range(1, recordCount)
            .Select(index => new ZLinkRelocationQueuedJob(
                checked((ulong)index),
                frozenRecord))
            .ToArray();
        var inventory = new ZLinkRelocationEnvelope(
            Guid.NewGuid(),
            1,
            new byte[32],
            [
                new ZLinkRelocationParticipantEnvelope(
                    new ZLinkAuthorityKey("spot:room"),
                    ZLinkPlacementObjectKind.UserSpot,
                    1,
                    1,
                    ReadOnlyMemory<byte>.Empty,
                    [],
                    []),
                new ZLinkRelocationParticipantEnvelope(
                    new ZLinkAuthorityKey("actor:room:held"),
                    ZLinkPlacementObjectKind.Actor,
                    1,
                    1,
                    ReadOnlyMemory<byte>.Empty,
                    accepted,
                    [])
            ]);

        var legacyEncoded = ZLinkRelocationEnvelopeCodec.Encode(inventory);
        var legacyDecoded = ZLinkRelocationEnvelopeCodec.Decode(legacyEncoded);
        Assert.Equal(recordCount, legacyDecoded.Participants[1].AcceptedJobs.Count);

        var canonical = ZLinkCanonicalSpotRelocationWriter.CreateInitial(
            inventory,
            "room",
            nameof(RelocationRuntimeTests),
            RoutingId.From("target"),
            1);
        var canonicalEncoded = ZLinkRelocationEnvelopeCodec.Encode(canonical);
        var canonicalDecoded = ZLinkRelocationEnvelopeCodec.Decode(
            canonicalEncoded);
        var restoredAccepted = canonicalDecoded.Participants[1].AcceptedJobs;
        Assert.Equal(recordCount, restoredAccepted.Count);
        Assert.Equal<ulong>(1, restoredAccepted[0].AcceptedSequence);
        Assert.Equal<ulong>(recordCount, restoredAccepted[^1].AcceptedSequence);

        var held = restoredAccepted.Select(static job =>
                new ZLinkSpotRetireHeldRecord(
                    job.AcceptedSequence,
                    job.Payload.ToArray()))
            .ToArray();
        var stage = CreateTargetStageForHeldJournal();
        ZLinkSpotRetireTargetRuntime.ValidateHeldRecords(held);
        Assert.True(ZLinkSpotRetireTargetRuntime.TrySetHeldRecords(stage, held));
        Assert.Equal(recordCount, stage.HeldRecords.Count);

        var impossibleLegacyCount = ZLinkRelocationEnvelopeCodec.Encode(
            inventory with
            {
                Participants = [inventory.Participants[0]]
            });
        var acceptedCountOffset = sizeof(uint) + sizeof(ushort) + 16
                                  + sizeof(ulong) + sizeof(int) + 32
                                  + sizeof(int);
        var keyLength = BinaryPrimitives.ReadUInt16LittleEndian(
            impossibleLegacyCount.AsSpan(acceptedCountOffset));
        acceptedCountOffset += sizeof(ushort) + keyLength
                               + sizeof(byte) + 2 * sizeof(ulong);
        var stateLength = BinaryPrimitives.ReadInt32LittleEndian(
            impossibleLegacyCount.AsSpan(acceptedCountOffset));
        acceptedCountOffset += sizeof(int) + stateLength;
        BinaryPrimitives.WriteInt32LittleEndian(
            impossibleLegacyCount.AsSpan(acceptedCountOffset),
            int.MaxValue);
        Assert.Throws<InvalidDataException>(() =>
            ZLinkRelocationEnvelopeCodec.Decode(impossibleLegacyCount));
        using var impossibleLegacyStream = new MemoryStream(
            impossibleLegacyCount,
            writable: false);
        Assert.Throws<InvalidDataException>(() =>
            ZLinkRelocationEnvelopeCodec.Decode(impossibleLegacyStream));

    }

    [Fact]
    public async Task PrecommitAbortRestoresSourceBeforeRouteUnseal()
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
                events.Add("target-cleaned");
                return ValueTask.CompletedTask;
            },
            () =>
            {
                events.Add("staging-discarded");
                return ValueTask.CompletedTask;
            },
            () =>
            {
                events.Add("source-resumed");
                return ValueTask.CompletedTask;
            },
            () =>
            {
                events.Add("routes-unsealed");
                return ValueTask.CompletedTask;
            });

        Assert.Equal(
            [
                "durable-aborted",
                "target-cleaned",
                "staging-discarded",
                "source-resumed",
                "routes-unsealed"
            ],
            events);
    }

    [Fact]
    public async Task PrecommitAbortCrashBeforeDurableAckKeepsSourceSealed()
    {
        var targetCleaned = false;
        var sourceResumed = false;

        await Assert.ThrowsAsync<IOException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    () => new ValueTask(Task.FromException(
                        new IOException("durable abort outcome unknown"))),
                    () =>
                    {
                        targetCleaned = true;
                        return ValueTask.CompletedTask;
                    },
                    static () => ValueTask.CompletedTask,
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    },
                    static () => ValueTask.CompletedTask)
                .AsTask());

        Assert.False(targetCleaned);
        Assert.False(sourceResumed);
    }

    [Fact]
    public async Task PrecommitAbortTargetCleanupNackKeepsSourceSealed()
    {
        var stagingDiscarded = false;
        var sourceResumed = false;
        await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    static () => ValueTask.CompletedTask,
                    () => new ValueTask(Task.FromException(
                        new ZLinkFrameworkException(
                            ZLinkFrameworkErrorKind.Unavailable,
                            "target cleanup NACK",
                            ZLinkRetryAdvice.RetryAfterBackoff))),
                    () =>
                    {
                        stagingDiscarded = true;
                        return ValueTask.CompletedTask;
                    },
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    },
                    static () => ValueTask.CompletedTask)
                .AsTask());
        Assert.False(stagingDiscarded);
        Assert.False(sourceResumed);
    }

    [Fact]
    public async Task PrecommitAbortTargetCleanupTimeoutKeepsSourceSealed()
    {
        var sourceResumed = false;
        await Assert.ThrowsAsync<TimeoutException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    static () => ValueTask.CompletedTask,
                    () => new ValueTask(Task.FromException(
                        new TimeoutException("target cleanup timeout"))),
                    static () => ValueTask.CompletedTask,
                    () =>
                    {
                        sourceResumed = true;
                        return ValueTask.CompletedTask;
                    },
                    static () => ValueTask.CompletedTask)
                .AsTask());
        Assert.False(sourceResumed);
    }

    [Fact]
    public async Task PrecommitAbortRouteUnsealFailureDoesNotGateSourceRestore()
    {
        var targetCleaned = false;
        var sourceResumed = false;
        var unsealAttempts = 0;

        await ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
            static () => ValueTask.CompletedTask,
            () =>
            {
                targetCleaned = true;
                return ValueTask.CompletedTask;
            },
            static () => ValueTask.CompletedTask,
            () =>
            {
                sourceResumed = true;
                return ValueTask.CompletedTask;
            },
            () =>
            {
                unsealAttempts++;
                return ValueTask.FromException(
                    new ZLinkRelocationDataLostException(
                        "session route seal was not restored"));
            });

        Assert.True(targetCleaned);
        Assert.True(sourceResumed);
        Assert.Equal(3, unsealAttempts);
    }

    [Fact]
    public async Task PrecommitAbortWithUnresponsiveSessionOwnerStillRestoresSource()
    {
        // The session owner never acknowledges: every re-send times out. The
        // abort must complete with source admission already restored and a
        // bounded number of unseal attempts.
        var order = new List<string>();

        await ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
            null,
            static () => ValueTask.CompletedTask,
            static () => ValueTask.CompletedTask,
            () =>
            {
                order.Add("source-resumed");
                return ValueTask.CompletedTask;
            },
            () =>
            {
                order.Add("unseal-attempt");
                return ValueTask.FromException(
                    new TimeoutException("session owner never replied"));
            });

        Assert.Equal(
            [
                "source-resumed",
                "unseal-attempt",
                "unseal-attempt",
                "unseal-attempt"
            ],
            order);
    }

    [Fact]
    public async Task PrecommitAbortSourceSealRestoreFailureIsNotSuccess()
    {
        var routesUnsealed = false;
        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            () => ZLinkSpotRetireScheduler.ExecutePrecommitAbortAsync(
                    static () => ValueTask.CompletedTask,
                    static () => ValueTask.CompletedTask,
                    static () => ValueTask.CompletedTask,
                    () => new ValueTask(Task.FromException(
                        new ZLinkRelocationDataLostException(
                            "source seal token mismatch"))),
                    () =>
                    {
                        routesUnsealed = true;
                        return ValueTask.CompletedTask;
                    })
                .AsTask());
        Assert.False(routesUnsealed);
    }

    [Fact]
    public void RecoveredCanonicalInventoryBindsTakeoverBumpedRootGenerations()
    {
        var (_, candidate) = CreateCanonicalPublishedReconciliationFixture();
        // Durable roots written by earlier builds may carry takeover-bumped
        // aggregate generations. The read side must keep binding them to the
        // authority publication (the generation of record) instead of failing
        // an in-flight relocation with RelocationDataLost across the upgrade.
        var bumped = candidate with
        {
            Envelope = candidate.Envelope with { AggregateGeneration = 3 }
        };

        var bound = ZLinkSpotRetireTargetRuntime
            .BindRecoveredCanonicalInventory(bumped);

        Assert.Equal(
            candidate.Envelope.AggregateGeneration,
            bound.Envelope.AggregateGeneration);
    }

    [Fact]
    public void SpotMessageFollowDoesNotApplyFormerMessageAndByteBounds()
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
        for (var index = 0; index < 1_025; index++)
        {
            Assert.True(messages.TryAcquire(0, out var lease));
            messageLeases.Add(lease!);
        }
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
        Assert.True(bytes.TryAcquire(1, out var nextByteLease));
        byteLease!.Dispose();
        nextByteLease!.Dispose();
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
    public void SpotMessageFollowCountsWireBytesAtConfiguredAdmissionBoundary()
    {
        const long byteCapacity = 16L * 1024 * 1024;
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
            DateTimeOffset.UtcNow.AddSeconds(30),
            new ZLinkBoundedIngressAdmission(8, byteCapacity));
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
            byteCapacity - fixedBytes)));
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

        Assert.Equal(byteCapacity, encodedBytes);
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
        ZLinkBackendRequestCallback? callback = null;
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
            "mesh",
            validateFlow: true);

        Assert.NotNull(replyHeader);
        Assert.Equal(
            "invalid_operation",
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
    public async Task Authority_relocation_moves_owner_and_active_allocation_in_one_cas()
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
            AuthorityDescriptor("target", target.Token, placementWeight: 0),
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
            await store.GetPlacementCapacityUsageAsync(
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("source")),
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));

        Assert.Equal(
            (0L, 0L),
            await store.GetPlacementCapacityUsageAsync(
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
                    null)));
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
                    committed.Snapshot.Allocation with
                    {
                        Descriptor = new ZLinkMeshNodeDescriptorKey(
                            "mesh",
                            RoutingId.From("target")),
                        DescriptorLifecycleGeneration = 1
                    },
                    prepared.Snapshot.AuthorityOwnerGeneration + 1)));

        Assert.Equal(opaque, moved.Snapshot.Payload.ToArray());
        Assert.Equal(target.Token.OwnerId, moved.Snapshot.OwnerId);
        Assert.Equal(
            target.Token.LeaseGeneration,
            moved.Snapshot.OwnerLeaseGeneration);
        Assert.Equal(
            prepared.Snapshot.AuthorityOwnerGeneration + 1,
            moved.Snapshot.AuthorityOwnerGeneration);
        Assert.Equal(
            (0L, 0L),
            await store.GetPlacementCapacityUsageAsync(
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("source")),
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
        Assert.Equal(
            (0L, 1L),
            await store.GetPlacementCapacityUsageAsync(
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
            await store.GetPlacementCapacityUsageAsync(
                descriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
        Assert.IsType<ZLinkObjectAbortResult.Aborted>(
            await store.AbortAsync(abortedReservation.Reservation));
        Assert.Equal(
            (0L, 0L),
            await store.GetPlacementCapacityUsageAsync(
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
            await store.GetPlacementCapacityUsageAsync(
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
            await store.GetPlacementCapacityUsageAsync(
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
            await store.GetPlacementCapacityUsageAsync(
                sourceDescriptor,
                1,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor"));
        Assert.Equal(
            (0L, 1L),
            await store.GetPlacementCapacityUsageAsync(
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
            await store.GetPlacementCapacityUsageAsync(
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

        Assert.Same(
            relocation,
            registration.Locations.RelocationStoreInstance);
        Assert.IsType<ZLinkProviderRelocationRepository>(
            registration.Locations.ResolveRelocationStore());
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
            1,
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
            1,
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
            1,
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
            async () => await coordinator.PublishAsync(request));

        Assert.InRange(authority.MaximumConcurrentReads, 2, 64);
        Assert.Contains(
            authority.Events,
            static item => item.Name == "abort");
        Assert.Contains(
            relocation.Events,
            static item => item.Name == "delete");
    }

    [Fact]
    public async Task AggregatePrepareResponseLossReconcilesPublishedAggregate()
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

        var published = await coordinator.PublishAsync(request);

        Assert.Equal(request.AggregateId, published.Fence.AggregateId);
        Assert.Equal(request.Participants.Count, authority.PublishedCount);
    }

    [Fact]
    public async Task ConcurrentCommitDuringPrepareProbeReconcilesPublication()
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

        var published = await coordinator.PublishAsync(request);

        Assert.Equal(request.AggregateId, published.Fence.AggregateId);
        Assert.Equal(request.Participants.Count, authority.PublishedCount);
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
            1,
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
            async () => await coordinator.PublishAsync(request));

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
            1,
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
            async () => await coordinator.PublishAsync(request));

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
    public async Task AggregateTargetPublishReusesAcceptedOpaqueRoot()
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
            1,
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

        var published = await coordinator.PublishAsync(
            request,
            CancellationToken.None,
            accepted);

        Assert.Equal(accepted.Reference, published.Relocation.Reference);
        Assert.Equal(
            accepted.ChecksumCrc32c,
            published.Relocation.ChecksumCrc32c);
        Assert.Equal(
            putsBeforeTargetPrepare,
            relocation.Events.Count(static item => item.Name == "put"));
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
            1,
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
            () => coordinator.PublishAsync(
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
        await Assert.ThrowsAsync<ZLinkAuthorityGenerationExhaustedException>(
            () => coordinator.PublishAsync(
                    CreateAggregateRelocationRequest(CreateEnvelope()))
                .AsTask());

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

    private static byte[] CreateMinimalCanonicalFrozenRecord()
    {
        using var source = new MemoryStream();
        WriteText8(source, "n");
        WriteUInt64(source, 1);
        WriteText8(source, "o");
        WriteUInt64(source, 1);

        using var payload = new MemoryStream();
        WriteText8(payload, "p");
        WriteText8(payload, "application/octet-stream");
        WriteUInt32(payload, 0);

        using var record = new MemoryStream();
        record.WriteByte(1);
        record.WriteByte(1);
        WriteUInt16(record, checked((ushort)source.Length));
        source.Position = 0;
        source.CopyTo(record);
        record.WriteByte(0);
        WriteUInt64(record, 0);
        WriteUInt64(record, 0);
        WriteUInt32(record, 0);
        WriteUInt16(record, 0);
        record.WriteByte(1);
        WriteUInt32(record, checked((uint)payload.Length));
        payload.Position = 0;
        payload.CopyTo(record);
        return record.ToArray();

        static void WriteText8(Stream stream, string value)
        {
            var bytes = Encoding.UTF8.GetBytes(value);
            stream.WriteByte(checked((byte)bytes.Length));
            stream.Write(bytes);
        }

        static void WriteUInt16(Stream stream, ushort value)
        {
            Span<byte> bytes = stackalloc byte[sizeof(ushort)];
            BinaryPrimitives.WriteUInt16BigEndian(bytes, value);
            stream.Write(bytes);
        }

        static void WriteUInt32(Stream stream, uint value)
        {
            Span<byte> bytes = stackalloc byte[sizeof(uint)];
            BinaryPrimitives.WriteUInt32BigEndian(bytes, value);
            stream.Write(bytes);
        }

        static void WriteUInt64(Stream stream, ulong value)
        {
            Span<byte> bytes = stackalloc byte[sizeof(ulong)];
            BinaryPrimitives.WriteUInt64BigEndian(bytes, value);
            stream.Write(bytes);
        }
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
            "target-owner",
            9,
            new byte[] { 8, 8 },
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
        int? pendingLimit = null,
        int placementWeight = 100) =>
        new(
            "mesh",
            RoutingId.From(rid),
            LifecycleGeneration: 1,
            DescriptorRevision: 1,
            $"inproc://{rid}",
            new Dictionary<string, int>(StringComparer.Ordinal)
            {
                ["mesh"] = placementWeight
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
        // Native context startup can be serialized behind other aggregate
        // tests. Keep the admission assertion bounded, but use the same
        // 30-second aggregate startup budget as the other runtime fixtures.
        var deadline = Stopwatch.GetTimestamp()
                       + (long)(Stopwatch.Frequency * 30);
        while (!predicate())
        {
            if (Stopwatch.GetTimestamp() >= deadline)
                throw new TimeoutException();
            await Task.Delay(10);
        }
    }

    private sealed class RecordingRelocationStore :
        IZLinkRelocationRepository,
        IZLinkRelocationStore
    {
        internal Dictionary<string, byte[]> Payloads { get; } =
            new(StringComparer.Ordinal);

        internal List<(long Sequence, string Name)> Events { get; } = [];

        internal void Seed(string reference, byte[] payload) =>
            Payloads.Add(reference, payload);

        public ValueTask<ZLinkBlobPutResult> PutAsync(
            ZLinkBlobReference reference,
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var now = DateTimeOffset.UtcNow;
            var expiresAt = now + retention;
            Events.Add((EventClock.Next(), "put"));
            if (Payloads.TryGetValue(reference.Value, out var current))
            {
                return ValueTask.FromResult<ZLinkBlobPutResult>(
                    current.AsSpan().SequenceEqual(payload.Span)
                        ? new ZLinkBlobPutResult.AlreadyStored(expiresAt, now)
                        : new ZLinkBlobPutResult.Conflict(now));
            }
            Payloads.Add(reference.Value, payload.ToArray());
            return ValueTask.FromResult<ZLinkBlobPutResult>(
                new ZLinkBlobPutResult.Stored(expiresAt, now));
        }

        public ValueTask<ZLinkBlobReadResult> ReadAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Events.Add((EventClock.Next(), "get"));
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkBlobReadResult>(
                Payloads.TryGetValue(reference.Value, out var payload)
                    ? new ZLinkBlobReadResult.Found(
                        payload,
                        now + TimeSpan.FromHours(24),
                        now)
                    : new ZLinkBlobReadResult.Missing(now));
        }

        public ValueTask<ZLinkBlobRenewResult> RenewAsync(
            ZLinkBlobReference reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var now = DateTimeOffset.UtcNow;
            return ValueTask.FromResult<ZLinkBlobRenewResult>(
                Payloads.ContainsKey(reference.Value)
                    ? new ZLinkBlobRenewResult.Renewed(
                        now + retention,
                        now)
                    : new ZLinkBlobRenewResult.Missing(now));
        }

        public ValueTask DeleteAsync(
            ZLinkBlobReference reference,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            Events.Add((EventClock.Next(), "delete"));
            Payloads.Remove(reference.Value);
            return ValueTask.CompletedTask;
        }

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

        internal void Seed(ZLinkAuthorityKey key, ZLinkAuthoritySnapshot snapshot) =>
            _snapshots[key.Value] = snapshot;

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
        1,
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
            string.Empty,
            "staging-root",
            1,
            DateTimeOffset.MaxValue,
            [],
            null!,
            new Dictionary<ZLinkActorId, ulong>(),
            1,
            2,
            "mesh",
            1,
            1,
            1);

    private static (
        TargetStage Stage,
        ZLinkRelocationRecoveryCandidate Candidate)
        CreateCanonicalPublishedReconciliationFixture()
    {
        var sourceRid = RoutingId.From("source");
        var targetRid = RoutingId.From("target");
        var key = ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("room");
        var authorityPayload = CanonicalFixtureAuthorityPayload();
        var participant = new ZLinkRelocationParticipantEnvelope(
            key,
            ZLinkPlacementObjectKind.UserSpot,
            5,
            11,
            new byte[] { 1 },
            [],
            []);
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
                    AuthorityOwnerGeneration = 11
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
                        "target-owner",
                        1,
                        targetRid.ToHex(),
                        1,
                        5,
                        "published-root",
                        37,
                        canonical.CanonicalApplicationVersion)
                    {
                        AggregateGeneration = decoded.AggregateGeneration
                    },
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

    private static byte[] CanonicalFixtureAuthorityPayload() =>
        ZLinkUserSpotAuthorityPayloadCodec.Encode(
            new ZLinkUserSpotAuthorityPayload(
                ZLinkUserSpotAuthorityState.Ready,
                "Game.Room",
                "room",
                "source-owner",
                1,
                "mesh",
                RoutingId.From("source"),
                1));

    private static byte[] CanonicalFixtureRecovery() =>
        ZLinkCanonicalParticipantRecoveryCodec.Encode(
            new ZLinkCanonicalParticipantRecovery(
                ZLinkUserSpotAuthorityPayloadCodec.AuthorityKey("room"),
                ZLinkPlacementObjectKind.UserSpot,
                5,
                11,
                "v-source",
                "Game.Room",
                CanonicalFixtureAuthorityPayload(),
                ReadOnlyMemory<byte>.Empty));

}
