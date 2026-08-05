using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class GeneratedNodeIdentityTests
{
    [Fact]
    public void Prepared_identity_is_the_effective_runtime_identity()
    {
        var explicitId = RoutingId.From("explicit-node");
        var generatedId = RoutingId.From("generated-node");
        var registration = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "mesh",
            RoutingId = explicitId,
            PreparedRoutingId = generatedId
        };

        Assert.Equal(generatedId, registration.EffectiveRoutingId);
    }

    [Fact]
    public void Explicit_identity_is_used_before_runtime_preparation()
    {
        var explicitId = RoutingId.From("explicit-node");
        var registration = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "mesh",
            RoutingId = explicitId
        };

        Assert.Equal(explicitId, registration.EffectiveRoutingId);
    }

    [Fact]
    public void C11_generated_identity_excludes_self_and_accepts_exact_remote_peer()
    {
        var source = RoutingId.From("generated-source");
        var target = RoutingId.From("generated-target");
        var registration = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "observability.play",
            PreparedRoutingId = source
        };
        ZLinkRouteMeshPeerIdentity[] descriptors =
        [
            new(source, 11, false),
            new(target, 22, false)
        ];
        MeshNodePeer[] peers =
        [
            new(
                ConnectionIntentId: 1,
                Source: MeshPeerSource.Discovery,
                State: MeshPeerState.Admitted,
                RoutingId: target,
                LifecycleGeneration: 22,
                DescriptorRevision: 2,
                Endpoint: "tcp://127.0.0.1:20002",
                ChannelCount: 1,
                LastError: 0,
                LastChangedMs: 1)
        ];

        Assert.True(ZLinkFrameworkRuntime.HasExactPeerReadiness(
            descriptors,
            peers,
            new HashSet<RoutingId> { registration.EffectiveRoutingId }));
    }

    [Fact]
    public void C11_target_snapshot_reserves_room_and_instance_capabilities()
    {
        var sourceRid = RoutingId.From("generated-source");
        var targetRid = RoutingId.From("generated-target");
        var source = new ZLinkFrameworkRegistration
        {
            MaintenanceWave = "wave-a"
        };
        var capabilities = new[]
        {
            new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.UserSpot,
                "observability-room",
                ZLinkObjectMaintenancePolicyKind.Snapshot,
                true,
                128),
            new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.InstanceSpot,
                "observability-instance",
                ZLinkObjectMaintenancePolicyKind.Snapshot,
                true,
                128),
            new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.Actor,
                "observability-player",
                ZLinkObjectMaintenancePolicyKind.Snapshot,
                true,
                0)
        };
        var target = new ZLinkMeshNodeDescriptor(
            "observability.play",
            targetRid,
            22,
            2,
            "tcp://127.0.0.1:20002",
            new Dictionary<string, int>(),
            ZLinkTransportSecurityIdentity.Plaintext,
            "target-owner",
            7,
            DateTimeOffset.UtcNow)
        {
            State = ZLinkFrameworkRuntimeState.Serving,
            ApplicationVersion = 2,
            MaintenanceWave = "wave-b",
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            ObjectCapabilities = capabilities,
            PlacementWeight = 100,
            Capacity = new ZLinkPlacementCapacity(
                new ZLinkPopulationCapacity(0, 0, 128),
                new ZLinkPopulationCapacity(0, 0, 128),
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        "observability-room",
                        0,
                        0,
                        128),
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.InstanceSpot,
                        "observability-instance",
                        0,
                        0,
                        128)
                ]),
            ActivationConcurrency = new ZLinkActivationConcurrency(0, 32)
        };
        var owner = new ZLinkLocationOwnerToken("source-owner", 5);
        var room = new ZLinkSpotRetireInventory(
            "observability.play",
            sourceRid,
            11,
            owner,
            "room-1",
            "observability-room",
            typeof(object),
            false,
            false,
            1,
            ["actor-1"],
            [capabilities[0], capabilities[2]]);
        var instance = new ZLinkSpotRetireInventory(
            "observability.play",
            sourceRid,
            11,
            owner,
            "instance-1",
            "observability-instance",
            typeof(object),
            true,
            false,
            1,
            [],
            [capabilities[1]]);
        var selection = new ZLinkRelocationTargetSelection(
            ZLinkFrameworkRelocationMode.RollingUpdate,
            2);
        var plan = new ZLinkRetirePreflightPlan();

        Assert.True(selection.Matches(target));
        Assert.True(ZLinkSpotRetireTargetRuntime.IsCompatibleTarget(
            target,
            source,
            room,
            ZLinkPlacementObjectKind.UserSpot));
        Assert.True(plan.TryReserve(
            target,
            new ZLinkCapacityVector(
                1,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.UserSpot,
                    "observability-room",
                    1))));
        Assert.True(ZLinkSpotRetireTargetRuntime.IsCompatibleTarget(
            target,
            source,
            instance,
            ZLinkPlacementObjectKind.InstanceSpot));
        Assert.True(plan.TryReserve(
            target,
            new ZLinkCapacityVector(
                0,
                1,
                new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    "observability-instance",
                    1))));
    }
}
