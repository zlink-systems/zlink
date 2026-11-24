namespace Zlink.Framework.UnitTests;

public sealed class SpotTargetRelocationReleaseTests
{
    [Fact]
    public async Task Target_opening_admits_lifecycle_work_after_replay_reservation()
    {
        using var errors = new ZLinkRuntimeErrorSink();
        await using var executor = new ZLinkSpotSerialExecutor(
            null!,
            () => false,
            CancellationToken.None,
            errors
        );
        await executor.BeginTargetPreparationAsync();
        var seal = await executor.TrySealRelocationAsync();
        Assert.NotNull(seal);

        Task? lifecycle = null;
        var runs = 0;
        Assert.True(
            await executor.TryOpenRelocationAfterMessageFollowAsync(
                seal,
                () =>
                {
                    lifecycle = executor
                        .ExecuteLifecycleAsync(
                            (_, _) =>
                            {
                                Interlocked.Increment(ref runs);
                                return ValueTask.CompletedTask;
                            },
                            CancellationToken.None
                        )
                        .AsTask();
                }
            )
        );

        Assert.NotNull(lifecycle);
        await lifecycle.WaitAsync(TimeSpan.FromSeconds(10));
        Assert.Equal(1, Volatile.Read(ref runs));
    }
}
