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
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
            new ZLinkRequestAdmission().Complete();
        var sourceBytes = GC.GetAllocatedBytesForCurrentThread() - before;
        var admitted = true;
        var replies = 0;
        before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < iterations; index++)
        {
            var request = ZLinkRequestAdmission.Run(execute);
            admitted &= request.Admitted.IsCompletedSuccessfully;
            replies += request.Reply.GetAwaiter().GetResult();
        }
        var observedBytes = GC.GetAllocatedBytesForCurrentThread() - before;
        Assert.True(admitted);
        Assert.Equal(replyValue * iterations, replies);
        Assert.Equal(sourceBytes, observedBytes);
    }

    [Fact]
    public void Accepted_admission_allocates_only_its_existing_owner_storage()
    {
        const int count = 512;
        var baseline = new System.Runtime.CompilerServices.StrongBox<Task>[count];
        var requests = new ZLinkRequest<int>[count];
        Func<ZLinkRequestAdmission, ValueTask<int>> execute = static admission =>
        {
            admission.Complete();
            return ValueTask.FromResult(7);
        };
        for (var index = 0; index < count; index++)
            _ = ZLinkRequestAdmission.Run(execute);
        var started = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < count; index++)
            baseline[index] = new System.Runtime.CompilerServices.StrongBox<Task>();
        var expected = GC.GetAllocatedBytesForCurrentThread() - started;
        started = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < count; index++)
            requests[index] = ZLinkRequestAdmission.Run(execute);
        var measured = GC.GetAllocatedBytesForCurrentThread() - started;
        foreach (var request in requests)
        {
            Assert.True(request.Admitted.IsCompletedSuccessfully);
            Assert.Equal(7, request.Reply.GetAwaiter().GetResult());
        }
        GC.KeepAlive(baseline);
        Assert.Equal(expected, measured);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Pending_admission_completes_independently_of_reply_failure(bool cancel)
    {
        ZLinkRequestAdmission? owner = null;
        var reply = new TaskCompletionSource<int>();
        var request = ZLinkRequestAdmission.Run<int>(admission =>
        {
            owner = admission;
            return new ValueTask<int>(reply.Task);
        });
        Assert.False(request.Admitted.IsCompleted);
        Assert.False(request.Reply.IsCompleted);
        owner!.Complete();
        await request.Admitted;
        Assert.False(request.Reply.IsCompleted);
        using var cancellation = new CancellationTokenSource();
        if (cancel)
        {
            cancellation.Cancel();
            reply.SetCanceled(cancellation.Token);
            var failure = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                request.AsTask()
            );
            Assert.Equal(cancellation.Token, failure.CancellationToken);
        }
        else
        {
            var expected = new InvalidOperationException("reply failed after admission");
            reply.SetException(expected);
            Assert.Same(
                expected,
                await Assert.ThrowsAsync<InvalidOperationException>(() => request.AsTask())
            );
        }
        await request.Admitted;
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
