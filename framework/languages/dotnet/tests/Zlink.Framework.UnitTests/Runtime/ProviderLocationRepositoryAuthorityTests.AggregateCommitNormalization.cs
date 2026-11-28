using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed partial class ProviderLocationRepositoryAuthorityTests
{
    [Theory]
    [InlineData(true, true)]
    [InlineData(true, false)]
    [InlineData(false, false)]
    public async Task AggregateCommitDistinguishesInternalDeadlineFromCallerCancellation(
        bool loseCommitReply,
        bool stallBeforeConfirmation
    )
    {
        var fixture = await PrepareInventoryFixtureAsync(1);
        var time = new ControllableTimeProvider();
        using var caller = new CancellationTokenSource();
        var readStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseRead = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var inspect = new InspectAuthorityBatchLocationStore(fixture.Provider);
        var aggregateKey = new ZLinkStoreKey(AggregatePrefix(fixture.Prepared.Fence));
        inspect.Inspect = request =>
        {
            if (
                request
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .Any(put => put.Key == aggregateKey)
            )
                inspect.BeforeRead = key =>
                {
                    var shouldStall = stallBeforeConfirmation
                        ? key == aggregateKey
                        : key.Value.Contains(":inventory:", StringComparison.Ordinal);
                    if (!shouldStall)
                        return Task.CompletedTask;
                    readStarted.TrySetResult();
                    return releaseRead.Task.WaitAsync(caller.Token);
                };
            return Task.CompletedTask;
        };
        var provider = new ApplyThenThrowLocationStore(inspect)
        {
            ThrowAfterNextWrite = loseCommitReply,
        };
        var repository = new ZLinkProviderLocationRepository(provider, time);
        var commit = repository.CommitAggregateAsync(fixture.Prepared.Fence, caller.Token).AsTask();
        var testLimit = TimeSpan.FromSeconds(15);

        try
        {
            await readStarted.Task.WaitAsync(testLimit);
            if (loseCommitReply)
            {
                var timeout = TimeSpan.FromSeconds(5);
                var deadlineScheduled = time.HasTimerDueIn(timeout);
                time.AdvanceMonotonic(timeout);

                var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                    await commit.WaitAsync(testLimit)
                );
                Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
                var cause = Assert.IsAssignableFrom<OperationCanceledException>(
                    failure.InnerException
                );
                Assert.True(cause.CancellationToken.IsCancellationRequested);
                Assert.False(caller.IsCancellationRequested);
                Assert.True(deadlineScheduled);
            }
            else
            {
                caller.Cancel();

                var cancellation = await Assert.ThrowsAnyAsync<OperationCanceledException>(
                    async () =>
                        await commit.WaitAsync(testLimit)
                );
                Assert.Equal(caller.Token, cancellation.CancellationToken);
            }
        }
        finally
        {
            releaseRead.TrySetResult();
        }
    }

    [Fact]
    public async Task AggregatePrepareReportsInternalDeadlineWithCauseWhenReplyIsLost()
    {
        var (inner, _, request) = await SeedInventoryFixtureAsync(1);
        var time = new ControllableTimeProvider();
        var readStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseRead = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var inspect = new InspectAuthorityBatchLocationStore(inner);
        var provider = new ApplyThenThrowLocationStore(inspect);
        var aggregateKey = new ZLinkStoreKey(
            AggregatePrefix(
                new ZLinkAggregateFence(request.AggregateId, request.AggregateGeneration)
            )
        );
        inspect.Inspect = write =>
        {
            if (
                write
                    .Conditions.OfType<ZLinkStoreCondition.Version>()
                    .Any(condition => condition.Key == aggregateKey)
                && write
                    .Mutations.OfType<ZLinkStoreMutation.Put>()
                    .Any(put => put.Key == aggregateKey)
            )
            {
                provider.ThrowAfterNextWrite = true;
                inspect.BeforeRead = key =>
                {
                    if (key != aggregateKey)
                        return Task.CompletedTask;
                    if (readStarted.TrySetResult())
                        return releaseRead.Task;
                    throw new IOException("The provider is unavailable.");
                };
            }
            return Task.CompletedTask;
        };

        var repository = new ZLinkProviderLocationRepository(provider, time);
        var prepare = repository.PrepareAggregateAsync(request).AsTask();
        var testLimit = TimeSpan.FromSeconds(15);

        try
        {
            await readStarted.Task.WaitAsync(testLimit);
            var timeout = TimeSpan.FromSeconds(5);
            var deadlineScheduled = time.HasTimerDueIn(timeout);
            time.AdvanceMonotonic(timeout);

            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await prepare.WaitAsync(testLimit)
            );
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
            var cause = Assert.IsAssignableFrom<OperationCanceledException>(failure.InnerException);
            Assert.True(cause.CancellationToken.IsCancellationRequested);
            Assert.True(deadlineScheduled);
        }
        finally
        {
            releaseRead.TrySetResult();
        }
    }
}
