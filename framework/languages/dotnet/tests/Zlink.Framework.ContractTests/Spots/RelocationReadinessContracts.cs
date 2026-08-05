using Zlink.Framework.ContractTests.Support;

namespace Zlink.Framework.ContractTests.Spots;

public sealed class RelocationReadinessContracts
{
    [Fact]
    [ContractExample(
        typeof(IZLinkSpotRelocationReadyCall),
        typeof(IZLinkUserSpotFactoryBuilder<>))]
    public void SpotWide_application_signaled_surface_matches_the_exact_contract()
    {
        Assert.Equal(
            new[] { "Defer" },
            typeof(IZLinkSpotRelocationReadyCall)
                .GetMethods()
                .Select(static method => method.Name)
                .ToArray());
        Assert.NotNull(typeof(IZLinkSpotContext).GetMethod("RelocationReady"));
        Assert.NotNull(typeof(IZLinkSpot).GetMethod(
            "OnRelocationReadyCompletedAsync"));

        Assert.NotNull(
            typeof(IZLinkUserSpotFactoryBuilder<>)
                .GetMethod("RelocationReadiness"));
        Assert.Equal(
            0,
            (int)ZLinkSpotRelocationReadinessMode.AnyTurnBoundary);
    }
}
