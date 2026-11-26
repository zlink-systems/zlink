using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Fact]
    public async Task PreparedReplacementWritesOnlyRepresentativeAndFencesPreviousCommit()
    {
        var provider = new InspectAuthorityBatchLocationStore(
            new ZLinkInMemoryProviderLocationStore()
        );
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "seal-source");
        var targetOwner = await ClaimAsync(repository, "seal-target");
        var source = Descriptor("source", sourceOwner);
        var target = Descriptor("target", targetOwner);
        await PublishAsync(repository, source, ZLinkLocationWriteIntent.NewClaim);
        await PublishAsync(repository, target, ZLinkLocationWriteIntent.NewClaim);
        var reservation = Reservation("representative", source, sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(reservation)
        );
        var originalPayload = ZLinkActorAuthorityPayloadCodec.Encode(
            StandaloneActorRelocationPrecommitTests.Authority(
                "representative",
                source,
                sourceOwner
            ) with
            {
                StableType = "player",
            }
        );
        var ready = Assert
            .IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(reserved.Reservation, originalPayload)
            )
            .Snapshot;
        var fence = ZLinkAggregateInventory.RelocationFence(Guid.NewGuid());
        var participant = ZLinkDirectRelocationInventory.Participant(
            reservation.Key,
            ready.StoreVersion,
            originalPayload
        );
        var digest = await repository.SealAggregateInventoryAsync(fence, [participant], default);
        var captured = await ZLinkSpotRelocationSourceInventory.CaptureAsync(
            repository,
            fence,
            reservation.Key,
            ready,
            digest,
            1,
            default
        );
        var attempt = new ZLinkAuthorityRelocation.TargetFence(
            target.Rid.ToHex(),
            target.LifecycleGeneration,
            targetOwner.OwnerId,
            (ulong)targetOwner.LeaseGeneration,
            1
        );
        var request = new ZLinkAggregatePrepareRequest(
            fence.AggregateId,
            fence.AggregateGeneration,
            [participant with { CapturedStoreVersion = captured.StoreVersion }],
            Convert.FromHexString(digest),
            new(target.MeshName, target.Rid),
            target.LifecycleGeneration,
            new(1, 0, null),
            targetOwner
        )
        {
            RepresentativeKey = reservation.Key,
            Relocation = captured.Relocation! with { Target = attempt },
        };
        Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request)
        );
        var writes = new List<ZLinkStoreWriteRequest>();
        provider.Inspect = batch =>
        {
            writes.Add(batch);
            return Task.CompletedTask;
        };
        var replacement = request with
        {
            Relocation = request.Relocation! with
            {
                Target = attempt with { AttemptGeneration = 2 },
            },
        };
        Assert.IsType<ZLinkAggregatePrepareResult.AlreadyPrepared>(
            await repository.PrepareAggregateAsync(replacement)
        );
        var exchange = Assert.Single(writes);
        var representative = ZLinkProviderLocationRepository.AuthorityMetaKey(reservation.Key);
        Assert.Equal(
            representative,
            Assert.IsType<ZLinkStoreMutation.Put>(Assert.Single(exchange.Mutations)).Key
        );
        Assert.Contains(
            exchange.Conditions,
            condition =>
                condition is ZLinkStoreCondition.Version version && version.Key == representative
        );
        Assert.IsType<ZLinkAggregatePrepareResult.Conflict>(
            await repository.PrepareAggregateAsync(request)
        );
        Assert.Equal(
            ZLinkAggregateCommitResult.Stale,
            await repository.CommitAggregateAttemptAsync(fence, 1, default)
        );
        Assert.Equal(
            ZLinkAggregateAbortResult.Stale,
            await repository.AbortAggregateAttemptAsync(fence, 1, default)
        );
        Assert.Single(writes);
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAttemptAsync(fence, 2, default)
        );
        Assert.Equal(
            ZLinkAggregateCommitResult.Stale,
            await repository.CommitAggregateAttemptAsync(fence, 1, default)
        );
        var owner = Assert
            .IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(reservation.Key)
            )
            .Snapshot;
        Assert.Equal(2UL, owner.Relocation!.Target!.AttemptGeneration);
        Assert.True(
            ZLinkActorAuthorityPayloadCodec.TryDecodeRelocating(owner.Payload.Span, out var actor)
        );
        Assert.Equal(target.Rid, actor.NodeRid);
        Assert.Equal(targetOwner.OwnerId, actor.OwnerId);
        var sealedParticipant = Assert.Single(
            await repository.ReadAggregateInventoryAsync(fence, digest, default)
        );
        Assert.Equal(originalPayload, sealedParticipant.Payload.ToArray());
        Assert.Equal("[]"u8.ToArray(), sealedParticipant.Mutation.ToArray());
    }

    [Theory]
    [InlineData("committed")]
    [InlineData("aborted")]
    public async Task TerminalInventoryCleanupResumesAfterPageDeletion(string phase)
    {
        const string prefix = "zlink:v11:";
        var storage = new ZLinkInMemoryProviderLocationStore();
        var provider = new InspectAuthorityBatchLocationStore(storage);
        var repository = new ZLinkProviderLocationRepository(provider);
        var fence = ZLinkAggregateInventory.RelocationFence(Guid.NewGuid());
        var participant = new ZLinkAggregateParticipant(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey("cleanup"),
            "v1",
            ZLinkAuthorityGenerationTransition.Preserve,
            "authority"u8.ToArray(),
            "[]"u8.ToArray()
        );
        var digest = await repository.SealAggregateInventoryAsync(fence, [participant], default);
        var key = ZLinkAggregateInventory.AggregateKey(prefix, fence);
        var root = ZLinkAggregateInventory.RootKey(prefix, fence);
        var part = ZLinkAggregateInventory.ParticipantKey(prefix, fence, 0, membership: false);
        var inventoryPrefix = ZLinkAggregateInventory.KeyPrefix(prefix, fence);
        var target = new ZLinkMeshNodeDescriptorKey("mesh", RoutingId.From("target"));
        var body = ZLinkAggregateInventory.Describe(
            fence,
            phase,
            1,
            digest,
            target,
            1,
            new(0, 0, null),
            new("target-owner", 1)
        );
        await storage.WriteAsync(
            new([], [new ZLinkStoreMutation.Put(key, ZLinkAggregateInventory.Encode(body), null)])
        );

        var deletions = new List<string>();
        provider.Inspect = batch =>
        {
            var keys = batch
                .Mutations.OfType<ZLinkStoreMutation.Delete>()
                .Select(delete => delete.Key)
                .ToArray();
            deletions.AddRange(keys.Select(value => value.Value));
            if (keys.Contains(part))
                throw new IOException("interrupted after page batch");
            return Task.CompletedTask;
        };

        // Begin with a normalized committed row or an aborted row whose fences
        // are already released. The cleanup then owns all remaining writes.
        if (phase == "committed")
        {
            var housekeeping = new ZLinkStoreKey(key.Value + ":housekeeping");
            await storage.WriteAsync(
                new(
                    [],
                    [
                        new ZLinkStoreMutation.Put(
                            housekeeping,
                            """{"stagedAt":"2026-01-01T00:00:00Z","participantsNormalized":true,"participantsCleaned":false,"relocation":null,"clearRelocation":false}"""u8.ToArray(),
                            null
                        ),
                    ]
                )
            );
        }

        var interrupted = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            repository.RemoveSealedInventoryAsync(fence, default).AsTask()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, interrupted.Kind);
        Assert.IsType<IOException>(interrupted.InnerException);
        Assert.Contains(
            deletions,
            value => value.StartsWith(inventoryPrefix + "page:", StringComparison.Ordinal)
        );
        Assert.IsType<ZLinkStoreReadResult.Found>(await storage.ReadAsync(root));
        Assert.IsType<ZLinkStoreReadResult.Found>(await storage.ReadAsync(part));
        Assert.IsType<ZLinkStoreReadResult.Found>(await storage.ReadAsync(key));
        provider.Inspect = batch =>
        {
            deletions.AddRange(
                batch
                    .Mutations.OfType<ZLinkStoreMutation.Delete>()
                    .Select(delete => delete.Key.Value)
            );
            return Task.CompletedTask;
        };
        await new ZLinkProviderLocationRepository(provider).RemoveSealedInventoryAsync(
            fence,
            default
        );
        Assert.True(deletions.LastIndexOf(part.Value) < deletions.LastIndexOf(root.Value));
        Assert.True(deletions.LastIndexOf(root.Value) < deletions.LastIndexOf(key.Value));
        Assert.IsType<ZLinkStoreReadResult.Missing>(await storage.ReadAsync(root));
        Assert.IsType<ZLinkStoreReadResult.Missing>(await storage.ReadAsync(key));
    }

    [Theory]
    [InlineData("committed")]
    [InlineData("aborted")]
    [InlineData("prepared")]
    public async Task AggregateReadDistinguishesTerminalCleanupFromMissingPreparedPage(string phase)
    {
        const string prefix = "zlink:v11:";
        var storage = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(storage);
        var fence = ZLinkAggregateInventory.RelocationFence(Guid.NewGuid());
        var key = ZLinkAggregateInventory.AggregateKey(prefix, fence);
        var root = ZLinkAggregateInventory.RootKey(prefix, fence);
        var participant = new ZLinkAggregateParticipant(
            ZLinkActorAuthorityPayloadCodec.AuthorityKey("damaged"),
            "v1",
            ZLinkAuthorityGenerationTransition.Preserve,
            "authority"u8.ToArray(),
            "[]"u8.ToArray()
        );
        var digest = await repository.SealAggregateInventoryAsync(fence, [participant], default);
        var scan = Assert.IsType<ZLinkStoreScanResult.Page>(
            await storage.ScanAsync(
                new(ZLinkAggregateInventory.KeyPrefix(prefix, fence) + "page:", null, 10)
            )
        );
        var missing = phase == "prepared" ? Assert.Single(scan.Value.Items).Key : root;
        var body = ZLinkAggregateInventory.Describe(
            fence,
            phase,
            1,
            digest,
            new("mesh", RoutingId.From("target")),
            1,
            new(0, 0, null),
            new("target-owner", 1)
        );
        await storage.WriteAsync(
            new(
                [],
                [
                    new ZLinkStoreMutation.Put(key, ZLinkAggregateInventory.Encode(body), null),
                    new ZLinkStoreMutation.Delete(missing),
                ]
            )
        );

        switch (phase)
        {
            case "prepared":
                await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(() =>
                    repository.CommitAggregateAsync(fence).AsTask()
                );
                break;
            case "committed":
                Assert.Equal(
                    ZLinkAggregateCommitResult.AlreadyCommitted,
                    await repository.CommitAggregateAsync(fence)
                );
                break;
            case "aborted":
                Assert.Equal(
                    ZLinkAggregateAbortResult.AlreadyAborted,
                    await repository.AbortAggregateAsync(fence)
                );
                break;
        }
    }
}
