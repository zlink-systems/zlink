using ZoneWorld.Server.ZoneNode.Application.Node;
using ZoneWorld.Shared.Contracts;
using Xunit;

namespace Zlink.Framework.SampleRegressionTests;

public sealed class ZoneWorldMaintenancePolicyTests
{
    [Fact]
    public void Maintenance_Allows_Same_Node_Zone_Move_But_Rejects_New_And_Remote_Arrivals()
    {
        var policy = new NodeMaintenancePolicy(NodeIds.West);
        policy.Apply(NodeIds.West, enabled: true);

        Assert.False(policy.RejectsArrival(NodeIds.West, NodeIds.West));
        Assert.True(policy.RejectsArrival(NodeIds.West, sourceNodeId: null));
        Assert.True(policy.RejectsArrival(NodeIds.West, NodeIds.East));
    }
}
