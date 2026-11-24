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

    [Fact]
    public async Task Target_replay_reservation_runs_as_an_actor_on_the_shared_gate()
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

        ZLinkSpotRelocationActorQueueReservation? reservation = null;
        Assert.True(
            await executor.TryOpenRelocationAfterMessageFollowAsync(
                seal,
                () => reservation = executor.ReserveRelocationActorQueue(seal, "actor-1")
            )
        );

        Assert.NotNull(reservation);
        await reservation
            .ExecuteAsync(
                "actor-1",
                static _ =>
                {
                    var execution = ZLinkApplicationExecutionContext.Current;
                    Assert.NotNull(execution);
                    Assert.Equal("actor-1", execution.Value.ActorId);
                    Assert.True(execution.Value.YieldAllowed);
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            )
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(10));
    }
}
