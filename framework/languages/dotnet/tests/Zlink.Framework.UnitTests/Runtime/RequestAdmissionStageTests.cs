using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RequestAdmissionStageTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ReplySuccess_CompletesUnobservedAdmission(bool asynchronous)
    {
        var reply = new TaskCompletionSource<int>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var request = ZLinkRequestAdmission.Run<int>(_ =>
            asynchronous ? new ValueTask<int>(reply.Task) : ValueTask.FromResult(42)
        );
        if (asynchronous)
        {
            Assert.False(request.Admitted.IsCompleted);
            Assert.False(request.Reply.IsCompleted);
            reply.SetResult(42);
        }
        Assert.Equal(42, await request);
        Assert.True(request.Admitted.IsCompletedSuccessfully);
        await request.Admitted;
    }

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
