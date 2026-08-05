namespace Zlink.Framework.UnitTests;

public sealed class LocationContractTests
{
    [Fact]
    public void Canonical_Name_Helper_Is_Not_Public_Contract()
    {
        var assembly = typeof(ZLinkLocationAutoConnectType).Assembly;

        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkLocation" + "CanonicalNames"));
    }

    [Fact]
    public void Removed_Location_Kind_Is_Not_Public_Contract()
    {
        var assembly = typeof(ZLinkLocationAutoConnectType).Assembly;

        Assert.Null(assembly.GetType(
            "Zlink.Framework.Contracts.Locations.ZLinkLocation" + "Kind"));
    }

    [Fact]
    public void WriteResult_Stored_Carries_StoreIssued_Generation_And_UpdatedAt()
    {
        var updatedAt = new DateTimeOffset(2026, 7, 2, 0, 0, 0, TimeSpan.Zero);

        var stored = ZLinkLocationWriteResult.Stored(5UL, updatedAt);

        Assert.Equal(ZLinkLocationWriteStatus.Stored, stored.Status);
        Assert.Equal(5UL, stored.Generation);
        Assert.Equal(updatedAt, stored.UpdatedAt);
        Assert.Equal(ZLinkLocationWriteStatus.IgnoredStale, ZLinkLocationWriteResult.IgnoredStale.Status);
        Assert.Equal(ZLinkLocationWriteStatus.RejectedConflict, ZLinkLocationWriteResult.RejectedConflict.Status);
        Assert.DoesNotContain(
            Enum.GetNames<ZLinkLocationWriteStatus>(),
            static name => name == "Store" + "Unavailable");
    }

    [Fact]
    public void PageRequest_Default_Is_Zero_Before_Runtime_Normalization()
    {
        ZLinkPageRequest page = default;

        Assert.Equal(0, page.PageSize);
        Assert.Null(page.ContinuationToken);
    }

    [Fact]
    public void Location_Readiness_Contract_Uses_Boolean_Peer_Check()
    {
        var method = typeof(IZLinkLocationReadiness).GetMethod(nameof(IZLinkLocationReadiness.IsPeerReadyAsync));

        Assert.NotNull(method);
        Assert.Equal(typeof(ValueTask<bool>), method.ReturnType);
    }
}
