using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RequestAdmissionStageTests
{
    [Theory]
    [InlineData(typeof(IZLinkRequestCall))]
    [InlineData(typeof(IZLinkActorRequestCall))]
    [InlineData(typeof(IZLinkSpotRequestCall))]
    public void Async_ReturnsRequestWithAdmissionAndReply(Type callType)
    {
        var result = callType.GetMethod("Async")!.ReturnType;
        Assert.Equal("ZLinkRequest`1", result.Name);
        Assert.Equal(typeof(ValueTask), result.GetProperty("Admitted")!.PropertyType);
        Assert.NotNull(result.GetProperty("Reply"));
        Assert.NotNull(result.GetMethod("GetAwaiter"));
        Assert.NotNull(result.GetMethod("ConfigureAwait"));
        Assert.NotNull(result.GetMethod("AsTask"));
    }
}
