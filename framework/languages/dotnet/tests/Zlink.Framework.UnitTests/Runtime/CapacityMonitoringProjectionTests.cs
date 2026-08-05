using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Dispatch;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Host;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class CapacityMonitoringProjectionTests
{
    [Fact]
    public void Public_Monitoring_Surface_Does_Not_Expose_Publish_Target_Statistics()
    {
        var contracts = typeof(ZLinkMeshNodeSnapshot).Assembly;
        Assert.Null(contracts.GetType(
            "Zlink.Framework.Contracts.Configuration.ZLinkLogicalMulticastSnapshot"));
        Assert.Null(typeof(ZLinkMeshNodeSnapshot).GetProperty("Multicast"));

        var removedNames = new HashSet<string>(
            [
                "RemoteSnapshotCount",
                "RemoteAdmittedCount",
                "RemoteDroppedCount",
                "RemoteUnreachableCount",
                "LocalSnapshotCount",
                "LocalAdmittedCount",
                "LocalDroppedCount"
            ],
            StringComparer.Ordinal);
        Assert.DoesNotContain(
            typeof(ZLinkMeshRuntimeEvent).GetProperties(),
            property => removedNames.Contains(property.Name));
        Assert.DoesNotContain(
            typeof(ZLinkMessageFlowEvent).GetProperties(),
            property => removedNames.Contains(property.Name));
    }

    [Fact]
    public void RouteMesh_Fallback_Projection_Preserves_Typed_Limits_And_Unlimited()
    {
        var registration = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "mesh",
            MaxActiveObjects = 0,
            MaxPendingActivations = 64
        };
        registration.SpotRelocations.Add(
            "room",
            new ZLinkObjectRelocationRegistration(
                typeof(object),
                new ZLinkObjectPlacementOptions { MaxActiveObjects = 12 },
                PolicyKind: 1,
                AdapterType: null,
                AdapterInvoker: null));
        registration.InstanceSpotRelocations.Add(
            "worker",
            new ZLinkObjectRelocationRegistration(
                typeof(object),
                new ZLinkObjectPlacementOptions { MaxActiveObjects = 0 },
                PolicyKind: 1,
                AdapterType: null,
                AdapterInvoker: null));

        var capacity =
            ZLinkRouteMeshRuntimeService.BuildPopulationCapacity(registration);

        Assert.Equal(new ZLinkPopulationCapacity(0, 0, 0), capacity.Actors);
        Assert.Equal(new ZLinkPopulationCapacity(0, 0, 0), capacity.Spots);
        Assert.Collection(
            capacity.SpotTypes,
            room =>
            {
                Assert.Equal(ZLinkPlacementObjectKind.UserSpot, room.ObjectKind);
                Assert.Equal("room", room.StableType);
                Assert.Equal(12, room.Limit);
            },
            worker =>
            {
                Assert.Equal(
                    ZLinkPlacementObjectKind.InstanceSpot,
                    worker.ObjectKind);
                Assert.Equal("worker", worker.StableType);
                Assert.Equal(0, worker.Limit);
            });
    }

    [Fact]
    public void Placement_Event_Carries_Operation_And_Node_Capacity_Separately()
    {
        var population = new ZLinkPlacementCapacity(
            new ZLinkPopulationCapacity(5, 2, 0),
            new ZLinkPopulationCapacity(3, 1, 20),
            []);
        var activation = new ZLinkActivationConcurrency(2, 64);
        var operation = new ZLinkCapacityVector(
            Actors: 1,
            Spots: 0,
            SpotType: null);
        var runtimeEvent = new ZLinkMeshRuntimeEvent(
            "zlink.runtime.object.placement_changed",
            1,
            DateTimeOffset.UtcNow,
            "mesh",
            RoutingId.From("node"),
            PeerRid: null,
            LifecycleGeneration: null,
            DescriptorRevision: 3,
            ChannelName: null,
            ClaimDomain: null,
            MessageKind: null,
            PlacementOutcome: "reserved",
            Capacity: operation,
            PopulationCapacity: population,
            ActivationConcurrency: activation,
            Reason: null,
            State: null);

        Assert.Same(population, runtimeEvent.PopulationCapacity);
        Assert.Same(activation, runtimeEvent.ActivationConcurrency);
        Assert.Same(operation, runtimeEvent.Capacity);
    }
}
