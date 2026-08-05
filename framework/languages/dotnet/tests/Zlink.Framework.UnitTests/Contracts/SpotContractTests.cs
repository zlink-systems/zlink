namespace Zlink.Framework.UnitTests;

public sealed class SpotContractTests
{
    [Fact]
    public void SpotCreateResult_Exposes_Exact_SpotRef()
    {
        const string spotId = "0a0b0c";

        var reference = new SpotRef(
            spotId, 7, "game", RoutingId.From("node-a"));
        var created = new ZLinkSpotCreateResult(
            reference, ZLinkSpotCreateState.Created, null);

        Assert.Equal(reference, created.Spot);
        Assert.Equal(ZLinkSpotCreateState.Created, created.State);
    }
}
