using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class ProviderLocationRepositoryAuthorityTests
{
    [Fact]
    public async Task OwnerClaimResponseLossReconcilesTheAppliedLease()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner)
        {
            ThrowAfterNextWrite = true
        };
        var repository = new ZLinkProviderLocationRepository(provider);

        var claimed = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await repository.ClaimOwnerLeaseAsync(
                "ambiguous-owner",
                TimeSpan.FromMinutes(2)));
        var read = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await repository.ReadOwnerLeaseAsync("ambiguous-owner"));

        Assert.Equal(claimed.Token, read.Token);
        Assert.Equal(claimed.LeaseExpiresAt, read.LeaseExpiresAt);
    }

    [Fact]
    public async Task DescriptorPagingUsesTheBoundedProviderSnapshotCursor()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ExpireFirstSnapshotLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "snapshot-owner");
        _ = await repository.UpdateMeshNodeAsync(
            Descriptor("source", owner),
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            Descriptor("target", owner),
            ZLinkLocationWriteIntent.NewClaim);

        var page = await repository.ListMeshNodesAsync(
            "play",
            new ZLinkPageRequest(100));

        Assert.Single(page.Items);
        Assert.NotNull(page.ContinuationToken);
        Assert.Equal(1, provider.ScanCalls);

        await Assert.ThrowsAsync<ZLinkLocationSnapshotExpiredException>(
            () => repository.ListMeshNodesAsync(
                    "play",
                    new ZLinkPageRequest(100, page.ContinuationToken))
                .AsTask());
        Assert.Equal(2, provider.ScanCalls);

        await Assert.ThrowsAsync<ArgumentException>(
            () => repository.ListMeshNodesAsync(
                    "another-mesh",
                    new ZLinkPageRequest(100, page.ContinuationToken))
                .AsTask());
        Assert.Equal(2, provider.ScanCalls);

        await Assert.ThrowsAsync<ArgumentException>(
            () => repository.ListMeshNodesAsync(
                    "play",
                    new ZLinkPageRequest(100, new string('x', 5601)))
                .AsTask());
        Assert.Equal(2, provider.ScanCalls);
    }

    [Fact]
    public async Task InternalCompleteListDiscardsAnExpiredProviderSnapshot()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ExpireFirstSnapshotLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "complete-list-owner");
        _ = await repository.UpdateMeshNodeAsync(
            Descriptor("source", owner),
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            Descriptor("target", owner),
            ZLinkLocationWriteIntent.NewClaim);

        var rows = await repository.ListAllMeshNodesAsync("play");

        Assert.Equal(2, rows.Count);
        Assert.True(provider.ScanCalls >= 4);
    }

    [Theory]
    [InlineData("mesh")]
    [InlineData("client-server")]
    [InlineData("fanout")]
    public async Task DescriptorResponseLossReconcilesTheExactAppliedRow(
        string descriptorKind)
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "descriptor-owner");
        provider.ThrowAfterNextWrite = true;

        ZLinkLocationWriteResult result;
        switch (descriptorKind)
        {
            case "mesh":
                result = await repository.UpdateMeshNodeAsync(
                    Descriptor("source", owner),
                    ZLinkLocationWriteIntent.NewClaim);
                Assert.Single(
                    (await repository.ListMeshNodesAsync("play", default))
                    .Items);
                break;
            case "client-server":
                result = await repository.UpdateClientServerAsync(
                    new ZLinkClientServerServerDescriptor(
                        "orders",
                        RoutingId.From("orders-server"),
                        1,
                        1,
                        "tcp://127.0.0.1:7201",
                        100,
                        ZLinkFrameworkRuntimeState.Serving,
                        string.Empty,
                        owner.OwnerId,
                        owner.LeaseGeneration,
                        default),
                    ZLinkLocationWriteIntent.NewClaim);
                Assert.Single(
                    (await repository.ListClientServersAsync(
                        "orders",
                        default)).Items);
                break;
            case "fanout":
                result = await repository.UpdateFanoutPublisherAsync(
                    new ZLinkFanoutPublisherDescriptor(
                        "events",
                        RoutingId.From("events-publisher"),
                        1,
                        1,
                        "tcp://127.0.0.1:7202",
                        ZLinkFrameworkRuntimeState.Serving,
                        string.Empty,
                        owner.OwnerId,
                        owner.LeaseGeneration,
                        default),
                    ZLinkLocationWriteIntent.NewClaim);
                Assert.Single(
                    (await repository.ListFanoutPublishersAsync(
                        "events",
                        default)).Items);
                break;
            default:
                throw new ArgumentOutOfRangeException(
                    nameof(descriptorKind));
        }

        Assert.Equal(ZLinkLocationWriteStatus.Stored, result.Status);
        Assert.Equal(1UL, result.Generation);
    }

    [Fact]
    public async Task SharedOpaqueProvider_CreatesAndReadsAuthorityAcrossRepositories()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var source = new ZLinkProviderLocationRepository(provider);
        var target = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(target, "target-owner");
        var descriptor = Descriptor("target", owner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await target.UpdateMeshNodeAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);

        var request = Reservation("actor:shared", descriptor, owner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await source.ReserveAsync(request));
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await source.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x22 }));
        var observed = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await target.ReadAuthorityAsync(request.Key));

        Assert.Equal(committed.Snapshot.StoreVersion, observed.Snapshot.StoreVersion);
        Assert.Equal((byte)0x22, observed.Snapshot.Payload.Span[0]);
        Assert.Equal(owner.OwnerId, observed.Snapshot.OwnerId);
    }

    [Fact]
    public async Task ReservedCreationCompletesAfterPlacementWeightBecomesZero()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "target-owner");
        var descriptor = Descriptor("target", owner, actorLimit: 1);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        var request = Reservation("actor:weight-change", descriptor, owner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(request));

        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                descriptor with
                {
                    DescriptorRevision = descriptor.DescriptorRevision + 1,
                    PlacementWeight = 0
                },
                ZLinkLocationWriteIntent.Renew)).Status);

        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x23 }));
        var current = Assert.Single(
            (await repository.ListMeshNodesAsync("play", default)).Items);
        Assert.Equal(0, current.PlacementWeight);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                current with
                {
                    DescriptorRevision = current.DescriptorRevision + 1,
                    PlacementWeight = 100
                },
                ZLinkLocationWriteIntent.Renew)).Status);
        Assert.IsType<ZLinkObjectReserveResult.PlacementCapacityExhausted>(
            await repository.ReserveAsync(
                Reservation("actor:capacity-proof", descriptor, owner)));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RecreatingDeletedAuthorityAdvancesObjectGeneration(
        bool userSpot)
    {
        var objectKind = userSpot
            ? ZLinkPlacementObjectKind.UserSpot
            : ZLinkPlacementObjectKind.Actor;
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "target-owner");
        var descriptor = Descriptor("target", owner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                descriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        var key = $"recreate:{objectKind}";
        var request = Reservation(key, descriptor, owner, objectKind);
        var first = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(request));
        var committed = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                first.Reservation,
                new byte[] { 0x24 })).Snapshot;
        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Deleted>(
            await repository.CompareExchangeAuthorityAsync(
                request.Key,
                committed.StoreVersion,
                new ZLinkAuthorityMutation.Delete()));

        var second = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(request));

        Assert.True(
            second.Reservation.ObjectGeneration > first.Reservation.ObjectGeneration);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ExpiredOwnerAuthorityIsReclaimedWithNewObjectGeneration(
        bool userSpot)
    {
        var objectKind = userSpot
            ? ZLinkPlacementObjectKind.UserSpot
            : ZLinkPlacementObjectKind.Actor;
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                sourceDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                targetDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        var key = $"expired-owner:{objectKind}";
        var firstRequest = Reservation(
            key,
            sourceDescriptor,
            sourceOwner,
            objectKind);
        var first = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(firstRequest));
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                first.Reservation,
                new byte[] { 0x25 }));
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await repository.ReleaseOwnerLeaseAsync(sourceOwner));

        var replacement = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(
                Reservation(
                    key,
                    targetDescriptor,
                    targetOwner,
                    objectKind)));

        Assert.True(
            replacement.Reservation.ObjectGeneration
            > first.Reservation.ObjectGeneration);
        Assert.Equal(targetOwner, replacement.Reservation.TargetOwner);
        Assert.Equal(
            targetDescriptor.Rid,
            replacement.Reservation.TargetDescriptor.Rid);
    }

    [Fact]
    public async Task ConcurrentExpiredOwnerReclaimIssuesOneReplacementReservation()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var firstRepository = new ZLinkProviderLocationRepository(provider);
        var secondRepository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(firstRepository, "source-owner");
        var targetOwner = await ClaimAsync(firstRepository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner, actorLimit: 1);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await firstRepository.UpdateMeshNodeAsync(
                sourceDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await firstRepository.UpdateMeshNodeAsync(
                targetDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        var key = "actor:concurrent-expired-owner";
        var firstRequest = Reservation(key, sourceDescriptor, sourceOwner);
        var first = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await firstRepository.ReserveAsync(firstRequest));
        Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await firstRepository.CommitAsync(
                first.Reservation,
                new byte[] { 0x26 }));
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await firstRepository.ReleaseOwnerLeaseAsync(sourceOwner));
        var replacementRequest = Reservation(
            key,
            targetDescriptor,
            targetOwner);

        var results = await Task.WhenAll(
            firstRepository.ReserveAsync(replacementRequest).AsTask(),
            secondRepository.ReserveAsync(replacementRequest).AsTask());

        var replacement = Assert.Single(
            results.OfType<ZLinkObjectReserveResult.Reserved>());
        Assert.True(
            replacement.Reservation.ObjectGeneration
            > first.Reservation.ObjectGeneration);
        Assert.Single(
            results.Where(result =>
                result is ZLinkObjectReserveResult.Conflict(
                    ZLinkAuthorityReadResult.Found)));
        Assert.IsType<ZLinkObjectReserveResult.PlacementCapacityExhausted>(
            await firstRepository.ReserveAsync(
                Reservation(
                    "actor:capacity-after-reclaim",
                    targetDescriptor,
                    targetOwner)));
    }

    [Fact]
    public async Task SharedOpaqueProvider_RelocationCapacityAndOwnerCommitAreAtomic()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var source = new ZLinkProviderLocationRepository(provider);
        var target = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(source, "source-owner");
        var targetOwner = await ClaimAsync(target, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await source.UpdateMeshNodeAsync(
                sourceDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await target.UpdateMeshNodeAsync(
                targetDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);

        var request = Reservation(
            "actor:relocation",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await source.ReserveAsync(request));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await source.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x31 })).Snapshot;
        var relocation = new ZLinkRelocationCapacityReservationRequest(
            Guid.NewGuid(),
            request.Key,
            created.StoreVersion,
            ZLinkPlacementObjectKind.Actor,
            "player",
            new ZLinkMeshNodeDescriptorKey(
                "play",
                sourceDescriptor.Rid),
            sourceDescriptor.LifecycleGeneration,
            sourceOwner,
            new ZLinkMeshNodeDescriptorKey(
                "play",
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            targetOwner,
            new ZLinkCapacityVector(1, 0, null));
        var capacity = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await target.ReserveRelocationCapacityAsync(relocation));

        var moved = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await source.CompareExchangeAuthorityAsync(
                request.Key,
                created.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x32 },
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    targetOwner,
                    capacity.Fence))).Snapshot;
        var observed = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await target.ReadAuthorityAsync(request.Key)).Snapshot;

        Assert.Equal(targetOwner.OwnerId, moved.OwnerId);
        Assert.Equal(targetOwner, new ZLinkLocationOwnerToken(
            observed.OwnerId,
            observed.OwnerLeaseGeneration));
        Assert.Equal(created.ObjectGeneration, observed.ObjectGeneration);
        Assert.Equal(
            capacity.TargetAuthorityOwnerGeneration,
            observed.AuthorityOwnerGeneration);
        Assert.True(
            capacity.TargetAuthorityOwnerGeneration
            > created.AuthorityOwnerGeneration);
        Assert.Equal(targetDescriptor.Rid, observed.Allocation.Descriptor.Rid);
        Assert.IsType<ZLinkRelocationCapacityReserveResult.Conflict>(
            await target.ReserveRelocationCapacityAsync(relocation));
    }

    [Fact]
    public async Task SharedOpaqueProvider_RelocationOwnerCommitRetriesUnchangedAuthorityAfterCapacityConflict()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new AuthorityCapacityConflictOnceLocationStore(inner);
        var source = new ZLinkProviderLocationRepository(provider);
        var target = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(source, "source-owner");
        var targetOwner = await ClaimAsync(target, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await source.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await target.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);

        var request = Reservation(
            "actor:relocation-capacity-retry",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await source.ReserveAsync(request));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await source.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x33 })).Snapshot;
        var relocation = new ZLinkRelocationCapacityReservationRequest(
            Guid.NewGuid(),
            request.Key,
            created.StoreVersion,
            ZLinkPlacementObjectKind.Actor,
            "player",
            new ZLinkMeshNodeDescriptorKey(
                "play",
                sourceDescriptor.Rid),
            sourceDescriptor.LifecycleGeneration,
            sourceOwner,
            new ZLinkMeshNodeDescriptorKey(
                "play",
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            targetOwner,
            new ZLinkCapacityVector(1, 0, null));
        var capacity = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await target.ReserveRelocationCapacityAsync(relocation));
        provider.ResetAuthorityMoveAttempts();
        provider.ConflictNextAuthorityMove = true;

        var moved = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await source.CompareExchangeAuthorityAsync(
                request.Key,
                created.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x34 },
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    targetOwner,
                    capacity.Fence)));

        Assert.Equal(2, provider.AuthorityMoveAttempts);
        Assert.Equal(targetOwner.OwnerId, moved.Snapshot.OwnerId);
        Assert.Equal(
            capacity.TargetAuthorityOwnerGeneration,
            moved.Snapshot.AuthorityOwnerGeneration);
    }

    [Fact]
    public async Task Stale_Authority_Delete_Submits_The_Expected_Version_Fence()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var source = new ZLinkProviderLocationRepository(provider);
        var target = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(source, "source-owner");
        var targetOwner = await ClaimAsync(target, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await source.UpdateMeshNodeAsync(
                sourceDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await target.UpdateMeshNodeAsync(
                targetDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);

        var createRequest = Reservation(
            "actor:stale-delete",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await source.ReserveAsync(createRequest));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await source.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x31 })).Snapshot;
        var relocation = new ZLinkRelocationCapacityReservationRequest(
            Guid.NewGuid(),
            createRequest.Key,
            created.StoreVersion,
            ZLinkPlacementObjectKind.Actor,
            "player",
            new ZLinkMeshNodeDescriptorKey(
                "play",
                sourceDescriptor.Rid),
            sourceDescriptor.LifecycleGeneration,
            sourceOwner,
            new ZLinkMeshNodeDescriptorKey(
                "play",
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            targetOwner,
            new ZLinkCapacityVector(1, 0, null));
        var capacity = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await target.ReserveRelocationCapacityAsync(relocation));
        _ = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await source.CompareExchangeAuthorityAsync(
                createRequest.Key,
                created.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x32 },
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    targetOwner,
                    capacity.Fence)));

        var deleteBatchesBefore = provider.DeleteBatchCalls;
        var stale = await source.CompareExchangeAuthorityAsync(
            createRequest.Key,
            created.StoreVersion,
            new ZLinkAuthorityMutation.Delete());

        Assert.IsType<ZLinkAuthorityCompareExchangeResult.Conflict>(stale);
        Assert.Equal(deleteBatchesBefore + 1, provider.DeleteBatchCalls);
        var current = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await target.ReadAuthorityAsync(createRequest.Key));
        Assert.Equal(targetOwner.OwnerId, current.Snapshot.OwnerId);
        Assert.Equal(
            created.ObjectGeneration,
            current.Snapshot.ObjectGeneration);
    }

    [Fact]
    public async Task SharedOpaqueProvider_AbortedReservationBurnsIssuedGeneration()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                sourceDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);
        Assert.Equal(
            ZLinkLocationWriteStatus.Stored,
            (await repository.UpdateMeshNodeAsync(
                targetDescriptor,
                ZLinkLocationWriteIntent.NewClaim)).Status);

        var createRequest = Reservation(
            "actor:burned-generation",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(createRequest));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x41 })).Snapshot;

        ZLinkRelocationCapacityReservationRequest Relocation(Guid id) =>
            new(
                id,
                createRequest.Key,
                created.StoreVersion,
                ZLinkPlacementObjectKind.Actor,
                "player",
                new ZLinkMeshNodeDescriptorKey(
                    "play",
                    sourceDescriptor.Rid),
                sourceDescriptor.LifecycleGeneration,
                sourceOwner,
                new ZLinkMeshNodeDescriptorKey(
                    "play",
                    targetDescriptor.Rid),
                targetDescriptor.LifecycleGeneration,
                targetOwner,
                new ZLinkCapacityVector(1, 0, null));

        var firstRequest = Relocation(Guid.NewGuid());
        var first = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await repository.ReserveRelocationCapacityAsync(
                firstRequest));
        Assert.Equal(
            ZLinkRelocationCapacityAbortResult.Aborted,
            await repository.AbortRelocationCapacityAsync(first.Fence));
        Assert.IsType<ZLinkRelocationCapacityReserveResult.Conflict>(
            await repository.ReserveRelocationCapacityAsync(
                firstRequest));
        var second = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.Reserved>(
            await repository.ReserveRelocationCapacityAsync(
                Relocation(Guid.NewGuid())));

        Assert.Equal(
            created.AuthorityOwnerGeneration + 2,
            second.TargetAuthorityOwnerGeneration);
    }

    [Fact]
    public async Task SharedOpaqueProvider_ReconcilesAppliedReservationAfterReplyLoss()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var createRequest = Reservation(
            "actor:ambiguous-reserve",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(createRequest));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x51 })).Snapshot;
        var request = new ZLinkRelocationCapacityReservationRequest(
            Guid.NewGuid(),
            createRequest.Key,
            created.StoreVersion,
            ZLinkPlacementObjectKind.Actor,
            "player",
            new ZLinkMeshNodeDescriptorKey(
                "play",
                sourceDescriptor.Rid),
            sourceDescriptor.LifecycleGeneration,
            sourceOwner,
            new ZLinkMeshNodeDescriptorKey(
                "play",
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            targetOwner,
            new ZLinkCapacityVector(1, 0, null));

        provider.ThrowAfterNextWrite = true;
        var reconciled = Assert.IsType<
            ZLinkRelocationCapacityReserveResult.AlreadyReserved>(
            await repository.ReserveRelocationCapacityAsync(request));

        Assert.Equal(
            created.AuthorityOwnerGeneration + 1,
            reconciled.TargetAuthorityOwnerGeneration);
    }

    [Fact]
    public async Task SharedOpaqueProvider_ReconcilesAppliedAggregateAfterReplyLoss()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var createRequest = Reservation(
            "actor:ambiguous-aggregate",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(createRequest));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x61 })).Snapshot;
        var request = new ZLinkAggregatePrepareRequest(
            Guid.NewGuid(),
            1,
            [
                new ZLinkAggregateParticipant(
                    createRequest.Key,
                    created.StoreVersion,
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    new byte[] { 0x62 },
                    new byte[] { 0x63 })
            ],
            Enumerable.Repeat((byte)0x64, 32).ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "play",
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            new ZLinkCapacityVector(1, 0, null),
            targetOwner);

        provider.ThrowAfterNextWrite = true;
        // Root claim and the authoritative inventory page add two writes
        // before participant staging. Lose the final Prepared response.
        provider.SkipWritesBeforeThrow = 5;
        var reconciled = Assert.IsType<
            ZLinkAggregatePrepareResult.AlreadyPrepared>(
            await repository.PrepareAggregateAsync(request));

        Assert.Equal(
            created.AuthorityOwnerGeneration + 1,
            reconciled.TargetAuthorityOwnerGenerations[createRequest.Key]);
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(reconciled.Fence));
        var afterCommit =
            Assert.IsType<ZLinkAggregatePrepareResult.AlreadyPrepared>(
                await repository.PrepareAggregateAsync(request));
        Assert.Equal(
            reconciled.TargetAuthorityOwnerGenerations[createRequest.Key],
            afterCommit.TargetAuthorityOwnerGenerations[createRequest.Key]);
    }

    [Fact]
    public async Task SharedOpaqueProvider_CommittedAggregateIsVisibleBeforePhysicalNormalization()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var createRequest = Reservation(
            "actor:aggregate-visible",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(createRequest));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x65 })).Snapshot;
        var request = new ZLinkAggregatePrepareRequest(
            Guid.NewGuid(),
            1,
            [
                new ZLinkAggregateParticipant(
                    createRequest.Key,
                    created.StoreVersion,
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    new byte[] { 0x66 },
                    new byte[] { 0x67 })
            ],
            Enumerable.Repeat((byte)0x68, 32).ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "play",
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            new ZLinkCapacityVector(1, 0, null),
            targetOwner);
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request));

        provider.ThrowAfterNextWrite = true;
        provider.BlockWritesAfterThrownResponse = true;
        await Assert.ThrowsAsync<IOException>(async () =>
            await repository.CommitAggregateAsync(prepared.Fence));

        var visible = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await repository.ReadAuthorityAsync(createRequest.Key)).Snapshot;
        Assert.Equal(new byte[] { 0x66 }, visible.Payload);
        Assert.Equal(targetOwner.OwnerId, visible.OwnerId);
        Assert.Equal(
            prepared.TargetAuthorityOwnerGenerations[createRequest.Key],
            visible.AuthorityOwnerGeneration);
        Assert.Equal(targetDescriptor.Rid, visible.Allocation.Descriptor.Rid);

        provider.BlockWrites = false;
        Assert.Equal(
            ZLinkAggregateCommitResult.AlreadyCommitted,
            await repository.CommitAggregateAsync(prepared.Fence));
        var normalized = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await repository.ReadAuthorityAsync(createRequest.Key)).Snapshot;
        Assert.Equal(visible.Payload.ToArray(), normalized.Payload.ToArray());
        Assert.Equal(
            visible.AuthorityOwnerGeneration,
            normalized.AuthorityOwnerGeneration);
    }

    [Fact]
    public async Task SharedOpaqueProvider_ConcurrentDistinctReservationsHideCounterContention()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "target-owner");
        var descriptor = Descriptor("target", owner);
        _ = await repository.UpdateMeshNodeAsync(
            descriptor,
            ZLinkLocationWriteIntent.NewClaim);

        var results = await Task.WhenAll(
            Enumerable.Range(0, 64)
                .Select(index => repository.ReserveAsync(
                        Reservation(
                            $"actor:counter-contention:{index}",
                            descriptor,
                            owner))
                    .AsTask()));

        var reservations = results
            .Select(static result =>
                Assert.IsType<ZLinkObjectReserveResult.Reserved>(result))
            .ToArray();
        Assert.Equal(
            64,
            reservations
                .Select(static result =>
                    result.Reservation.AuthorityOwnerGeneration)
                .Distinct()
                .Count());
    }

    [Fact]
    public async Task SharedOpaqueProvider_ConcurrentDistinctCreationCompletionsRetryCapacityContention()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "target-owner");
        var descriptor = Descriptor("target", owner);
        _ = await repository.UpdateMeshNodeAsync(
            descriptor,
            ZLinkLocationWriteIntent.NewClaim);

        var reservations = await Task.WhenAll(
            Enumerable.Range(0, 64)
                .Select(async index =>
                {
                    var request = Reservation(
                        $"actor:completion-contention:{index}",
                        descriptor,
                        owner);
                    var reserved = Assert.IsType<
                        ZLinkObjectReserveResult.Reserved>(
                        await repository.ReserveAsync(request));
                    return (index, request, reserved.Reservation);
                }));

        var completions = await Task.WhenAll(
            reservations.Select(async item =>
            {
                var envelope = new byte[] { 0x54, checked((byte)item.index) };
                var operation = new ZLinkCreationOperationId(
                    descriptor.Rid,
                    descriptor.LifecycleGeneration,
                    0,
                    checked((ulong)item.index + 1));
                var completion = new ZLinkObjectCreationCompletion.Created(
                    new byte[] { 0x22 },
                    new ZLinkCreationTerminalPublication(
                        operation,
                        envelope,
                        SHA256.HashData(envelope),
                        DateTimeOffset.UtcNow.AddMinutes(5)));
                return (
                    item,
                    operation,
                    Result: await repository.CompleteCreationAsync(
                        item.Reservation,
                        completion));
            }));

        foreach (var completion in completions)
        {
            Assert.IsType<ZLinkObjectCreationCompleteResult.Created>(
                completion.Result);
            Assert.IsType<ZLinkCreationTerminalReadResult.Found>(
                await repository.ReadCreationTerminalAsync(
                    completion.operation));
            Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(
                    completion.item.request.Key));
        }
    }

    [Fact]
    public async Task SharedOpaqueProvider_ConcurrentDistinctCommitsRetryCapacityContention()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner = await ClaimAsync(repository, "target-owner");
        var descriptor = Descriptor("target", owner);
        _ = await repository.UpdateMeshNodeAsync(
            descriptor,
            ZLinkLocationWriteIntent.NewClaim);

        var reservations = await Task.WhenAll(
            Enumerable.Range(0, 64)
                .Select(async index =>
                {
                    var request = Reservation(
                        $"actor:commit-contention:{index}",
                        descriptor,
                        owner);
                    var reserved = Assert.IsType<
                        ZLinkObjectReserveResult.Reserved>(
                        await repository.ReserveAsync(request));
                    return (request, reserved.Reservation);
                }));

        var commits = await Task.WhenAll(
            reservations.Select(item =>
                repository.CommitAsync(
                        item.Reservation,
                        new byte[] { 0x23 })
                    .AsTask()));

        Assert.All(
            commits,
            static result =>
                Assert.IsType<ZLinkObjectCommitResult.Committed>(result));
        foreach (var item in reservations)
            Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(item.request.Key));
    }

    [Fact]
    public async Task SharedOpaqueProvider_AggregateCommitRetriesCapacityContention()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new AggregateCommitConflictOnceLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = new List<ZLinkAggregateParticipant>();
        for (var index = 0; index < 2; index++)
        {
            var createRequest = Reservation(
                $"actor:aggregate-contention:{index}",
                sourceDescriptor,
                sourceOwner);
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await repository.ReserveAsync(createRequest));
            var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(
                    reserved.Reservation,
                    new byte[] { 0x69 })).Snapshot;
            participants.Add(new ZLinkAggregateParticipant(
                createRequest.Key,
                created.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                new byte[] { 0x6A, (byte)index },
                new byte[] { 0x6B }));
        }
        var request = AggregateRequest(
            participants,
            targetDescriptor,
            targetOwner);
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request));

        provider.ConflictNextAggregateCommit = true;

        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(prepared.Fence));
        Assert.Equal(2, provider.AggregateCommitAttempts);
        foreach (var participant in participants)
        {
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(participant.Key)).Snapshot;
            Assert.Equal(targetOwner.OwnerId, authority.OwnerId);
            Assert.True(participant.AuthorityPayload.Span.SequenceEqual(
                authority.Payload.Span));
        }
    }

    [Fact]
    public async Task SharedOpaqueProvider_PreparesMaximumAggregateWithinKeyBound()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = new List<ZLinkAggregateParticipant>(1024);
        for (var index = 0; index < 1024; index++)
        {
            var createRequest = Reservation(
                $"actor:aggregate-bound:{index}",
                sourceDescriptor,
                sourceOwner);
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await repository.ReserveAsync(createRequest));
            var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(
                    reserved.Reservation,
                    new byte[] { 0x71 })).Snapshot;
            participants.Add(new ZLinkAggregateParticipant(
                createRequest.Key,
                created.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                Enumerable.Repeat((byte)0x72, 4096).ToArray(),
                new byte[] { 0x73 }));
        }
        var request = new ZLinkAggregatePrepareRequest(
            Guid.NewGuid(),
            1,
            participants,
            Enumerable.Repeat((byte)0x74, 32).ToArray(),
            new ZLinkMeshNodeDescriptorKey(
                "play",
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            new ZLinkCapacityVector(1024, 0, null),
            targetOwner);

        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request));

        Assert.Equal(1024, prepared.TargetAuthorityOwnerGenerations.Count);
        Assert.Equal(
            1024,
            prepared.TargetAuthorityOwnerGenerations.Values.Distinct().Count());
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(prepared.Fence));
        Assert.DoesNotContain(
            await ScanKeysAsync(
                provider,
                AggregatePrefix(prepared.Fence)),
            static key => key.Contains(":participant:", StringComparison.Ordinal));
        foreach (var participant in new[]
                 {
                     participants[0],
                     participants[^1]
                 })
        {
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(participant.Key)).Snapshot;
            Assert.Equal(4096, authority.Payload.Length);
            Assert.Equal((byte)0x72, authority.Payload.Span[0]);
            Assert.Equal((byte)0x72, authority.Payload.Span[^1]);
            Assert.Equal(targetOwner.OwnerId, authority.OwnerId);
            Assert.Equal(
                targetOwner.LeaseGeneration,
                authority.OwnerLeaseGeneration);
            Assert.Equal(
                prepared.TargetAuthorityOwnerGenerations[participant.Key],
                authority.AuthorityOwnerGeneration);
        }
    }

    [Fact]
    public async Task SharedOpaqueProvider_MaximumParticipantPayloadsDoNotExpandAggregateRecord()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);

        var participants = new List<ZLinkAggregateParticipant>();
        for (var index = 0; index < 4; index++)
        {
            var create = Reservation(
                $"actor:payload-bound:{index}",
                sourceDescriptor,
                sourceOwner);
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await repository.ReserveAsync(create));
            var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(
                    reserved.Reservation,
                    new byte[] { (byte)index })).Snapshot;
            var payload = new byte[1024 * 1024];
            payload[0] = (byte)(0x80 + index);
            payload[^1] = (byte)(0x90 + index);
            participants.Add(new ZLinkAggregateParticipant(
                create.Key,
                created.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                payload,
                SHA256.HashData(payload)));
        }

        var request = AggregateRequest(
            participants,
            targetDescriptor,
            targetOwner);
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request));
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(prepared.Fence));

        foreach (var participant in participants)
        {
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(participant.Key)).Snapshot;
            Assert.Equal(1024 * 1024, authority.Payload.Length);
            Assert.Equal(
                participant.AuthorityPayload.Span[0],
                authority.Payload.Span[0]);
            Assert.Equal(
                participant.AuthorityPayload.Span[^1],
                authority.Payload.Span[^1]);
        }
    }

    [Fact]
    public async Task SharedOpaqueProvider_PartialNormalizationRetriesEveryParticipant()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = new List<ZLinkAggregateParticipant>();
        for (var index = 0; index < 16; index++)
        {
            var create = Reservation(
                $"actor:partial-normalize:{index}",
                sourceDescriptor,
                sourceOwner);
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await repository.ReserveAsync(create));
            var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(
                    reserved.Reservation,
                    new byte[] { (byte)index })).Snapshot;
            participants.Add(new ZLinkAggregateParticipant(
                create.Key,
                created.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                new byte[] { 0xa0, (byte)index },
                new byte[] { 0xb0, (byte)index }));
        }
        var request = AggregateRequest(
            participants,
            targetDescriptor,
            targetOwner);
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request));

        provider.ThrowAfterNextWrite = true;
        // Keep the root and inventory page, then fail the remaining staging
        // sequence. The retry must fill any rows that were not written.
        provider.SkipWritesBeforeThrow = 2;
        provider.BlockWritesAfterThrownResponse = true;
        await Assert.ThrowsAnyAsync<IOException>(async () =>
            await repository.CommitAggregateAsync(prepared.Fence));

        foreach (var participant in participants)
        {
            var visible = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(participant.Key)).Snapshot;
            Assert.Equal(participant.AuthorityPayload, visible.Payload);
            Assert.Equal(targetOwner.OwnerId, visible.OwnerId);
        }
        Assert.Contains(
            await ScanKeysAsync(
                inner,
                AggregatePrefix(prepared.Fence)),
            static key => key.EndsWith(":payload", StringComparison.Ordinal));

        provider.BlockWrites = false;
        Assert.Equal(
            ZLinkAggregateCommitResult.AlreadyCommitted,
            await repository.CommitAggregateAsync(prepared.Fence));
        foreach (var participant in participants)
        {
            var normalized = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(participant.Key)).Snapshot;
            Assert.Equal(participant.AuthorityPayload, normalized.Payload);
            Assert.Equal(targetOwner.OwnerId, normalized.OwnerId);
        }
        Assert.DoesNotContain(
            await ScanKeysAsync(
                inner,
                AggregatePrefix(prepared.Fence)),
            static key => key.Contains(":participant:", StringComparison.Ordinal));
    }

    [Fact]
    public async Task SharedOpaqueProvider_AggregatePreparationUsesBoundedConcurrency()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ConcurrencyTrackingLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = new List<ZLinkAggregateParticipant>();
        for (var index = 0; index < 64; index++)
        {
            var create = Reservation(
                $"actor:parallel:{index}",
                sourceDescriptor,
                sourceOwner);
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await repository.ReserveAsync(create));
            var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(
                    reserved.Reservation,
                    new byte[] { (byte)index })).Snapshot;
            participants.Add(new ZLinkAggregateParticipant(
                create.Key,
                created.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                new byte[] { 0xc0, (byte)index },
                new byte[] { 0xd0, (byte)index }));
        }

        provider.Reset();
        _ = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(AggregateRequest(
                participants,
                targetDescriptor,
                targetOwner)));

        Assert.InRange(provider.MaximumConcurrentReads, 2, 64);
        Assert.InRange(provider.MaximumConcurrentWrites, 2, 64);
    }

    [Fact]
    public async Task SharedOpaqueProvider_StagingFailureIsOwnedAndRetryable()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = new List<ZLinkAggregateParticipant>();
        for (var index = 0; index < 2; index++)
        {
            var create = Reservation(
                $"actor:staging-retry:{index}",
                sourceDescriptor,
                sourceOwner);
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await repository.ReserveAsync(create));
            var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(
                    reserved.Reservation,
                    new byte[] { (byte)index })).Snapshot;
            participants.Add(new ZLinkAggregateParticipant(
                create.Key,
                created.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                new byte[] { 0xe0, (byte)index },
                new byte[] { 0xf0, (byte)index }));
        }
        var request = AggregateRequest(
            participants,
            targetDescriptor,
            targetOwner);
        var fence = new ZLinkAggregateFence(
            request.AggregateId,
            request.AggregateGeneration);

        provider.ThrowAfterNextWrite = true;
        provider.SkipWritesBeforeThrow = 1;
        provider.BlockWritesAfterThrownResponse = true;
        await Assert.ThrowsAsync<IOException>(async () =>
            await repository.PrepareAggregateAsync(request));
        Assert.Equal(
            ZLinkAggregateCommitResult.Stale,
            await repository.CommitAggregateAsync(fence));
        Assert.Contains(
            await ScanKeysAsync(inner, AggregatePrefix(fence)),
            static key => key.Contains(":inventory:", StringComparison.Ordinal));

        provider.BlockWrites = false;
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request));
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(prepared.Fence));
        Assert.DoesNotContain(
            await ScanKeysAsync(inner, AggregatePrefix(fence)),
            static key => key.Contains(":participant:", StringComparison.Ordinal));
    }

    [Fact]
    public async Task SharedOpaqueProvider_CancellationAfterFenceInstallAbortsAndReleasesAllFences()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new CancelAfterFirstAggregateFenceStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "cancel-source");
        var targetOwner = await ClaimAsync(repository, "cancel-target");
        var sourceDescriptor = Descriptor("cancel-source", sourceOwner);
        var targetDescriptor = Descriptor("cancel-target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = new List<ZLinkAggregateParticipant>();
        for (var index = 0; index < 2; index++)
        {
            var create = Reservation(
                $"actor:fence-cancel:{index}",
                sourceDescriptor,
                sourceOwner);
            var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await repository.ReserveAsync(create));
            var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
                await repository.CommitAsync(
                    reserved.Reservation,
                    new byte[] { (byte)index })).Snapshot;
            participants.Add(new ZLinkAggregateParticipant(
                create.Key,
                created.StoreVersion,
                ZLinkAuthorityGenerationTransition.NewOwner,
                new byte[] { 0x71, (byte)index },
                new byte[] { 0x72, (byte)index }));
        }
        var request = AggregateRequest(
            participants,
            targetDescriptor,
            targetOwner);
        using var callerCancellation = new CancellationTokenSource();
        provider.FailAggregateId = request.AggregateId;
        provider.CallerCancellation = callerCancellation;

        Assert.IsType<ZLinkAggregatePrepareResult.Conflict>(
            await repository.PrepareAggregateAsync(
                request,
                callerCancellation.Token));

        var refreshed = new List<ZLinkAggregateParticipant>();
        foreach (var participant in participants)
        {
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(participant.Key)).Snapshot;
            refreshed.Add(participant with
            {
                ExpectedStoreVersion = authority.StoreVersion
            });
        }
        var retry = request with
        {
            AggregateId = Guid.NewGuid(),
            Participants = refreshed
        };
        Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(retry));
    }

    [Fact]
    public async Task SharedOpaqueProvider_CancellationAfterAggregateClaimCleansUnfencedChildren()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new CancelAfterAggregateClaimStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "claim-cancel-source");
        var targetOwner = await ClaimAsync(repository, "claim-cancel-target");
        var sourceDescriptor = Descriptor("claim-cancel-source", sourceOwner);
        var targetDescriptor = Descriptor("claim-cancel-target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var create = Reservation(
            "actor:claim-cancel",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(create));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x61 })).Snapshot;
        var participant = new ZLinkAggregateParticipant(
            create.Key,
            created.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            new byte[] { 0x62 },
            new byte[] { 0x63 });
        var request = AggregateRequest(
            [participant],
            targetDescriptor,
            targetOwner);
        var fence = new ZLinkAggregateFence(
            request.AggregateId,
            request.AggregateGeneration);
        using var callerCancellation = new CancellationTokenSource();
        provider.FailAggregateId = request.AggregateId;
        provider.CallerCancellation = callerCancellation;

        Assert.IsType<ZLinkAggregatePrepareResult.Conflict>(
            await repository.PrepareAggregateAsync(
                request,
                callerCancellation.Token));
        Assert.Equal(
            ZLinkAggregateCommitResult.Stale,
            await repository.CommitAggregateAsync(fence));
        Assert.DoesNotContain(
            await ScanKeysAsync(inner, AggregatePrefix(fence)),
            static key => key.Contains(
                ":participant:",
                StringComparison.Ordinal)
                || key.Contains(":inventory:", StringComparison.Ordinal));

        var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
            await repository.ReadAuthorityAsync(participant.Key)).Snapshot;
        var retry = request with
        {
            AggregateId = Guid.NewGuid(),
            Participants =
            [
                participant with
                {
                    ExpectedStoreVersion = authority.StoreVersion
                }
            ]
        };
        Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(retry));
    }

    [Fact]
    public async Task SharedOpaqueProvider_FirstAggregateUseReclaimsAbandonedStaging()
    {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var create = Reservation(
            "actor:staging-recovery",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(create));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x91 })).Snapshot;
        var participant = new ZLinkAggregateParticipant(
            create.Key,
            created.StoreVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            new byte[] { 0x92 },
            new byte[] { 0x93 });
        var abandoned = AggregateRequest(
            [participant],
            targetDescriptor,
            targetOwner);
        var abandonedFence = new ZLinkAggregateFence(
            abandoned.AggregateId,
            abandoned.AggregateGeneration);

        provider.ThrowAfterNextWrite = true;
        provider.SkipWritesBeforeThrow = 1;
        provider.BlockWritesAfterThrownResponse = true;
        await Assert.ThrowsAsync<IOException>(async () =>
            await repository.PrepareAggregateAsync(abandoned));
        provider.BlockWrites = false;
        await BackdateAggregateRootAsync(inner, abandonedFence);
        Assert.Equal(
            ZLinkOwnerLeaseReleaseResult.Released,
            await repository.ReleaseOwnerLeaseAsync(targetOwner));

        var recovering = new ZLinkProviderLocationRepository(inner);
        var next = abandoned with { AggregateId = Guid.NewGuid() };
        Assert.IsType<ZLinkAggregatePrepareResult.Conflict>(
            await recovering.PrepareAggregateAsync(next));

        Assert.DoesNotContain(
            await ScanKeysAsync(inner, AggregatePrefix(abandonedFence)),
            static key => key.Contains(":participant:", StringComparison.Ordinal));
        Assert.Equal(
            ZLinkAggregateCommitResult.Stale,
            await recovering.CommitAggregateAsync(abandonedFence));
    }

    [Fact]
    public async Task SharedOpaqueProvider_StaleParticipantDoesNotClaimStagingOrCapacity()
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(repository, "source-owner");
        var targetOwner = await ClaimAsync(repository, "target-owner");
        var sourceDescriptor = Descriptor("source", sourceOwner);
        var targetDescriptor = Descriptor("target", targetOwner);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var create = Reservation(
            "actor:stale-aggregate",
            sourceDescriptor,
            sourceOwner);
        var reserved = Assert.IsType<ZLinkObjectReserveResult.Reserved>(
            await repository.ReserveAsync(create));
        var created = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                reserved.Reservation,
                new byte[] { 0x94 })).Snapshot;
        var request = AggregateRequest(
            [
                new ZLinkAggregateParticipant(
                    create.Key,
                    created.StoreVersion,
                    ZLinkAuthorityGenerationTransition.NewOwner,
                    new byte[] { 0x95 },
                    new byte[] { 0x96 })
            ],
            targetDescriptor,
            targetOwner);
        _ = Assert.IsType<ZLinkAuthorityCompareExchangeResult.Stored>(
            await repository.CompareExchangeAuthorityAsync(
                create.Key,
                created.StoreVersion,
                new ZLinkAuthorityMutation.Put(
                    new byte[] { 0x97 },
                    ZLinkAuthorityGenerationTransition.Preserve,
                    null,
                    null)));

        Assert.IsType<ZLinkAggregatePrepareResult.Conflict>(
            await repository.PrepareAggregateAsync(request));
        Assert.Empty(await ScanKeysAsync(
            provider,
            AggregatePrefix(new ZLinkAggregateFence(
                request.AggregateId,
                request.AggregateGeneration))));
    }

    [Fact]
    public async Task SharedOpaqueProvider_TenThousandParticipantsUsePagedRowsAndSmallRoot()
    {
        const int participantCount = 10_000;
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(
            repository,
            "source-owner",
            TimeSpan.FromMinutes(15));
        var targetOwner = await ClaimAsync(
            repository,
            "target-owner",
            TimeSpan.FromMinutes(15));
        var sourceDescriptor = Descriptor(
            "source",
            sourceOwner,
            participantCount * 2);
        var targetDescriptor = Descriptor(
            "target",
            targetOwner,
            participantCount * 2);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = await SeedAuthoritiesAsync(
            provider,
            repository,
            sourceDescriptor,
            sourceOwner,
            participantCount);
        var request = AggregateRequest(
            participants,
            targetDescriptor,
            targetOwner);
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request));
        var root = Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(
                new ZLinkStoreKey(AggregatePrefix(prepared.Fence))));

        Assert.True(root.Value.Bytes.Length < 1024 * 1024);
        var preparedKeys = await ScanKeysAsync(
            provider,
            AggregatePrefix(prepared.Fence));
        var inventoryLeafKeys = preparedKeys
            .Where(static key => key.Contains(
                ":inventory:0:",
                StringComparison.Ordinal))
            .ToArray();
        Assert.Equal(10, inventoryLeafKeys.Length);
        var inventoryPageBytes =
            new Dictionary<string, byte[]>(StringComparer.Ordinal);
        foreach (var inventoryLeafKey in inventoryLeafKeys)
        {
            var page = Assert.IsType<ZLinkStoreReadResult.Found>(
                await provider.ReadAsync(
                    new ZLinkStoreKey(inventoryLeafKey)));
            Assert.InRange(page.Value.Bytes.Length, 1, 1024 * 1024);
            Assert.Equal(
                0x5A4C4956u,
                BinaryPrimitives.ReadUInt32BigEndian(
                    page.Value.Bytes.Span));
            Assert.Equal(1, page.Value.Bytes.Span[4]);
            Assert.Equal(2, page.Value.Bytes.Span[5]);
            Assert.InRange(
                BinaryPrimitives.ReadInt32BigEndian(
                    page.Value.Bytes.Span.Slice(22, sizeof(int))),
                1,
                1024);
            inventoryPageBytes[inventoryLeafKey] =
                page.Value.Bytes.ToArray();
        }
        Assert.Equal(
            participantCount,
            prepared.TargetAuthorityOwnerGenerations.Count);
        Assert.IsType<ZLinkAggregatePrepareResult.AlreadyPrepared>(
            await repository.PrepareAggregateAsync(request));
        foreach (var pair in inventoryPageBytes)
        {
            var repeated = Assert.IsType<ZLinkStoreReadResult.Found>(
                await provider.ReadAsync(new ZLinkStoreKey(pair.Key)));
            Assert.True(pair.Value.AsSpan().SequenceEqual(
                repeated.Value.Bytes.Span));
        }
        Assert.Equal(
            ZLinkAggregateCommitResult.Committed,
            await repository.CommitAggregateAsync(prepared.Fence));
        foreach (var participant in new[]
                 {
                     participants[0],
                     participants[participantCount / 2],
                     participants[^1]
                 })
        {
            var authority = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(participant.Key)).Snapshot;
            Assert.Equal(targetOwner.OwnerId, authority.OwnerId);
            Assert.True(participant.AuthorityPayload.Span.SequenceEqual(
                authority.Payload.Span));
        }
        Assert.DoesNotContain(
            await ScanKeysAsync(provider, AggregatePrefix(prepared.Fence)),
            static key => key.Contains(":participant:", StringComparison.Ordinal));
        Assert.DoesNotContain(
            await ScanKeysAsync(provider, AggregatePrefix(prepared.Fence)),
            static key => key.Contains(":inventory:", StringComparison.Ordinal));
    }

    [Fact]
    public async Task SharedOpaqueProvider_CommittedProjectionReadsOnlyInventoryPath()
    {
        const int participantCount = 10_000;
        const int projectedCount = 100;
        var inner = new ZLinkInMemoryProviderLocationStore();
        var provider = new ApplyThenThrowLocationStore(inner);
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(
            repository,
            "projection-source",
            TimeSpan.FromMinutes(15));
        var targetOwner = await ClaimAsync(
            repository,
            "projection-target",
            TimeSpan.FromMinutes(15));
        var sourceDescriptor = Descriptor(
            "projection-source",
            sourceOwner,
            participantCount * 2);
        var targetDescriptor = Descriptor(
            "projection-target",
            targetOwner,
            participantCount * 2);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = await SeedAuthoritiesAsync(
            inner,
            repository,
            sourceDescriptor,
            sourceOwner,
            participantCount);
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(
                AggregateRequest(
                    participants,
                    targetDescriptor,
                    targetOwner)));

        provider.ThrowAfterNextWrite = true;
        provider.BlockWritesAfterThrownResponse = true;
        await Assert.ThrowsAsync<IOException>(
            async () => await repository.CommitAggregateAsync(
                prepared.Fence));
        provider.ResetInventoryReadCount();
        for (var index = 0; index < projectedCount; index++)
        {
            var authority =
                Assert.IsType<ZLinkAuthorityReadResult.Found>(
                    await repository.ReadAuthorityAsync(
                        participants[index].Key)).Snapshot;
            Assert.Equal(targetOwner.OwnerId, authority.OwnerId);
        }

        Assert.InRange(
            provider.InventoryReadCount,
            projectedCount,
            projectedCount * 3);
        provider.BlockWrites = false;
        Assert.Equal(
            ZLinkAggregateCommitResult.AlreadyCommitted,
            await repository.CommitAggregateAsync(prepared.Fence));
    }

    [Fact]
    public async Task SharedOpaqueProvider_MissingOrCorruptInventoryPageIsDataLost()
    {
        foreach (var mutation in new[] { "missing", "corrupt" })
        {
            var fixture = await PrepareInventoryFixtureAsync(2);
            var inventoryKey = (await ScanKeysAsync(
                    fixture.Provider,
                    AggregatePrefix(fixture.Prepared.Fence)))
                .Single(static key => key.Contains(
                    ":inventory:0:",
                    StringComparison.Ordinal));
            var key = new ZLinkStoreKey(inventoryKey);
            var page = Assert.IsType<ZLinkStoreReadResult.Found>(
                await fixture.Provider.ReadAsync(key));
            ZLinkStoreMutation pageMutation;
            if (mutation == "missing")
                pageMutation = new ZLinkStoreMutation.Delete(key);
            else
            {
                var corrupted = page.Value.Bytes.ToArray();
                corrupted[^1] ^= 0x5A;
                pageMutation = new ZLinkStoreMutation.Put(
                    key,
                    corrupted,
                    null);
            }
            Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await fixture.Provider.WriteAsync(
                    new ZLinkStoreWriteRequest(
                        [new ZLinkStoreCondition.Version(
                            key,
                            page.Value.Version)],
                        [pageMutation])));

            await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
                async () => await fixture.Repository.CommitAggregateAsync(
                    fixture.Prepared.Fence));
        }
    }

    [Fact]
    public async Task SharedOpaqueProvider_AbortUsesInventoryWhenParticipantMetaIsMissing()
    {
        var fixture = await PrepareInventoryFixtureAsync(2);
        var keys = await ScanKeysAsync(
            fixture.Provider,
            AggregatePrefix(fixture.Prepared.Fence));
        var missingMeta = keys.Single(static key =>
            key.EndsWith(":participant:0:meta", StringComparison.Ordinal));
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await fixture.Provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [],
                    [new ZLinkStoreMutation.Delete(
                        new ZLinkStoreKey(missingMeta))])));

        Assert.Equal(
            ZLinkAggregateAbortResult.Aborted,
            await fixture.Repository.AbortAggregateAsync(
                fixture.Prepared.Fence));
        Assert.DoesNotContain(
            await ScanKeysAsync(
                fixture.Provider,
                AggregatePrefix(fixture.Prepared.Fence)),
            static key => key.Contains(
                ":participant:",
                StringComparison.Ordinal));
    }

    [Fact]
    public async Task SharedOpaqueProvider_AbortPreservesRowsWhenInventoryIsCorrupt()
    {
        var fixture = await PrepareInventoryFixtureAsync(2);
        var keys = await ScanKeysAsync(
            fixture.Provider,
            AggregatePrefix(fixture.Prepared.Fence));
        var inventoryKey = keys.Single(static key => key.Contains(
            ":inventory:0:",
            StringComparison.Ordinal));
        var key = new ZLinkStoreKey(inventoryKey);
        var page = Assert.IsType<ZLinkStoreReadResult.Found>(
            await fixture.Provider.ReadAsync(key));
        var corrupted = page.Value.Bytes.ToArray();
        corrupted[^1] ^= 0x5A;
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await fixture.Provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(
                        key,
                        page.Value.Version)],
                    [new ZLinkStoreMutation.Put(
                        key,
                        corrupted,
                        null)])));

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            async () => await fixture.Repository.AbortAggregateAsync(
                fixture.Prepared.Fence));
        Assert.Contains(
            await ScanKeysAsync(
                fixture.Provider,
                AggregatePrefix(fixture.Prepared.Fence)),
            static storedKey => storedKey.Contains(
                ":participant:",
                StringComparison.Ordinal));
    }

    [Theory]
    [InlineData("root")]
    [InlineData("participant-meta")]
    [InlineData("participant-payload")]
    public async Task SharedOpaqueProvider_DurableCorruptionIsTypedDataLost(
        string record)
    {
        var fixture = await PrepareInventoryFixtureAsync(2);
        var aggregatePrefix = AggregatePrefix(fixture.Prepared.Fence);
        var keys = await ScanKeysAsync(
            fixture.Provider,
            aggregatePrefix);
        var selected = record switch
        {
            "root" => aggregatePrefix,
            "participant-meta" => keys.Single(static key =>
                key.EndsWith(":participant:0:meta", StringComparison.Ordinal)),
            _ => keys.Single(static key =>
                key.EndsWith(
                    ":participant:0:payload",
                    StringComparison.Ordinal))
        };
        var key = new ZLinkStoreKey(selected);
        var current = Assert.IsType<ZLinkStoreReadResult.Found>(
            await fixture.Provider.ReadAsync(key));
        var corrupted = record == "participant-payload"
            ? current.Value.Bytes.ToArray()
            : "{not-valid-json"u8.ToArray();
        if (record == "participant-payload")
            corrupted[^1] ^= 0x5A;
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await fixture.Provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(
                        key,
                        current.Value.Version)],
                    [new ZLinkStoreMutation.Put(
                        key,
                        corrupted,
                        null)])));

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            async () => await fixture.Repository.CommitAggregateAsync(
                fixture.Prepared.Fence));
    }

    [Fact]
    public async Task SharedOpaqueProvider_CorruptAggregateRootBlocksStartupRecoveryWithoutCleanup()
    {
        var fixture = await PrepareInventoryFixtureAsync(2);
        var aggregatePrefix = AggregatePrefix(fixture.Prepared.Fence);
        var rootKey = new ZLinkStoreKey(aggregatePrefix);
        var root = Assert.IsType<ZLinkStoreReadResult.Found>(
            await fixture.Provider.ReadAsync(rootKey));
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await fixture.Provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(
                        rootKey,
                        root.Value.Version)],
                    [new ZLinkStoreMutation.Put(
                        rootKey,
                        "{not-valid-json"u8.ToArray(),
                        null)])));
        var keysBefore = await ScanKeysAsync(
            fixture.Provider,
            aggregatePrefix);
        var recovering = new ZLinkProviderLocationRepository(
            fixture.Provider);
        var next = fixture.Request with
        {
            AggregateId = Guid.NewGuid()
        };

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            async () => await recovering.PrepareAggregateAsync(next));
        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            async () => await recovering.PrepareAggregateAsync(next));

        Assert.Equal(
            keysBefore.Order(StringComparer.Ordinal),
            (await ScanKeysAsync(fixture.Provider, aggregatePrefix))
                .Order(StringComparer.Ordinal));
        Assert.IsType<ZLinkStoreReadResult.Found>(
            await fixture.Provider.ReadAsync(rootKey));
    }

    [Fact]
    public async Task SharedOpaqueProvider_ReorderedInventoryRootIsDataLost()
    {
        var fixture = await PrepareInventoryFixtureAsync(1025);
        var rootKey = new ZLinkStoreKey(
            AggregatePrefix(fixture.Prepared.Fence));
        var root = Assert.IsType<ZLinkStoreReadResult.Found>(
            await fixture.Provider.ReadAsync(rootKey));
        var json = JsonNode.Parse(root.Value.Bytes.Span)
                   ?? throw new InvalidDataException();
        var inventory = Property(
            json.AsObject(),
            "inventory").AsObject();
        var topPages = Property(inventory, "topPages").AsArray();
        Assert.Equal(2, topPages.Count);
        var first = topPages[0]!.DeepClone();
        topPages[0] = topPages[1]!.DeepClone();
        topPages[1] = first;
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await fixture.Provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(
                        rootKey,
                        root.Value.Version)],
                    [new ZLinkStoreMutation.Put(
                        rootKey,
                        Encoding.UTF8.GetBytes(json.ToJsonString()),
                        null)])));

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            async () => await fixture.Repository.CommitAggregateAsync(
                fixture.Prepared.Fence));
    }

    [Fact]
    public async Task SharedOpaqueProvider_ParticipantPermutationUsesSameFingerprint()
    {
        var fixture = await PrepareInventoryFixtureAsync(3);
        var permuted = fixture.Request with
        {
            Participants = fixture.Request.Participants
                .Reverse()
                .ToArray()
        };

        var repeated =
            Assert.IsType<ZLinkAggregatePrepareResult.AlreadyPrepared>(
                await fixture.Repository.PrepareAggregateAsync(permuted));
        Assert.Equal(fixture.Prepared.Fence, repeated.Fence);
        Assert.Equal(
            fixture.Prepared.TargetAuthorityOwnerGenerations
                .OrderBy(static pair => pair.Key.Value),
            repeated.TargetAuthorityOwnerGenerations
                .OrderBy(static pair => pair.Key.Value));
    }

    [Fact]
    public async Task SharedOpaqueProvider_InflatedInventoryTotalIsRejectedBeforeAllocation()
    {
        var fixture = await PrepareInventoryFixtureAsync(2);
        var rootKey = new ZLinkStoreKey(
            AggregatePrefix(fixture.Prepared.Fence));
        var root = Assert.IsType<ZLinkStoreReadResult.Found>(
            await fixture.Provider.ReadAsync(rootKey));
        var json = JsonNode.Parse(root.Value.Bytes.Span)
                   ?? throw new InvalidDataException();
        SetJsonNumber(
            json.AsObject(),
            "participantCount",
            int.MaxValue);
        SetJsonNumber(
            Property(json.AsObject(), "inventory").AsObject(),
            "totalCount",
            int.MaxValue);
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await fixture.Provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(
                        rootKey,
                        root.Value.Version)],
                    [new ZLinkStoreMutation.Put(
                        rootKey,
                        Encoding.UTF8.GetBytes(json.ToJsonString()),
                        null)])));

        await Assert.ThrowsAsync<ZLinkRelocationDataLostException>(
            async () => await fixture.Repository.CommitAggregateAsync(
                fixture.Prepared.Fence));
    }

    private static async ValueTask<(
        ZLinkInMemoryProviderLocationStore Provider,
        ZLinkProviderLocationRepository Repository,
        ZLinkAggregatePrepareResult.Prepared Prepared,
        ZLinkAggregatePrepareRequest Request)>
        PrepareInventoryFixtureAsync(int participantCount)
    {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var sourceOwner = await ClaimAsync(
            repository,
            $"source-owner-{participantCount}",
            TimeSpan.FromMinutes(15));
        var targetOwner = await ClaimAsync(
            repository,
            $"target-owner-{participantCount}",
            TimeSpan.FromMinutes(15));
        var sourceDescriptor = Descriptor(
            $"source-{participantCount}",
            sourceOwner,
            participantCount * 2);
        var targetDescriptor = Descriptor(
            $"target-{participantCount}",
            targetOwner,
            participantCount * 2);
        _ = await repository.UpdateMeshNodeAsync(
            sourceDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        _ = await repository.UpdateMeshNodeAsync(
            targetDescriptor,
            ZLinkLocationWriteIntent.NewClaim);
        var participants = await SeedAuthoritiesAsync(
            provider,
            repository,
            sourceDescriptor,
            sourceOwner,
            participantCount);
        var request = AggregateRequest(
            participants,
            targetDescriptor,
            targetOwner);
        var prepared = Assert.IsType<ZLinkAggregatePrepareResult.Prepared>(
            await repository.PrepareAggregateAsync(request));
        return (provider, repository, prepared, request);
    }

    private static JsonNode Property(
        JsonObject json,
        string propertyName) =>
        json.First(pair => string.Equals(
                pair.Key,
                propertyName,
                StringComparison.OrdinalIgnoreCase))
            .Value
        ?? throw new InvalidDataException(
            $"JSON property '{propertyName}' is null.");

    private static async ValueTask<ZLinkAggregateParticipant[]>
        SeedAuthoritiesAsync(
            IZLinkLocationStore provider,
            IZLinkLocationRepository repository,
            ZLinkMeshNodeDescriptor sourceDescriptor,
            ZLinkLocationOwnerToken sourceOwner,
            int participantCount)
    {
        const int participantsPerBatch = 300;
        const string templateActorId = "actor:ten-thousand:0";
        var templateRequest = Reservation(
            templateActorId,
            sourceDescriptor,
            sourceOwner);
        var templateReservation =
            Assert.IsType<ZLinkObjectReserveResult.Reserved>(
                await repository.ReserveAsync(templateRequest));
        var template = Assert.IsType<ZLinkObjectCommitResult.Committed>(
            await repository.CommitAsync(
                templateReservation.Reservation,
                new byte[] { 0x69 })).Snapshot;
        var templateMeta = Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(AuthorityMetaKey(templateActorId)));
        var templateGeneration = Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(AuthorityGenerationKey(templateActorId)));
        var participants =
            new ZLinkAggregateParticipant[participantCount];
        participants[0] = AggregateParticipant(
            templateRequest.Key,
            template.StoreVersion,
            index: 0);

        for (var offset = 1;
             offset < participantCount;
             offset += participantsPerBatch)
        {
            var count = Math.Min(
                participantsPerBatch,
                participantCount - offset);
            var mutations = new List<ZLinkStoreMutation>(count * 3);
            var metaKeys = new ZLinkStoreKey[count];
            for (var batchIndex = 0;
                 batchIndex < count;
                 batchIndex++)
            {
                var index = offset + batchIndex;
                var actorId = $"actor:ten-thousand:{index}";
                var ownerGeneration = checked((ulong)index + 1);
                var metaKey = AuthorityMetaKey(actorId);
                metaKeys[batchIndex] = metaKey;
                mutations.Add(new ZLinkStoreMutation.Put(
                    metaKey,
                    WithAuthorityOwnerGeneration(
                        templateMeta.Value.Bytes,
                        ownerGeneration),
                    null));
                mutations.Add(new ZLinkStoreMutation.Put(
                    AuthorityPayloadKey(actorId),
                    new byte[] { 0x69 },
                    null));
                mutations.Add(new ZLinkStoreMutation.Put(
                    AuthorityGenerationKey(actorId),
                    WithAuthorityOwnerGeneration(
                        templateGeneration.Value.Bytes,
                        ownerGeneration),
                    null));
            }
            var applied = Assert.IsType<ZLinkStoreWriteResult.Applied>(
                await provider.WriteAsync(
                    new ZLinkStoreWriteRequest([], mutations)));
            for (var batchIndex = 0;
                 batchIndex < count;
                 batchIndex++)
            {
                var index = offset + batchIndex;
                participants[index] = AggregateParticipant(
                    new ZLinkAuthorityKey($"actor:ten-thousand:{index}"),
                    applied.PutVersions[metaKeys[batchIndex]].Value,
                    index);
            }
        }

        var capacityKey = CapacityKey(sourceDescriptor);
        var capacity = Assert.IsType<ZLinkStoreReadResult.Found>(
            await provider.ReadAsync(capacityKey));
        var capacityJson = JsonNode.Parse(capacity.Value.Bytes.Span)
                           ?? throw new InvalidDataException();
        SetJsonNumber(
            capacityJson.AsObject(),
            "actorsActive",
            (long)participantCount);
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await provider.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(
                        capacityKey,
                        capacity.Value.Version)],
                    [new ZLinkStoreMutation.Put(
                        capacityKey,
                        Encoding.UTF8.GetBytes(capacityJson.ToJsonString()),
                        null)])));
        return participants;
    }

    private static ZLinkAggregateParticipant AggregateParticipant(
        ZLinkAuthorityKey key,
        string storeVersion,
        int index) =>
        new(
            key,
            storeVersion,
            ZLinkAuthorityGenerationTransition.NewOwner,
            Enumerable.Repeat((byte)(index % 251), 32).ToArray(),
            new byte[] { 0x51, (byte)index });

    private static byte[] WithAuthorityOwnerGeneration(
        ReadOnlyMemory<byte> encoded,
        ulong generation)
    {
        var json = JsonNode.Parse(encoded.Span)
                   ?? throw new InvalidDataException();
        SetJsonNumber(
            json.AsObject(),
            "authorityOwnerGeneration",
            generation);
        return Encoding.UTF8.GetBytes(json.ToJsonString());
    }

    private static void SetJsonNumber(
        JsonObject json,
        string propertyName,
        long value)
    {
        var key = json.First(pair => string.Equals(
            pair.Key,
            propertyName,
            StringComparison.OrdinalIgnoreCase)).Key;
        json[key] = value;
    }

    private static void SetJsonNumber(
        JsonObject json,
        string propertyName,
        ulong value)
    {
        var key = json.First(pair => string.Equals(
            pair.Key,
            propertyName,
            StringComparison.OrdinalIgnoreCase)).Key;
        json[key] = value;
    }

    private static ZLinkStoreKey AuthorityMetaKey(string actorId) =>
        new($"zlink:v11:authority:meta:{actorId}");

    private static ZLinkStoreKey AuthorityPayloadKey(string actorId) =>
        new($"zlink:v11:authority:payload:{StoreSegment(actorId)}");

    private static ZLinkStoreKey AuthorityGenerationKey(string actorId) =>
        new($"zlink:v11:authority:generation:{StoreSegment(actorId)}");

    private static ZLinkStoreKey CapacityKey(
        ZLinkMeshNodeDescriptor descriptor) =>
        new($"zlink:v11:capacity:{StoreSegment(descriptor.MeshName)}"
            + $"{StoreSegment(descriptor.Rid.ToHex())}"
            + descriptor.LifecycleGeneration);

    private static string StoreSegment(string value) =>
        $"{Encoding.UTF8.GetByteCount(value)}:{value}:";

    private static async ValueTask<ZLinkLocationOwnerToken> ClaimAsync(
        IZLinkLocationRepository repository,
        string ownerId,
        TimeSpan? leaseTtl = null) =>
        Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await repository.ClaimOwnerLeaseAsync(
                ownerId,
                leaseTtl ?? TimeSpan.FromMinutes(2))).Token;

    private static ZLinkMeshNodeDescriptor Descriptor(
        string node,
        ZLinkLocationOwnerToken owner,
        int actorLimit = 2048) =>
        new(
            "play",
            RoutingId.From(node),
            1,
            1,
            $"tcp://127.0.0.1:{(node == "source" ? 7101 : 7102)}",
            new Dictionary<string, int>(StringComparer.Ordinal)
            {
                ["play"] = 100
            },
            string.Empty,
            owner.OwnerId,
            owner.LeaseGeneration,
            default)
        {
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities =
            [
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0),
                new ZLinkObjectCapability(
                    ZLinkPlacementObjectKind.UserSpot,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    100)
            ],
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, actorLimit),
                new ZLinkPopulationCapacity(0, 0, 2048),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        "player",
                        0,
                        0,
                        100)
                ]),
            EntrySpotId =
                $"play-entry-00000000-0000-4000-8000-{(node == "source" ? "000000000001" : "000000000002")}",
            State = ZLinkFrameworkRuntimeState.Serving
        };

    private static ZLinkObjectReservationRequest Reservation(
        string actorId,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind objectKind =
            ZLinkPlacementObjectKind.Actor)
    {
        var intent = Encoding.UTF8.GetBytes($"create:{actorId}");
        return new ZLinkObjectReservationRequest(
            objectKind,
            new ZLinkAuthorityKey(actorId),
            "player",
            $"inline:{actorId}",
            SHA256.HashData(intent),
            intent.Length,
            new ZLinkMeshNodeDescriptorKey(
                descriptor.MeshName,
                descriptor.Rid),
            descriptor.LifecycleGeneration,
            owner,
            new byte[] { 0x11 },
            objectKind == ZLinkPlacementObjectKind.Actor
                ? new ZLinkCapacityVector(1, 0, null)
                : new ZLinkCapacityVector(
                    0,
                    1,
                    new ZLinkSpotTypeCapacityDelta(
                        objectKind,
                        "player",
                        1)));
    }

    private static ZLinkAggregatePrepareRequest AggregateRequest(
        IReadOnlyList<ZLinkAggregateParticipant> participants,
        ZLinkMeshNodeDescriptor targetDescriptor,
        ZLinkLocationOwnerToken targetOwner) =>
        new(
            Guid.NewGuid(),
            1,
            participants,
            SHA256.HashData([0x41, 0x47, 0x47]),
            new ZLinkMeshNodeDescriptorKey(
                targetDescriptor.MeshName,
                targetDescriptor.Rid),
            targetDescriptor.LifecycleGeneration,
            new ZLinkCapacityVector(participants.Count, 0, null),
            targetOwner);

    private static string AggregatePrefix(ZLinkAggregateFence fence) =>
        $"zlink:v11:aggregate:{fence.AggregateId:N}:"
        + fence.AggregateGeneration;

    private static async ValueTask<IReadOnlyList<string>> ScanKeysAsync(
        IZLinkLocationStore store,
        string prefix)
    {
        var keys = new List<string>();
        ZLinkStoreScanCursor? cursor = null;
        do
        {
            var result = Assert.IsType<ZLinkStoreScanResult.Page>(
                await store.ScanAsync(
                    new ZLinkStoreScanRequest(prefix, cursor, 1000)));
            keys.AddRange(result.Value.Items.Select(pair => pair.Key.Value));
            cursor = result.Value.NextCursor;
        } while (cursor is not null);
        return keys;
    }

    private static async ValueTask BackdateAggregateRootAsync(
        IZLinkLocationStore store,
        ZLinkAggregateFence fence)
    {
        var rootKey = new ZLinkStoreKey(AggregatePrefix(fence));
        var read = Assert.IsType<ZLinkStoreReadResult.Found>(
            await store.ReadAsync(rootKey));
        var json = JsonNode.Parse(read.Value.Bytes.Span)
                   ?? throw new InvalidDataException();
        var property = json.AsObject().First(pair =>
            string.Equals(
                pair.Key,
                "stagedAt",
                StringComparison.OrdinalIgnoreCase)).Key;
        json[property] = DateTimeOffset.UtcNow
            .Subtract(TimeSpan.FromDays(2));
        Assert.IsType<ZLinkStoreWriteResult.Applied>(
            await store.WriteAsync(
                new ZLinkStoreWriteRequest(
                    [new ZLinkStoreCondition.Version(
                        rootKey,
                        read.Value.Version)],
                    [new ZLinkStoreMutation.Put(
                        rootKey,
                        Encoding.UTF8.GetBytes(json.ToJsonString()),
                        null)])));
    }

    private sealed class ApplyThenThrowLocationStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        public bool ThrowAfterNextWrite { get; set; }
        public int SkipWritesBeforeThrow { get; set; }
        public bool BlockWritesAfterThrownResponse { get; set; }
        public bool BlockWrites { get; set; }
        public int DeleteBatchCalls { get; private set; }
        private int _inventoryReadCount;

        public int InventoryReadCount =>
            Volatile.Read(ref _inventoryReadCount);

        public void ResetInventoryReadCount() =>
            Volatile.Write(ref _inventoryReadCount, 0);

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default)
        {
            if (key.Value.Contains(":inventory:", StringComparison.Ordinal))
                Interlocked.Increment(ref _inventoryReadCount);
            return await inner.ReadAsync(key, cancellationToken);
        }

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default)
        {
            if (BlockWrites)
                throw new IOException("The provider is unavailable.");
            if (request.Mutations.Any(static mutation =>
                    mutation is ZLinkStoreMutation.Delete))
                DeleteBatchCalls++;
            var result = await inner.WriteAsync(request, cancellationToken);
            if (ThrowAfterNextWrite)
            {
                if (SkipWritesBeforeThrow > 0)
                {
                    SkipWritesBeforeThrow--;
                    return result;
                }
                ThrowAfterNextWrite = false;
                BlockWrites = BlockWritesAfterThrownResponse;
                throw new IOException("The provider response was lost.");
            }
            return result;
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);
    }

    private sealed class ExpireFirstSnapshotLocationStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private bool _firstPageReturned;
        private bool _expired;

        internal int ScanCalls { get; private set; }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default) =>
            inner.WriteAsync(request, cancellationToken);

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default)
        {
            ScanCalls++;
            if (_firstPageReturned && !_expired && request.Cursor is not null)
            {
                _expired = true;
                return ValueTask.FromResult<ZLinkStoreScanResult>(
                    new ZLinkStoreScanResult.Expired());
            }

            if (!_firstPageReturned && request.Cursor is null)
                _firstPageReturned = true;
            return inner.ScanAsync(
                request with { Limit = 1 },
                cancellationToken);
        }
    }

    private sealed class CancelAfterFirstAggregateFenceStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private int _failed;

        internal Guid FailAggregateId { get; set; }

        internal CancellationTokenSource? CallerCancellation { get; set; }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAsync(key, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default)
        {
            var result = await inner.WriteAsync(request, cancellationToken);
            if (FailAggregateId != Guid.Empty
                && Volatile.Read(ref _failed) == 0
                && request.Mutations
                    .OfType<ZLinkStoreMutation.Put>()
                    .Any(ContainsAggregateFence)
                && Interlocked.Exchange(ref _failed, 1) == 0)
            {
                CallerCancellation?.Cancel();
                throw new OperationCanceledException(
                    CallerCancellation?.Token
                    ?? new CancellationToken(canceled: true));
            }
            return result;
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);

        private bool ContainsAggregateFence(ZLinkStoreMutation.Put put)
        {
            return put.Key.Value.Contains(
                    ":authority:meta:",
                    StringComparison.Ordinal);
        }
    }

    private sealed class CancelAfterAggregateClaimStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private int _cancelled;

        internal Guid FailAggregateId { get; set; }

        internal CancellationTokenSource? CallerCancellation { get; set; }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAsync(key, cancellationToken);

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default)
        {
            var result = await inner.WriteAsync(request, cancellationToken);
            var aggregateRoot =
                $"zlink:v11:aggregate:{FailAggregateId:N}:1";
            if (FailAggregateId != Guid.Empty
                && Volatile.Read(ref _cancelled) == 0
                && request.Mutations
                    .OfType<ZLinkStoreMutation.Put>()
                    .Any(put => string.Equals(
                        put.Key.Value,
                        aggregateRoot,
                        StringComparison.Ordinal))
                && Interlocked.Exchange(ref _cancelled, 1) == 0)
                CallerCancellation?.Cancel();
            return result;
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);
    }

    private sealed class ConcurrencyTrackingLocationStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        private int _activeReads;
        private int _activeWrites;
        private int _maximumConcurrentReads;
        private int _maximumConcurrentWrites;

        public int MaximumConcurrentReads =>
            Volatile.Read(ref _maximumConcurrentReads);

        public int MaximumConcurrentWrites =>
            Volatile.Read(ref _maximumConcurrentWrites);

        public void Reset()
        {
            Volatile.Write(ref _maximumConcurrentReads, 0);
            Volatile.Write(ref _maximumConcurrentWrites, 0);
        }

        public async ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default)
        {
            var active = Interlocked.Increment(ref _activeReads);
            UpdateMaximum(ref _maximumConcurrentReads, active);
            try
            {
                await Task.Delay(2, cancellationToken);
                return await inner.ReadAsync(key, cancellationToken);
            }
            finally
            {
                Interlocked.Decrement(ref _activeReads);
            }
        }

        public async ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default)
        {
            var active = Interlocked.Increment(ref _activeWrites);
            UpdateMaximum(ref _maximumConcurrentWrites, active);
            try
            {
                await Task.Delay(2, cancellationToken);
                return await inner.WriteAsync(request, cancellationToken);
            }
            finally
            {
                Interlocked.Decrement(ref _activeWrites);
            }
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);

        private static void UpdateMaximum(ref int maximum, int value)
        {
            var observed = Volatile.Read(ref maximum);
            while (observed < value)
            {
                var prior = Interlocked.CompareExchange(
                    ref maximum,
                    value,
                    observed);
                if (prior == observed) return;
                observed = prior;
            }
        }
    }

    private sealed class AggregateCommitConflictOnceLocationStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        public bool ConflictNextAggregateCommit { get; set; }

        public int AggregateCommitAttempts { get; private set; }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default)
        {
            if (request.Mutations
                .OfType<ZLinkStoreMutation.Put>()
                .Any(static put =>
                    put.Key.Value.StartsWith(
                        "zlink:v11:aggregate:",
                        StringComparison.Ordinal)
                    && !put.Key.Value.Contains(
                        ":participant:",
                        StringComparison.Ordinal)
                    && Encoding.UTF8.GetString(put.Bytes.Span)
                        .Contains(
                            "\"status\":2",
                            StringComparison.Ordinal))
                && request.Mutations
                    .OfType<ZLinkStoreMutation.Put>()
                    .Any(static put => put.Key.Value.StartsWith(
                        "zlink:v11:capacity:",
                        StringComparison.Ordinal)))
            {
                AggregateCommitAttempts++;
                if (ConflictNextAggregateCommit)
                {
                    ConflictNextAggregateCommit = false;
                    return ValueTask.FromResult<ZLinkStoreWriteResult>(
                        new ZLinkStoreWriteResult.Conflict(
                            DateTimeOffset.UtcNow));
                }
            }
            return inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);
    }

    private sealed class AuthorityCapacityConflictOnceLocationStore(
        IZLinkLocationStore inner) : IZLinkLocationStore
    {
        public bool ConflictNextAuthorityMove { get; set; }

        public int AuthorityMoveAttempts { get; private set; }

        public void ResetAuthorityMoveAttempts() => AuthorityMoveAttempts = 0;

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default) =>
            inner.ReadAsync(key, cancellationToken);

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default)
        {
            var hasAuthorityPut = request.Mutations
                .OfType<ZLinkStoreMutation.Put>()
                .Any(static put => put.Key.Value.StartsWith(
                    "zlink:v11:authority:",
                    StringComparison.Ordinal));
            var hasCapacityPut = request.Mutations
                .OfType<ZLinkStoreMutation.Put>()
                .Any(static put => put.Key.Value.StartsWith(
                    "zlink:v11:capacity:",
                    StringComparison.Ordinal));
            if (hasAuthorityPut && hasCapacityPut)
            {
                AuthorityMoveAttempts++;
                if (ConflictNextAuthorityMove)
                {
                    ConflictNextAuthorityMove = false;
                    return ValueTask.FromResult<ZLinkStoreWriteResult>(
                        new ZLinkStoreWriteResult.Conflict(
                            DateTimeOffset.UtcNow));
                }
            }
            return inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default) =>
            inner.ScanAsync(request, cancellationToken);
    }
}
