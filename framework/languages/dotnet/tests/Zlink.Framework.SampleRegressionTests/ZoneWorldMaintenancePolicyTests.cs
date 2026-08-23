using ZoneWorld.Server.ZoneNode.Application.Node;
using ZoneWorld.Shared.Contracts;
using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed class ZoneWorldMaintenancePolicyTests
{
    [Fact]
    public void Maintenance_Allows_Only_Same_Zone_Movement_And_Rejects_Every_Join()
    {
        var policy = new NodeMaintenancePolicy(NodeIds.West);
        policy.Apply(NodeIds.West, enabled: true);

        Assert.False(policy.RejectsArrival(ZoneIds.NorthWest, ZoneIds.NorthWest));
        Assert.True(policy.RejectsArrival(ZoneIds.NorthWest, sourceZoneId: null));
        Assert.True(policy.RejectsArrival(ZoneIds.NorthWest, ZoneIds.SouthWest));
    }
}
