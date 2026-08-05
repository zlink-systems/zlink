using Systems.Zlink;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Locations;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Configuration.Builders;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class WeightContractTests
{
    [Theory]
    [InlineData(0)]
    [InlineData(100)]
    [InlineData(10_000)]
    public void StartupBuildersAcceptTheSignedWeightRange(int weight)
    {
        var registration = new ZLinkFrameworkRegistration();
        var options = new ZLinkFrameworkOptionsBuilder(registration);

        options.AddRouteMesh("mesh")
            .SetPlacementWeight(weight)
            .Channel("route").Server()
            .SetWeight(weight);
        options.AddClientServerChannel("rpc")
            .Server()
            .SetWeight(weight);

        Assert.Equal(weight, registration.SpotNodes["mesh"].PlacementWeight);
        Assert.Equal(
            weight,
            registration.SpotNodes["mesh"].ChannelMemberships.Single().Weight);
        Assert.Equal(
            weight,
            registration.Channels["rpc"].Server!.SocketConfig.Weight);
    }

    [Theory]
    [InlineData(-1)]
    [InlineData(10_001)]
    public void StartupBuildersRejectWeightsOutsideTheSignedRange(int weight)
    {
        var registration = new ZLinkFrameworkRegistration();
        var options = new ZLinkFrameworkOptionsBuilder(registration);
        var mesh = options.AddRouteMesh("mesh");

        Assert.Throws<ZLinkConfigurationException>(
            () => mesh.SetPlacementWeight(weight));
        Assert.Throws<ZLinkConfigurationException>(
            () => mesh.Channel("route").Server().SetWeight(weight));
        Assert.Throws<ZLinkConfigurationException>(
            () => options.AddClientServerChannel("rpc")
                .Server()
                .SetWeight(weight));
    }

    [Fact]
    public void WeightedSelectionUsesA64BitSumAndExactRelativeWeights()
    {
        var overflowInt32 = Enumerable.Repeat(10_000, 214_749).ToArray();
        Assert.Equal(
            2_147_490_000L,
            ZLinkWeightedSelector.Sum(
                overflowInt32,
                static weight => weight));

        WeightedCandidate[] candidates =
        [
            new("one", 100),
            new("three", 300)
        ];
        var counts = candidates.ToDictionary(
            static candidate => candidate.Name,
            static _ => 0,
            StringComparer.Ordinal);
        var currents = new Dictionary<string, long>(StringComparer.Ordinal);
        for (var index = 0; index < 400; index++)
        {
            var selected = ZLinkWeightedSelector.Select(
                candidates,
                static candidate => candidate.Weight,
                static candidate => candidate.Name,
                currents,
                StringComparer.Ordinal);
            counts[selected!.Name]++;
        }

        Assert.Equal(100, counts["one"]);
        Assert.Equal(300, counts["three"]);
    }

    [Fact]
    public void EqualWeightSelectionDoesNotDelayANewCandidateForAFullWeightBlock()
    {
        WeightedCandidate[] candidates =
        [
            new("existing", 100),
            new("new", 100)
        ];
        var currents = new Dictionary<string, long>(StringComparer.Ordinal);

        var selected = Enumerable.Range(0, 4)
            .Select(_ => ZLinkWeightedSelector.Select(
                candidates,
                static candidate => candidate.Weight,
                static candidate => candidate.Name,
                currents,
                StringComparer.Ordinal)!.Name)
            .ToArray();

        Assert.Contains("existing", selected);
        Assert.Contains("new", selected);
    }

    [Fact]
    public void EqualCurrentValuesUseStableIdentifierAsTheTieBreak()
    {
        WeightedCandidate[] candidates =
        [
            new("z", 100),
            new("a", 100)
        ];
        var currents = new Dictionary<string, long>(StringComparer.Ordinal);

        var selected = ZLinkWeightedSelector.Select(
            candidates,
            static candidate => candidate.Weight,
            static candidate => candidate.Name,
            currents,
            StringComparer.Ordinal);

        Assert.Equal("a", selected!.Name);
    }

    [Fact]
    public void EquivalentWeightsUseTheShortestExactRatioCycle()
    {
        WeightedCandidate[] candidates =
        [
            new("one", 100),
            new("three", 300)
        ];
        var counts = candidates.ToDictionary(
            static candidate => candidate.Name,
            static _ => 0,
            StringComparer.Ordinal);
        var currents = new Dictionary<string, long>(StringComparer.Ordinal);

        for (var index = 0; index < 20; index++)
        {
            var selected = ZLinkWeightedSelector.Select(
                candidates,
                static candidate => candidate.Weight,
                static candidate => candidate.Name,
                currents,
                StringComparer.Ordinal);
            counts[selected!.Name]++;
        }

        Assert.Equal(5, counts["one"]);
        Assert.Equal(15, counts["three"]);
    }

    [Fact]
    public void SelectionPlanPrecomputesTheExactSmoothWeightedOrder()
    {
        WeightedCandidate[] candidates =
        [
            new("a", 100),
            new("b", 300)
        ];
        var plan = new ZLinkWeightedSelectionPlan<
            WeightedCandidate,
            string>(
            candidates,
            static candidate => candidate.Weight,
            static candidate => candidate.Name,
            retainedCurrents: null,
            StringComparer.Ordinal,
            StringComparer.Ordinal);

        var selected = Enumerable.Range(0, 4)
            .Select(_ => plan.Select()!.Name)
            .ToArray();

        Assert.Equal(new[] { "b", "a", "b", "b" }, selected);
    }

    [Fact]
    public void SelectionPlanKeepsRetainedCurrentsWhenTopologyChanges()
    {
        WeightedCandidate[] initialCandidates =
        [
            new("a", 100),
            new("b", 100)
        ];
        var initial = new ZLinkWeightedSelectionPlan<
            WeightedCandidate,
            string>(
            initialCandidates,
            static candidate => candidate.Weight,
            static candidate => candidate.Name,
            retainedCurrents: null,
            StringComparer.Ordinal,
            StringComparer.Ordinal);

        Assert.Equal("a", initial.Select()!.Name);
        var retained = initial.CaptureCurrents();
        WeightedCandidate[] replacementCandidates =
        [
            new("b", 100),
            new("c", 100)
        ];
        var replacement = new ZLinkWeightedSelectionPlan<
            WeightedCandidate,
            string>(
            replacementCandidates,
            static candidate => candidate.Weight,
            static candidate => candidate.Name,
            retained,
            StringComparer.Ordinal,
            StringComparer.Ordinal);

        Assert.Equal("b", replacement.Select()!.Name);
    }

    [Fact]
    public void MeshChannelSelectionKeepsDeclaredChannelsSeparateFromEligibleTargets()
    {
        var selection = new ZLinkMeshChannelSelection();
        var target = new ZLinkMeshChannelTarget(
            RoutingId.From("node-a"),
            weight: 100);

        selection.Rebuild(
            ["events"],
            _ => [target]);

        Assert.True(selection.IsDeclared("events"));
        Assert.True(selection.TrySelect("events", out var selected));
        Assert.Equal(target.RoutingId, selected);

        selection.Rebuild(
            ["events"],
            _ => Array.Empty<ZLinkMeshChannelTarget>());

        Assert.True(selection.IsDeclared("events"));
        Assert.False(selection.TrySelect("events", out _));
    }

    [Fact]
    public void ObjectPlacementFiltersCapacityAndZeroWeightBeforeSelection()
    {
        var eligible = Descriptor(
            weight: 100,
            actors: new ZLinkPopulationCapacity(9, 0, 10),
            spots: new ZLinkPopulationCapacity(9, 0, 10),
            spotType: new ZLinkSpotTypeCapacity(
                ZLinkPlacementObjectKind.UserSpot,
                "room",
                9,
                0,
                10));
        Assert.True(ZLinkActorManagerService.IsEligibleCandidate(
            eligible,
            "player"));
        Assert.True(ZLinkSpotRuntimeManager.IsEligibleCandidate(
            eligible,
            "room"));

        var zeroWeight = eligible with { PlacementWeight = 0 };
        Assert.False(ZLinkActorManagerService.IsEligibleCandidate(
            zeroWeight,
            "player"));
        Assert.False(ZLinkSpotRuntimeManager.IsEligibleCandidate(
            zeroWeight,
            "room"));

        var actorFull = eligible with
        {
            Capacity = eligible.Capacity with
            {
                Actors = new ZLinkPopulationCapacity(9, 1, 10)
            }
        };
        Assert.False(ZLinkActorManagerService.IsEligibleCandidate(
            actorFull,
            "player"));

        var spotTypeFull = eligible with
        {
            Capacity = eligible.Capacity with
            {
                SpotTypes =
                [
                    new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.UserSpot,
                        "room",
                        9,
                        1,
                        10)
                ]
            }
        };
        Assert.False(ZLinkSpotRuntimeManager.IsEligibleCandidate(
            spotTypeFull,
            "room"));
    }

    [Fact]
    public void Placement_candidates_require_an_admitted_peer_for_the_same_generation()
    {
        var local = Descriptor(
            weight: 100,
            actors: new ZLinkPopulationCapacity(0, 0, 10),
            spots: new ZLinkPopulationCapacity(0, 0, 10),
            spotType: new ZLinkSpotTypeCapacity(
                ZLinkPlacementObjectKind.UserSpot,
                "room",
                0,
                0,
                10)) with
        {
            Rid = RoutingId.From("local-target")
        };
        var current = local with
        {
            Rid = RoutingId.From("current-target"),
            LifecycleGeneration = 7
        };
        var stale = local with
        {
            Rid = RoutingId.From("stale-target"),
            LifecycleGeneration = 9
        };
        var peers = new[]
        {
            new MeshNodePeer(
                1,
                MeshPeerSource.Discovery,
                MeshPeerState.Admitted,
                current.Rid,
                current.LifecycleGeneration,
                1,
                current.Endpoint,
                0,
                0,
                0),
            new MeshNodePeer(
                2,
                MeshPeerSource.Discovery,
                MeshPeerState.Admitted,
                stale.Rid,
                stale.LifecycleGeneration - 1,
                1,
                stale.Endpoint,
                0,
                0,
                0)
        };

        var filtered = ZLinkMeshNodeTargetAvailability.FilterAdmitted(
                local.Rid,
                new[] { local, current, stale },
                peers)
            .Select(static candidate => candidate.Rid)
            .ToArray();

        Assert.Equal(
            new[] { local.Rid, current.Rid },
            filtered);
    }

    private static ZLinkMeshNodeDescriptor Descriptor(
        int weight,
        ZLinkPopulationCapacity actors,
        ZLinkPopulationCapacity spots,
        ZLinkSpotTypeCapacity spotType) =>
        new(
            "objects",
            RoutingId.From("weight-target"),
            7,
            1,
            "inproc://weight-target",
            new Dictionary<string, int>(),
            "test",
            "owner",
            3,
            DateTimeOffset.UtcNow)
        {
            State = ZLinkFrameworkRuntimeState.Serving,
            ObjectRole = ZLinkMeshNodeObjectRole.Server,
            EntrySpotId = "weight-entry",
            PlacementWeight = weight,
            ObjectCapabilities =
            [
                new(
                    ZLinkPlacementObjectKind.Actor,
                    "player",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0),
                new(
                    ZLinkPlacementObjectKind.UserSpot,
                    "room",
                    ZLinkObjectMaintenancePolicyKind.Disabled,
                    false,
                    0)
            ],
            Capacity = new(actors, spots, [spotType])
        };

    private sealed record WeightedCandidate(string Name, int Weight);
}
