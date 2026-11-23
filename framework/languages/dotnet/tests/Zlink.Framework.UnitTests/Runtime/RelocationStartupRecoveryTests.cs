using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class RelocationStartupRecoveryTests
{
    [Fact]
    public async Task PreparedRootCanBeReadByManifestBeforeAuthorityPublication()
    {
        var authority = new RecoveryAuthorityStore([]);
        var relocation = new InMemoryRelocationStore();
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
            authority,
            relocation
        );
        var envelope = RecoveryFixture.CreateEnvelope();

        var prepared = await coordinator.PrepareAsync(envelope);
        var restored = await coordinator.ReadPreparedAsync(prepared.Reference);

        Assert.Empty(authority.CompareExchangeCalls);
        Assert.Equal(envelope.AggregateId, restored.AggregateId);
        Assert.Equal(
            envelope.Participants.Select(static item => item.AuthorityKey),
            restored.Participants.Select(static item => item.AuthorityKey)
        );
    }

    [Fact]
    public async Task ExactPublishedConflictReconcilesWithoutDeletingRoot()
    {
        var fixture = await RecoveryFixture.CreateAsync();
        var envelope = RecoveryFixture.CreateEnvelope();
        var coordinator = new ZLinkRelocationPublicationCoordinator(
            new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
            fixture.Authority,
            fixture.Relocation
        );
        var prepared = await coordinator.PrepareAsync(envelope);
        var actor = envelope.Participants[0];

        var published = await coordinator.PublishPreparedAsync(
            new ZLinkRelocationPublicationRequest(
                actor.AuthorityKey,
                $"v-{actor.ObjectGeneration}",
                "target-owner",
                7,
                new byte[] { 9 },
                envelope
            ),
            prepared
        );

        Assert.Equal(fixture.Reference, published.Relocation.Reference);
        Assert.True(fixture.Relocation.Contains(fixture.Reference));
        Assert.Single(fixture.Authority.CompareExchangeCalls);
    }

    private sealed record RecoveryFixture(
        RecoveryAuthorityStore Authority,
        InMemoryRelocationStore Relocation,
        string Reference,
        ZLinkRelocationEnvelope Envelope
    )
    {
        internal static async ValueTask<RecoveryFixture> CreateAsync(
            ZLinkPlacementObjectKind spotKind = ZLinkPlacementObjectKind.UserSpot
        )
        {
            var relocation = new InMemoryRelocationStore();
            var envelope = CreateEnvelope(spotKind);
            var coordinator = new ZLinkRelocationPublicationCoordinator(
                new Zlink.Framework.UnitTests.Runtime.AuditRuntimeFailureReporter(),
                new RecoveryAuthorityStore([]),
                relocation
            );
            var prepared = await coordinator.PrepareAsync(envelope);
            var publication = new ZLinkRelocationAuthorityPayload(
                prepared.Relocation.Reference,
                prepared.Relocation.ChecksumCrc32c,
                envelope.AggregateId,
                envelope.AggregateGeneration,
                envelope.InventoryDigest,
                "target-owner",
                7,
                new byte[] { 9 }
            );
            var payload = ZLinkRelocationAuthorityPayloadCodec.Encode(publication);
            var entries = envelope
                .Participants.Select(participant => Entry(participant, payload))
                .ToArray();
            return new RecoveryFixture(
                new RecoveryAuthorityStore(entries),
                relocation,
                prepared.Relocation.Reference,
                envelope
            );
        }

        internal static ZLinkRelocationEnvelope CreateEnvelope(
            ZLinkPlacementObjectKind spotKind = ZLinkPlacementObjectKind.UserSpot
        )
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
                        []
                    ),
                    new ZLinkRelocationParticipantEnvelope(
                        spot,
                        spotKind,
                        12,
                        5,
                        new byte[] { 3 },
                        [],
                        []
                    ),
                ]
            );
        }

        private static ZLinkAuthorityEntry Entry(
            ZLinkRelocationParticipantEnvelope participant,
            ReadOnlyMemory<byte> payload
        ) =>
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
                        new ZLinkMeshNodeDescriptorKey("mesh", RoutingId.From("target")),
                        2,
                        participant.ObjectKind == ZLinkPlacementObjectKind.Actor
                            ? new ZLinkCapacityVector(1, 0, null)
                            : new ZLinkCapacityVector(
                                0,
                                1,
                                new ZLinkSpotTypeCapacityDelta(
                                    participant.ObjectKind,
                                    "Game.Room",
                                    1
                                )
                            )
                    ),
                    null,
                    DateTimeOffset.UnixEpoch
                )
            );
    }

    private sealed class RecoveryAuthorityStore(IReadOnlyList<ZLinkAuthorityEntry> entries)
        : ZLinkLocationStoreTestDouble
    {
        internal List<ZLinkAuthorityKey> CompareExchangeCalls { get; } = [];
        internal List<ZLinkAuthorityKey> ReadCalls { get; } = [];
        internal IReadOnlyList<ZLinkAuthorityEntry> Entries => entries;
        internal int ScanCalls { get; private set; }

        public override ValueTask<ZLinkAuthorityReadResult> ReadAuthorityAsync(
            ZLinkAuthorityKey key,
            CancellationToken cancellationToken = default
        )
        {
            ReadCalls.Add(key);
            return ValueTask.FromResult<ZLinkAuthorityReadResult>(
                entries.FirstOrDefault(item => item.Key == key) is { } entry
                    ? new ZLinkAuthorityReadResult.Found(entry.Snapshot)
                    : new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UnixEpoch)
            );
        }

        public override ValueTask<ZLinkAuthorityCompareExchangeResult> CompareExchangeAuthorityAsync(
            ZLinkAuthorityKey key,
            string expectedStoreVersion,
            ZLinkAuthorityMutation mutation,
            CancellationToken cancellationToken = default
        )
        {
            CompareExchangeCalls.Add(key);
            var entry = entries.FirstOrDefault(item => item.Key == key);
            return ValueTask.FromResult<ZLinkAuthorityCompareExchangeResult>(
                entry is null
                    ? new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Missing(DateTimeOffset.UnixEpoch)
                    )
                    : new ZLinkAuthorityCompareExchangeResult.Conflict(
                        new ZLinkAuthorityReadResult.Found(entry.Snapshot)
                    )
            );
        }

        public override ValueTask<ZLinkAuthorityScanResult> ListAuthoritiesAsync(
            string prefix,
            ZLinkAuthorityScanCursor? cursor,
            int limit,
            CancellationToken cancellationToken = default
        )
        {
            ScanCalls++;
            throw new InvalidOperationException(
                "Exact relocation reconciliation must not scan authorities."
            );
        }
    }
}
