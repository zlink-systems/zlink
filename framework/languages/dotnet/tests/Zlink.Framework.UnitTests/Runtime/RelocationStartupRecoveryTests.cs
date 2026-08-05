using System.Buffers.Binary;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class RelocationStartupRecoveryTests
{
    [Theory]
    [InlineData((byte)2)]
    [InlineData((byte)3)]
    public async Task StandaloneActorSourceOwnedPrecommitRootIsRecovered(
        byte phase)
    {
        var relocation = new RecoveryRelocationStore();
        var canonical = CreateCanonicalActorRoot();
        var stored = await ZLinkRelocationTreeStore.PutAsync(
            relocation,
            canonical.Envelope,
            TimeSpan.FromHours(24),
            CancellationToken.None);
        var state = PrecommitState(
            canonical.Envelope.AggregateId,
            phase,
            stored.Root.Reference,
            stored.Root.ChecksumCrc32c);
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                canonical.SteadyPayload,
                state,
                canonical.Envelope);
        var authority = new RecoveryAuthorityStore(
        [
            CanonicalEntry(canonical.Participant, payload)
        ]);
        ZLinkRelocationRecoveryCandidate? recovered = null;

        await new ZLinkRelocationStartupRecovery(authority, relocation)
            .RecoverAsync((candidate, _) =>
            {
                recovered = candidate;
                return ValueTask.CompletedTask;
            });

        Assert.NotNull(recovered);
        Assert.Equal(canonical.Envelope.AggregateId, recovered.Envelope.AggregateId);
        Assert.Equal("source-owner", recovered.Authorities.Single().Snapshot.OwnerId);
        Assert.True(ZLinkCanonicalRelocationAuthorityStateCodec.TryRead(
            recovered.Authorities.Single().Snapshot.Payload.Span,
            out var projection));
        Assert.Equal(phase, projection.Phase);
    }

    [Fact]
    public async Task StandaloneActorPreparingWithoutRootIsNotPublishedRecovery()
    {
        var canonical = CreateCanonicalActorRoot();
        var payload = ZLinkCanonicalRelocationAuthorityStateCodec
            .ReplaceRelocationState(
                canonical.SteadyPayload,
                PrecommitState(
                    canonical.Envelope.AggregateId,
                    phase: 1,
                    reference: string.Empty,
                    checksum: 0),
                root: null);
        var authority = new RecoveryAuthorityStore(
        [
            CanonicalEntry(canonical.Participant, payload)
        ]);
        var recovery = new ZLinkRelocationStartupRecovery(
            authority,
            new RecoveryRelocationStore());
        var offered = 0;
        ZLinkAuthorityEntry? preparing = null;

        await recovery.RecoverAsync(
            (_, _) =>
            {
                offered++;
                return ValueTask.CompletedTask;
            },
            (entry, _) =>
            {
                preparing = entry;
                return ValueTask.CompletedTask;
            });

        await recovery.RecoverAsync((_, _) =>
        {
            offered++;
            return ValueTask.CompletedTask;
        });
        var exact = await recovery.TryReadExactPublishedAsync(
            canonical.Envelope);

        Assert.Equal(0, offered);
        Assert.NotNull(preparing);
        Assert.Equal(canonical.Participant.AuthorityKey, preparing.Key);
        Assert.Null(exact);
    }

    [Fact]
    public async Task ActorAndUserSpotAuthoritiesResumeOneAggregatePerScan()
    {
        var fixture = await RecoveryFixture.CreateAsync();
        var offered = 0;
        var applied = new HashSet<Guid>();
        var recovery = new ZLinkRelocationStartupRecovery(
            fixture.Authority,
            fixture.Relocation);

        async ValueTask Resume(
            ZLinkRelocationRecoveryCandidate candidate,
            CancellationToken cancellationToken)
        {
            await Task.Yield();
            cancellationToken.ThrowIfCancellationRequested();
            offered++;
            applied.Add(candidate.Envelope.AggregateId);
            Assert.Equal(2, candidate.Authorities.Count);
            Assert.Equal(
                candidate.Envelope.AggregateId,
                candidate.Reference.AggregateId);
        }

        await recovery.RecoverAsync(Resume);
        await recovery.RecoverAsync(Resume); // process restart/repeated scan

        Assert.Equal(2, offered);
        Assert.Single(applied);
    }

    [Fact]
    public async Task InstanceSpotAuthorityUsesTheSharedSpotPrefixAndIsRecovered()
    {
        var fixture = await RecoveryFixture.CreateAsync(
            ZLinkPlacementObjectKind.InstanceSpot);
        ZLinkRelocationRecoveryCandidate? recovered = null;

        await new ZLinkRelocationStartupRecovery(
                fixture.Authority,
                fixture.Relocation)
            .RecoverAsync(
                (candidate, _) =>
                {
                    recovered = candidate;
                    return ValueTask.CompletedTask;
                });

        Assert.NotNull(recovered);
        Assert.Contains(
            recovered.Authorities,
            static authority =>
                authority.Snapshot.Allocation.ObjectKind
                == ZLinkPlacementObjectKind.InstanceSpot);
    }

    [Fact]
    public async Task ExactReconciliationReadsOnlyStagedParticipantAuthorities()
    {
        var fixture = await RecoveryFixture.CreateAsync();
        var recovery = new ZLinkRelocationStartupRecovery(
            fixture.Authority,
            fixture.Relocation);

        var attempts = await Task.WhenAll(
            recovery.TryReadExactPublishedAsync(fixture.Envelope)
                .AsTask(),
            recovery.TryReadExactPublishedAsync(fixture.Envelope)
                .AsTask());

        Assert.All(attempts, static candidate => Assert.NotNull(candidate));
        Assert.Equal(
            fixture.Envelope.Participants.Count * 2,
            fixture.Authority.ReadCalls.Count);
        Assert.Equal(0, fixture.Authority.ScanCalls);
    }

    [Fact]
    public async Task ExactReconciliationRejectsPartialPublication()
    {
        var fixture = await RecoveryFixture.CreateAsync();
        var entries = fixture.Authority.Entries
            .Select((entry, index) => index == 0
                ? entry
                : entry with
                {
                    Snapshot = entry.Snapshot with
                    {
                        Payload = new byte[] { 1, 2, 3 }
                    }
                })
            .ToArray();
        var recovery = new ZLinkRelocationStartupRecovery(
            new RecoveryAuthorityStore(entries),
            fixture.Relocation);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => recovery.TryReadExactPublishedAsync(fixture.Envelope)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.DataLost, error.Kind);
        Assert.False(error.RetryAdvice != ZLinkRetryAdvice.DoNotRetry);
    }

    [Fact]
    public async Task PublishedMissingRootFailsAsNonRetriableRelocationDataLost()
    {
        var fixture = await RecoveryFixture.CreateAsync();
        fixture.Relocation.Remove(fixture.Reference);
        var recovery = new ZLinkRelocationStartupRecovery(
            fixture.Authority,
            fixture.Relocation);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            async () => await recovery.RecoverAsync(
                static (_, _) => ValueTask.CompletedTask));

        Assert.Equal(ZLinkFrameworkErrorKind.DataLost, error.Kind);
        Assert.False(error.RetryAdvice != ZLinkRetryAdvice.DoNotRetry);
    }

    [Fact]
    public async Task PreparedRootCanBeReadByManifestBeforeAuthorityPublication()
    {
        var authority = new RecoveryAuthorityStore([]);
        var relocation = new RecoveryRelocationStore();
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            authority,
            relocation);
        var envelope = RecoveryFixture.CreateEnvelope();

        var prepared = await coordinator.PrepareAsync(envelope);
        var restored = await coordinator.ReadPreparedAsync(prepared.Reference);

        Assert.Empty(authority.CompareExchangeCalls);
        Assert.Equal(envelope.AggregateId, restored.AggregateId);
        Assert.Equal(
            envelope.Participants.Select(static item => item.AuthorityKey),
            restored.Participants.Select(static item => item.AuthorityKey));
    }

    [Fact]
    public async Task ExactPublishedConflictReconcilesWithoutDeletingRoot()
    {
        var fixture = await RecoveryFixture.CreateAsync();
        var envelope = RecoveryFixture.CreateEnvelope();
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            fixture.Authority,
            fixture.Relocation);
        var prepared = await coordinator.PrepareAsync(envelope);
        var actor = envelope.Participants[0];

        var published = await coordinator.PublishPreparedAsync(
            new ZLinkRelocationPublicationRequest(
                actor.AuthorityKey,
                $"v-{actor.ObjectGeneration}",
                ZLinkAuthorityGenerationTransition.Preserve,
                "target-owner",
                7,
                new byte[] { 9 },
                null,
                envelope),
            prepared);

        Assert.Equal(fixture.Reference, published.Relocation.Reference);
        Assert.True(fixture.Relocation.Contains(fixture.Reference));
        Assert.Single(fixture.Authority.CompareExchangeCalls);
    }

    private sealed record RecoveryFixture(
        RecoveryAuthorityStore Authority,
        RecoveryRelocationStore Relocation,
        string Reference,
        ZLinkRelocationEnvelope Envelope)
    {
        internal static async ValueTask<RecoveryFixture> CreateAsync(
            ZLinkPlacementObjectKind spotKind =
                ZLinkPlacementObjectKind.UserSpot)
        {
            var relocation = new RecoveryRelocationStore();
            var envelope = CreateEnvelope(spotKind);
            var coordinator = new ZLinkRelocationPublicationCoordinator(
                new RecoveryAuthorityStore([]),
                relocation);
            var prepared = await coordinator.PrepareAsync(envelope);
            var publication = new ZLinkRelocationAuthorityPayload(
                prepared.Relocation.Reference,
                prepared.Relocation.ChecksumCrc32c,
                envelope.AggregateId,
                envelope.AggregateGeneration,
                envelope.InventoryDigest,
                "target-owner",
                7,
                new byte[] { 9 });
            var payload = ZLinkRelocationAuthorityPayloadCodec.Encode(publication);
            var entries = envelope.Participants.Select(
                    participant => Entry(participant, payload))
                .ToArray();
            return new RecoveryFixture(
                new RecoveryAuthorityStore(entries),
                relocation,
                prepared.Relocation.Reference,
                envelope);
        }

        internal static ZLinkRelocationEnvelope CreateEnvelope(
            ZLinkPlacementObjectKind spotKind =
                ZLinkPlacementObjectKind.UserSpot)
        {
            var actor = new ZLinkAuthorityKey("zla1:a:7:actor-1");
            var spot = new ZLinkAuthorityKey("zla1:s:6:spot-1");
            return new ZLinkRelocationEnvelope(
                Guid.Parse("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"),
                4,
                Enumerable.Repeat((byte)0x2a, 32).ToArray(),
                [
                    new ZLinkRelocationParticipantEnvelope(
                        actor,
                        ZLinkPlacementObjectKind.Actor,
                        11,
                        3,
                        new byte[] { 1 },
                        [new ZLinkRelocationQueuedJob(1, new byte[] { 2 })],
                        []),
                    new ZLinkRelocationParticipantEnvelope(
                        spot,
                        spotKind,
                        12,
                        5,
                        new byte[] { 3 },
                        [],
                        [])
                ]);
        }

        private static ZLinkAuthorityEntry Entry(
            ZLinkRelocationParticipantEnvelope participant,
            ReadOnlyMemory<byte> payload) =>
            new(
                participant.AuthorityKey,
                new ZLinkAuthoritySnapshot(
                    $"v-{participant.ObjectGeneration}",
                    payload,
                    participant.ObjectGeneration,
                    participant.AuthorityOwnerGeneration + 1,
                    "target-owner",
                    7,
                    new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.Active,
                        participant.ObjectKind,
                        participant.ObjectKind == ZLinkPlacementObjectKind.Actor
                            ? "Game.Actor"
                            : "Game.Room",
                        new ZLinkMeshNodeDescriptorKey(
                            "mesh",
                            RoutingId.From("target")),
                        2,
                        participant.ObjectKind == ZLinkPlacementObjectKind.Actor
                            ? new ZLinkCapacityVector(1, 0, null)
                            : new ZLinkCapacityVector(
                                0,
                                1,
                                new ZLinkSpotTypeCapacityDelta(
                                    participant.ObjectKind,
                                    "Game.Room",
                                    1))),
                    null,
                    DateTimeOffset.UnixEpoch));
    }

    private static (
        ZLinkRelocationEnvelope Envelope,
        ZLinkRelocationParticipantEnvelope Participant,
        byte[] SteadyPayload) CreateCanonicalActorRoot()
    {
        var relocationId = Guid.NewGuid();
        var participant = new ZLinkRelocationParticipantEnvelope(
            new ZLinkAuthorityKey($"zla1:a:recovery-{relocationId:N}"),
            ZLinkPlacementObjectKind.Actor,
            11,
            3,
            new byte[] { 1 },
            [],
            [])
        {
            CanonicalParticipantId = 1
        };
        var inventory = new ZLinkRelocationEnvelope(
            relocationId,
            1,
            Enumerable.Repeat((byte)0x42, 32).ToArray(),
            [participant]);
        var envelope = ZLinkCanonicalActorRelocationWriter.CreateInitial(
            inventory,
            applicationVersion: 7);
        var sourceRid = RoutingId.From("startup-source");
        var steady = ZLinkActorAuthorityPayloadCodec.Encode(
            new ZLinkActorAuthorityPayload(
                ZLinkActorAuthorityState.Ready,
                "Game.Actor",
                $"actor-{relocationId:N}",
                "source-entry",
                5,
                ZLinkSpotKind.Entry,
                "source-owner",
                3,
                "mesh",
                sourceRid,
                5));
        return (envelope, envelope.Participants.Single(), steady);
    }

    private static ZLinkCanonicalRelocationAuthorityState PrecommitState(
        Guid relocationId,
        byte phase,
        string reference,
        uint checksum)
    {
        Span<byte> id = stackalloc byte[16];
        relocationId.TryWriteBytes(id, bigEndian: true, out _);
        var targetOwnedFields = phase == 3;
        return new ZLinkCanonicalRelocationAuthorityState(
            BinaryPrimitives.ReadUInt64BigEndian(id),
            BinaryPrimitives.ReadUInt64BigEndian(id[8..]),
            targetOwnedFields ? 1UL : 0,
            RoutingId.From("startup-source").ToHex(),
            5,
            "source-owner",
            3,
            targetOwnedFields ? RoutingId.From("startup-target").ToHex() : string.Empty,
            targetOwnedFields ? 6UL : 0,
            targetOwnedFields ? "target-owner" : string.Empty,
            targetOwnedFields ? 4UL : 0,
            targetOwnedFields ? 1UL : 0,
            "source-owner",
            3,
            RoutingId.From("startup-source").ToHex(),
            5,
            phase,
            reference,
            checksum,
            7,
            0);
    }

    private static ZLinkAuthorityEntry CanonicalEntry(
        ZLinkRelocationParticipantEnvelope participant,
        ReadOnlyMemory<byte> payload) => new(
        participant.AuthorityKey,
        new ZLinkAuthoritySnapshot(
            "source-version",
            payload,
            participant.ObjectGeneration,
            participant.AuthorityOwnerGeneration,
            "source-owner",
            3,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.Active,
                ZLinkPlacementObjectKind.Actor,
                "Game.Actor",
                new ZLinkMeshNodeDescriptorKey(
                    "mesh",
                    RoutingId.From("startup-source")),
                5,
                new ZLinkCapacityVector(1, 0, null)),
            null,
            DateTimeOffset.UnixEpoch));

    private sealed class RecoveryRelocationStore : IZLinkRelocationRepository
    {
        private readonly Dictionary<string, byte[]> _roots =
            new(StringComparer.Ordinal);

        internal void Remove(string reference) => _roots.Remove(reference);
        internal bool Contains(string reference) => _roots.ContainsKey(reference);

        public ValueTask<ZLinkRelocationStored> PutRelocationAsync(
            ReadOnlyMemory<byte> payload,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var bytes = payload.ToArray();
            var reference = Convert.ToHexString(
                System.Security.Cryptography.SHA256.HashData(bytes));
            _roots[reference] = bytes;
            var now = DateTimeOffset.UnixEpoch;
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
            if (_roots.TryGetValue(reference, out var current)
                && !current.AsSpan().SequenceEqual(bytes))
                throw new InvalidDataException("Relocation reference collision.");
            _roots[reference] = bytes;
            var now = DateTimeOffset.UnixEpoch;
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
                _roots.TryGetValue(reference, out var root)
                    ? new ZLinkRelocationReadResult.Found(root)
                    : new ZLinkRelocationReadResult.Missing());

        public ValueTask<ZLinkRelocationRenewResult> RenewRelocationAsync(
            string reference,
            TimeSpan retention,
            CancellationToken cancellationToken = default)
        {
            var now = DateTimeOffset.UnixEpoch;
            return ValueTask.FromResult<ZLinkRelocationRenewResult>(
                _roots.ContainsKey(reference)
                    ? new ZLinkRelocationRenewResult.Renewed(
                        now + retention,
                        now)
                    : new ZLinkRelocationRenewResult.Missing());
        }

        public ValueTask<ZLinkRelocationDeleteResult> DeleteRelocationAsync(
            string reference,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(
                _roots.Remove(reference)
                    ? ZLinkRelocationDeleteResult.Deleted
                    : ZLinkRelocationDeleteResult.Missing);
    }

    private sealed class RecoveryAuthorityStore(
        IReadOnlyList<ZLinkAuthorityEntry> entries) : ZLinkLocationStoreTestDouble
    {
        internal List<ZLinkAuthorityKey> CompareExchangeCalls { get; } = [];
        internal List<ZLinkAuthorityKey> ReadCalls { get; } = [];
        internal IReadOnlyList<ZLinkAuthorityEntry> Entries => entries;
        internal int ScanCalls { get; private set; }

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default)
        {
            ReadCalls.Add(key);
            return ValueTask.FromResult<ZLinkAuthorityReadResult>(
                entries.FirstOrDefault(item => item.Key == key) is { } entry
                    ? new ZLinkAuthorityReadResult.Found(entry.Snapshot)
                    : new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UnixEpoch));
        }

        public override ValueTask<ZLinkAuthorityCompareExchangeResult>
            CompareExchangeAuthorityAsync(
                ZLinkAuthorityKey key,
                string expectedStoreVersion,
                ZLinkAuthorityMutation mutation,
                CancellationToken cancellationToken = default)
        {
            CompareExchangeCalls.Add(key);
            var entry = entries.FirstOrDefault(item => item.Key == key);
            return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                entry is null
                    ? new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Missing(
                            DateTimeOffset.UnixEpoch))
                    : new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(entry.Snapshot)));
        }

        public override ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
            string prefix,
            ZLinkAuthorityScanCursor? cursor,
            int limit,
            CancellationToken cancellationToken = default)
        {
            ScanCalls++;
            return ValueTask.FromResult<ZLinkAuthorityScanResult>(
                new ZLinkAuthorityScanResult.Page(
                    new ZLinkAuthorityPage(
                        entries.Where(item => item.Key.Value.StartsWith(
                                prefix,
                                StringComparison.Ordinal))
                            .Take(limit)
                            .ToArray(),
                        null)));
        }

    }
}
