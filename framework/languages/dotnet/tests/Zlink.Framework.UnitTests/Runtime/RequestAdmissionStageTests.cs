using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Messaging;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RequestAdmissionStageTests
{
    [Fact]
    public void Immediate_reply_observation_allocates_only_the_admission_source()
    {
        const int iterations = 1024;
        const int warmupIterations = 256;
        const int replyValue = 7;
        Func<ZLinkRequestAdmission, ValueTask<int>> execute = static _ =>
            ValueTask.FromResult(replyValue);
        for (var index = 0; index < warmupIterations; index++)
        {
            new ZLinkRequestAdmission().Complete();
            _ = ZLinkRequestAdmission.Run(execute);
        }
        var sourceBytes = PerCallAllocation.MinWindow(
            () => new ZLinkRequestAdmission().Complete(),
            iterations
        );
        var admitted = true;
        var replies = 0;
        var observedBytes = PerCallAllocation.MinWindow(
            _ =>
            {
                var request = ZLinkRequestAdmission.Run(execute);
                admitted &= request.Admitted.IsCompletedSuccessfully;
                replies += request.Reply.GetAwaiter().GetResult();
            },
            iterations,
            beforeWindow: _ => replies = 0
        );
        Assert.True(admitted);
        Assert.Equal(replyValue * iterations, replies);
        Assert.Equal(sourceBytes, observedBytes);
    }

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
