using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Locations;

public sealed class LocationContracts
{
    private static readonly DateTimeOffset StoreNow =
        new(2026, 7, 2, 0, 0, 0, TimeSpan.Zero);

    [Fact]
    public void Location_contract_excludes_compatibility_lease_and_slot_allocation_surface()
    {
        var assembly = typeof(IZLinkLocationRepository).Assembly;
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkRoutingIdSlotAllocationStore"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkAllocatedRoutingIdProvider"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkRoutingIdSlotAcquireResult"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkOwnerLeaseSnapshot"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkOwnerLeaseRenewal"));

        var publicMethods =
            typeof(Zlink.Framework.Locations.Redis.ZLinkRedisLocationStore)
            .GetMethods();
        var publicMethodNames = publicMethods
            .Select(static method => method.Name)
            .ToHashSet(StringComparer.Ordinal);
        Assert.DoesNotContain("AcquireRoutingIdSlotAsync", publicMethodNames);
        Assert.DoesNotContain("ListRoutingIdSlotsAsync", publicMethodNames);
        Assert.DoesNotContain("ListOwnerLeasesAsync", publicMethodNames);
        Assert.DoesNotContain("RemoveOwnerLeaseAsync", publicMethodNames);

        Assert.DoesNotContain(
            publicMethodNames,
            name => name.Contains("OwnerLease", StringComparison.Ordinal)
                    || name.Contains("Authority", StringComparison.Ordinal)
                    || name.Contains("Aggregate", StringComparison.Ordinal)
                    || name.Contains("Capacity", StringComparison.Ordinal));
    }

    [Fact]
    public void Location_options_do_not_expose_a_spot_mesh_route_mapping()
    {
        Assert.DoesNotContain(
            typeof(ZLinkLocationOptions).GetMethods(),
            static candidate => candidate.Name == "MapSpotMesh" + "ToRouteChannel");
    }

    [Fact]
    public async Task Location_store_combines_authority_and_owner_lease_transaction_domain()
    {
        var assembly = typeof(IZLinkLocationRepository).Assembly;
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkAuthorityStore"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkMeshNodeLocationStore"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkOwnerLeaseStore"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkClientServerLocationStore"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkFanoutLocationStore"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkLocationChangeStampStore"));
        Assert.Empty(
            assembly.GetExportedTypes()
                .Where(static type =>
                    type.IsInterface
                    && type.Namespace == "Zlink.Framework.Contracts.Locations"
                    && type.Name.EndsWith("Store", StringComparison.Ordinal))
                .ToArray());
        Assert.Contains(
            typeof(IZLinkLocationRepository).GetMethods(),
            static method => method.Name == nameof(IZLinkLocationRepository.CompareExchangeAuthorityAsync));
        Assert.DoesNotContain(
            typeof(IZLinkLocationRepository).GetMethods(),
            static method => method.Name.Contains(
                "Preparing",
                StringComparison.Ordinal));
        Assert.NotNull(typeof(ZLinkAuthorityMutation).GetNestedType(
            nameof(ZLinkAuthorityMutation.Restore)));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkLocationChangeScopeKind"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkLocationChangeStampScope"));

        // Owner lease: the provider issues the exact generation token and
        // returns its own clock with each read.
        var leases = new ExampleOwnerLeaseStore();
        var leaseClaim = Assert.IsType<ZLinkOwnerLeaseClaimResult.Claimed>(
            await leases.ClaimOwnerLeaseAsync(
                "owner-b", TimeSpan.FromSeconds(15)));
        var read = Assert.IsType<ZLinkOwnerLeaseReadResult.Found>(
            await leases.ReadOwnerLeaseAsync("owner-b"));
        Assert.Equal(StoreNow, read.StoreNow);
        Assert.Equal(leaseClaim.Token, read.Token);
    }

    [Fact]
    public async Task MeshNode_lists_are_paged_and_object_authority_is_opaque()
    {
        // One physical store registers for every role at once:
        // AddLocationStore takes a single IZLinkLocationRepository instance the
        // way codecs take serializer instances ??the framework surface
        // never names a concrete backend.

        var meshNodes = new ExampleMeshNodeLocationStore();
        await meshNodes.UpdateMeshNodeAsync(MakeDescriptor("owner-a"), ZLinkLocationWriteIntent.NewClaim);

        // Descriptor lists are one point-in-time snapshot per mesh by
        // contract ??reconcile diffs need one consistent list, never pages.
        var descriptors = await meshNodes.ListMeshNodesAsync(
            "play",
            new ZLinkPageRequest());
        Assert.Single(descriptors.Items);

        Assert.DoesNotContain(
            typeof(IZLinkLocationRepository).GetMethods(),
            static method => method.Name.Contains("Spot", StringComparison.Ordinal)
                             || method.Name.Contains("Actor", StringComparison.Ordinal));

        // Spot and Actor ownership is available only through the opaque
        // authority surface. Object-specific projection stores are not part
        // of the public contract.
        Assert.DoesNotContain(
            typeof(IZLinkLocationRepository).GetMethods(),
            static method => method.Name is "ListSpotsAsync" or "ListActorsAsync");
        Assert.Null(typeof(IZLinkLocationRepository).Assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkInstanceSpotLocationStore"));
        Assert.Null(typeof(IZLinkLocationRepository).Assembly.GetType(
            "Zlink.Framework.Contracts.Locations.InstanceSpotLocation"));
    }

    [Fact]
    public void Object_and_mesh_routes_are_not_public_resolvers()
    {
        var assembly = typeof(IZLinkLocationRepository).Assembly;
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkMeshNodeLocationResolver"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkSpotHandleResolver"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.IZLinkActorSpotHandleResolver"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.SpotHandle"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkLocationKey"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkLocationKind"));
        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkRouteKind"));
    }

    [Fact]
    public void Runtime_location_projections_are_not_exported()
    {
        var exportedTypeNames = typeof(IZLinkLocationRepository).Assembly
            .GetExportedTypes()
            .Select(static type => type.FullName)
            .ToHashSet(StringComparer.Ordinal);

        Assert.DoesNotContain(typeof(ZLinkSpotLocation).FullName, exportedTypeNames);
        Assert.DoesNotContain(typeof(ZLinkActorLocation).FullName, exportedTypeNames);
        Assert.DoesNotContain(typeof(ZLinkSpotLocationKey).FullName, exportedTypeNames);
        Assert.DoesNotContain(typeof(ZLinkActorLocationKey).FullName, exportedTypeNames);
        Assert.DoesNotContain(typeof(ZLinkLocationAutoConnectType).FullName, exportedTypeNames);
        foreach (var internalType in new[]
                 {
                     "ZLinkMeshNodeDescriptor",
                     "ZLinkMeshNodeDescriptorKey",
                     "ZLinkClientServerServerDescriptor",
                     "ZLinkFanoutPublisherDescriptor",
                     "ZLinkLocationOwnerToken",
                     "ZLinkOwnerLeaseClaimResult",
                     "ZLinkOwnerLeaseRenewResult",
                     "ZLinkOwnerLeaseReleaseResult",
                     "ZLinkOwnerLeaseReadResult",
                     "ZLinkLocationWriteResult",
                     "ZLinkLocationWriteStatus",
                     "ZLinkLocationWriteIntent",
                     "ZLinkPlacementCapacity",
                     "ZLinkPopulationCapacity",
                     "ZLinkSpotTypeCapacity",
                     "ZLinkActivationConcurrency",
                     "ZLinkPlacementObjectKind",
                     "ZLinkObjectCapability",
                     "ZLinkObjectMaintenancePolicyKind",
                     "ZLinkMeshNodeObjectRole"
                 })
        {
            Assert.DoesNotContain(
                $"Zlink.Framework.Contracts.Locations.{internalType}",
                exportedTypeNames);
        }

        Assert.DoesNotContain(
            typeof(IZLinkLocationRuntimeQuery).GetMethods(),
            static method => method.Name == "ListMeshNodeDescriptorsAsync");
    }

    [Fact]
    [ContractExample(
        typeof(IZLinkLocationRuntimeQuery),
        typeof(IZLinkLocationReadiness))]
    public async Task Runtime_query_reads_store_directly_and_change_stamp_is_optional()
    {
        var query = new ExampleLocationRuntimeQuery();
        var readiness = new ExampleLocationReadiness(query);

        var status = await query.GetStatusAsync();
        Assert.True(status.StoreHealthy);
        Assert.True(status.OwnerLeaseHealthy);

        var ready = await readiness.IsPeerReadyAsync("play", ZLinkLocationRole.Router);
        Assert.True(ready);

        // Runtime query never goes through a cache, so it takes no freshness.
        var topology = await query.ListTopologyAsync(new ZLinkLocationTopologyFilter(MeshName: "play"));
        Assert.Single(topology.Items);

        var summaries = await query.ListServiceSummariesAsync(
            new ZLinkLocationServiceSummaryFilter(MeshName: "play"));
        Assert.Single(summaries.Items);

        // A poller skips the full list query while the stamp is unchanged.
        // The stamp is an optimization, never a correctness authority.
        var stamps = new ExampleChangeStampStore();
        var stamp = await stamps.GetMeshNodeChangeStampAsync("play");
        Assert.Equal(1UL, stamp);
    }

    private static ZLinkMeshNodeDescriptor MakeDescriptor(string ownerId) => new(
        "play",
        RoutingId.From("node-1"),
        LifecycleGeneration: 1,
        DescriptorRevision: 1,
        "tcp://127.0.0.1:5001",
        new Dictionary<string, int>(StringComparer.Ordinal) { ["play"] = 100 },
        SecurityIdentity: "cluster-a",
        OwnerId: ownerId,
        LeaseGeneration: 1,
        UpdatedAt: StoreNow);

    private sealed class ExampleOwnerLeaseStore : LocationStoreContractExample
    {
        private readonly Dictionary<string, Lease> _leases = [];
        private long _generation;

        public override ValueTask<ZLinkOwnerLeaseClaimResult> ClaimOwnerLeaseAsync(
            string ownerId,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default)
        {
            if (_leases.TryGetValue(ownerId, out var current)
                && current.LeaseExpiresAt > StoreNow)
                return ValueTask.FromResult<ZLinkOwnerLeaseClaimResult>(
                    new ZLinkOwnerLeaseClaimResult.Conflict());
            var expiresAt = StoreNow + leaseTtl;
            var token = new ZLinkLocationOwnerToken(ownerId, ++_generation);
            _leases[ownerId] = new Lease(token.LeaseGeneration, expiresAt);
            return ValueTask.FromResult<ZLinkOwnerLeaseClaimResult>(
                new ZLinkOwnerLeaseClaimResult.Claimed(
                    token, expiresAt, StoreNow));
        }

        public override ValueTask<ZLinkOwnerLeaseReadResult> ReadOwnerLeaseAsync(
            string ownerId,
            CancellationToken cancellationToken = default)
        {
            if (!_leases.TryGetValue(ownerId, out var lease)
                || lease.LeaseExpiresAt <= StoreNow)
                return ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                    new ZLinkOwnerLeaseReadResult.Missing());
            return ValueTask.FromResult<ZLinkOwnerLeaseReadResult>(
                new ZLinkOwnerLeaseReadResult.Found(
                    new ZLinkLocationOwnerToken(
                        ownerId, lease.LeaseGeneration),
                    lease.LeaseExpiresAt,
                    StoreNow));
        }

        public override ValueTask<ZLinkOwnerLeaseRenewResult> RenewOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            TimeSpan leaseTtl,
            CancellationToken cancellationToken = default)
        {
            if (!_leases.TryGetValue(token.OwnerId, out var lease)
                || lease.LeaseGeneration != token.LeaseGeneration)
                return ValueTask.FromResult<ZLinkOwnerLeaseRenewResult>(
                    new ZLinkOwnerLeaseRenewResult.Stale());
            var expiresAt = StoreNow + leaseTtl;
            _leases[token.OwnerId] = lease with
            {
                LeaseExpiresAt = expiresAt
            };
            return ValueTask.FromResult<ZLinkOwnerLeaseRenewResult>(
                new ZLinkOwnerLeaseRenewResult.Renewed(
                    expiresAt, StoreNow));
        }

        public override ValueTask<ZLinkOwnerLeaseReleaseResult> ReleaseOwnerLeaseAsync(
            ZLinkLocationOwnerToken token,
            CancellationToken cancellationToken = default)
        {
            if (!_leases.TryGetValue(token.OwnerId, out var lease)
                || lease.LeaseGeneration != token.LeaseGeneration)
                return ValueTask.FromResult(ZLinkOwnerLeaseReleaseResult.Stale);
            _leases.Remove(token.OwnerId);
            return ValueTask.FromResult(
                ZLinkOwnerLeaseReleaseResult.Released);
        }

        private readonly record struct Lease(
            long LeaseGeneration,
            DateTimeOffset LeaseExpiresAt);
    }

    private sealed class ExampleMeshNodeLocationStore : LocationStoreContractExample
    {
        private ZLinkMeshNodeDescriptor? _row;

        public override ValueTask<ZLinkLocationWriteResult> UpdateMeshNodeAsync(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationWriteIntent intent,
            CancellationToken cancellationToken = default)
        {
            _row = descriptor;
            return ValueTask.FromResult(
                ZLinkLocationWriteResult.Stored(descriptor.LifecycleGeneration, StoreNow));
        }

        public override ValueTask<ZLinkLocationWriteStatus> RemoveMeshNodeAsync(
            ZLinkMeshNodeDescriptorKey key,
            ZLinkLocationOwnerToken owner,
            CancellationToken cancellationToken = default)
        {
            _row = null;
            return ValueTask.FromResult(ZLinkLocationWriteStatus.Stored);
        }

        public override ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodesAsync(
            string meshName,
            ZLinkPageRequest page,
            CancellationToken cancellationToken = default)
        {
            IReadOnlyList<ZLinkMeshNodeDescriptor> items =
                _row is { } row && row.MeshName == meshName ? [row] : [];
            return ValueTask.FromResult(
                new ZLinkLocationPage<ZLinkMeshNodeDescriptor>(items, null));
        }
    }

    private sealed class ExampleLocationRuntimeQuery : IZLinkLocationRuntimeQuery
    {
        public ValueTask<ZLinkLocationRuntimeStatus> GetStatusAsync(
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(new ZLinkLocationRuntimeStatus(
                StoreHealthy: true,
                LastRefreshAt: StoreNow,
                OwnerLeaseHealthy: true,
                OwnerLeaseRenewedAt: StoreNow));

        public ValueTask<ZLinkLocationPage<ZLinkLocationTopologyEntry>> ListTopologyAsync(
            ZLinkLocationTopologyFilter filter,
            ZLinkPageRequest page = default,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult(new ZLinkLocationPage<ZLinkLocationTopologyEntry>(
                [
                    new ZLinkLocationTopologyEntry(
                        "play",
                        RoutingId.From("node-1"),
                        "tcp://127.0.0.1:5001",
                        Draining: false,
                        ZLinkLocationTopologyState.Ready,
                        StoreNow)
                ],
                null));

        public ValueTask<ZLinkLocationPage<ZLinkLocationServiceSummary>>
            ListServiceSummariesAsync(
            ZLinkLocationServiceSummaryFilter filter,
            ZLinkPageRequest page = default,
            CancellationToken cancellationToken = default)
        {
            IReadOnlyList<ZLinkLocationServiceSummary> items =
            [
                new ZLinkLocationServiceSummary(
                    "play",
                    1,
                    1,
                    0,
                    0,
                    StoreNow)
            ];
            return ValueTask.FromResult(
                new ZLinkLocationPage<ZLinkLocationServiceSummary>(items, null));
        }
    }

    private sealed class ExampleLocationReadiness(IZLinkLocationRuntimeQuery query) : IZLinkLocationReadiness
    {
        public async ValueTask<bool> IsPeerReadyAsync(
            string meshName,
            ZLinkLocationRole role,
            RoutingId? nodeRid = null,
            CancellationToken cancellationToken = default)
        {
            _ = meshName;
            _ = role;
            _ = nodeRid;
            var status = await query.GetStatusAsync(cancellationToken);
            return status.StoreHealthy && status.OwnerLeaseHealthy;
        }
    }

    private sealed class ExampleChangeStampStore : LocationStoreContractExample
    {
        public override ValueTask<ulong?> GetMeshNodeChangeStampAsync(
            string meshName,
            CancellationToken cancellationToken = default) =>
            ValueTask.FromResult<ulong?>(1UL);
    }
}
