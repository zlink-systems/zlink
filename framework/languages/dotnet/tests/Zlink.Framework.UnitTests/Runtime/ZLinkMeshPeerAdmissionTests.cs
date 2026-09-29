using Systems.Zlink;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ZLinkMeshPeerAdmissionTests
{
    [Fact]
    public void Selected_routes_report_missing_and_replaced_routes_as_ended()
    {
        var kept = RoutingId.From("kept");
        var replaced = RoutingId.From("replaced");
        var removed = RoutingId.From("removed");
        var added = RoutingId.From("added");
        var routes = new ZLinkMeshSelectedRoutes();

        Assert.Empty(routes.Apply([new(kept, 1), new(replaced, 2), new(removed, 3)]));

        var ended = routes.Apply([new(kept, 1), new(replaced, 9), new(added, 4)]);

        Assert.Equal(
            new RouterRoute[] { new(replaced, 2), new(removed, 3) }.OrderBy(static route =>
                route.RoutingId.ToHex()
            ),
            ended.OrderBy(static route => route.RoutingId.ToHex())
        );
    }

    [Fact]
    public void Selected_route_generation_is_the_observed_core_value()
    {
        var replaced = RoutingId.From("replaced");
        var removed = RoutingId.From("removed");
        var routes = new ZLinkMeshSelectedRoutes();

        routes.Apply([new(replaced, 2), new(removed, 3)]);
        Assert.Equal(2UL, routes.GenerationOf(replaced));
        Assert.Equal(3UL, routes.GenerationOf(removed));

        routes.Apply([new(replaced, 9)]);
        Assert.Equal(9UL, routes.GenerationOf(replaced));
        Assert.Equal(0UL, routes.GenerationOf(removed));
        Assert.Equal(0UL, routes.GenerationOf(default));
    }

}
