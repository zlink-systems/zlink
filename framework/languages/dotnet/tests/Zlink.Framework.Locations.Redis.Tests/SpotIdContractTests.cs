using System.Text;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed class SpotIdContractTests
{
    [Fact]
    public void SpotId_Accepts_One_Through_255_Utf8_Bytes()
    {
        Assert.True(ZLinkSpotId.IsValid("a"));
        Assert.True(ZLinkSpotId.IsValid(new string('가', 85)));

        Assert.False(ZLinkSpotId.IsValid(string.Empty));
        Assert.False(ZLinkSpotId.IsValid(new string('a', 256)));
        Assert.False(ZLinkSpotId.IsValid("\0"));
        Assert.False(ZLinkSpotId.IsValid("\ud800"));
    }

    [Fact]
    public void SpotId_Uses_Exact_Case_Sensitive_Text_Without_Normalization()
    {
        const string composed = "\u00e9";
        const string decomposed = "e\u0301";

        Assert.True(ZLinkSpotId.IsValid(composed));
        Assert.True(ZLinkSpotId.IsValid(decomposed));
        Assert.NotEqual(composed, decomposed);
        Assert.NotEqual("room", "Room");
    }

    [Fact]
    public void Native_Bridge_Rejects_Invalid_Utf8_And_Preserves_Valid_Text()
    {
        const string spotId = "room-\uac00";
        Assert.Equal(
            spotId,
            ZLinkSpotId.FromNativeRoutingId(
                ZLinkSpotId.ToNativeRoutingId(spotId)));

        Assert.Equal(
            string.Empty,
            ZLinkSpotId.FromNativeRoutingId(
                RoutingId.From(new byte[] { 0xc3, 0x28 })));
        Assert.Equal(
            8,
            Encoding.UTF8.GetByteCount(spotId));
    }
}
