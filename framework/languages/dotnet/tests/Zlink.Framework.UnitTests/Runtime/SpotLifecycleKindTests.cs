using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class SpotLifecycleKindTests
{
    [Theory]
    [InlineData(ZLinkSpotKind.Entry, typeof(ZLinkSpotLifecycleKind.Entry))]
    [InlineData(ZLinkSpotKind.User, typeof(ZLinkSpotLifecycleKind.User))]
    [InlineData(ZLinkSpotKind.Instance, typeof(ZLinkSpotLifecycleKind.Instance))]
    public void BoundaryTagCreatesOneClosedDomainVariant(
        ZLinkSpotKind boundary,
        Type expectedVariant)
    {
        var kind = ZLinkSpotLifecycleKind.FromBoundary(boundary);

        Assert.IsType(expectedVariant, kind);
    }

    [Fact]
    public void EntryCannotEnterRelocatableLifecycle()
    {
        Assert.Throws<InvalidOperationException>(() =>
            ZLinkSpotLifecycleKind.RelocatableFromBoundary(
                ZLinkSpotKind.Entry));
        Assert.IsType<ZLinkSpotLifecycleKind.User>(
            ZLinkSpotLifecycleKind.RelocatableFromBoundary(
                ZLinkSpotKind.User));
        Assert.IsType<ZLinkSpotLifecycleKind.Instance>(
            ZLinkSpotLifecycleKind.RelocatableFromBoundary(
                ZLinkSpotKind.Instance));
    }
}
