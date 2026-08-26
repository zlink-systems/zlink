using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests;

/// <summary>
/// Specifies <see cref="ZLinkStateLane"/>, the ownership primitive every converted state component
/// relies on. Each test states one guarantee a component author is allowed to assume.
/// </summary>
public sealed class StateLaneTests
{
    // ---- 기본 동작 -------------------------------------------------------------------

    [Fact]
    public async Task RunAsync_ReturnsTheResultOfTheWork()
    {
        await using var lane = new ZLinkStateLane();

        var result = await lane.RunAsync(() => 42);

        Assert.Equal(42, result);
    }

    [Fact]
    public async Task RunAsync_SurfacesAFailureToItsOwnCaller()
    {
        await using var lane = new ZLinkStateLane();

        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await lane.RunAsync<int>(
                () => throw new InvalidOperationException("boom")));
    }

    [Fact]
    public async Task RunAsync_KeepsServingAfterAWorkItemThrows()
    {
        await using var lane = new ZLinkStateLane();

        await Assert.ThrowsAsync<InvalidOperationException>(
            async () => await lane.RunAsync<int>(() => throw new InvalidOperationException()));

        Assert.Equal(7, await lane.RunAsync(() => 7));
    }

    // ---- 소유권: 잠금 없이 안전한가 --------------------------------------------------

    [Fact]
    public async Task ConcurrentCallers_MutateUnsynchronizedStateWithoutLosingUpdates()
    {
        //  This is the whole point of the design: the dictionary is a plain Dictionary with no
        //  gate. If the lane did not serialize, this would corrupt or lose writes.
        await using var lane = new ZLinkStateLane();
        var state = new Dictionary<int, int>();
        const int callers = 32;
        const int perCaller = 50;

        await Task.WhenAll(Enumerable.Range(0, callers).Select(caller =>
            Task.Run(async () =>
            {
                for (var i = 0; i < perCaller; i++)
                {
                    var key = (caller * perCaller) + i;
                    await lane.RunAsync(() => state[key] = key);
                }
            })));

        Assert.Equal(callers * perCaller, await lane.RunAsync(() => state.Count));
    }

    [Fact]
    public async Task WorkItems_NeverOverlap()
    {
        await using var lane = new ZLinkStateLane();
        var inFlight = 0;
        var observedOverlap = false;

        await Task.WhenAll(Enumerable.Range(0, 64).Select(_ =>
            Task.Run(async () => await lane.RunAsync(() =>
            {
                if (Interlocked.Increment(ref inFlight) != 1)
                    observedOverlap = true;
                Thread.SpinWait(200);
                Interlocked.Decrement(ref inFlight);
                return 0;
            }))));

        Assert.False(observedOverlap);
    }

    [Fact]
    public async Task PostsFromOneCaller_RunInPostOrder()
    {
        await using var lane = new ZLinkStateLane();
        var order = new List<int>();

        for (var i = 0; i < 100; i++)
        {
            var value = i;
            Assert.True(lane.TryPost(() =>
            {
                order.Add(value);
                return ValueTask.CompletedTask;
            }));
        }

        Assert.Equal(Enumerable.Range(0, 100), await lane.RunAsync(() => order.ToArray()));
    }

    [Fact]
    public async Task DrainingMoreThanOneBatch_StillRunsEveryItem()
    {
        //  The drain yields after a bounded batch. Everything queued past that boundary has to be
        //  picked up by the reschedule, not dropped.
        await using var lane = new ZLinkStateLane();
        var count = 0;

        for (var i = 0; i < 250; i++)
            Assert.True(lane.TryPost(() => { count++; return ValueTask.CompletedTask; }));

        Assert.Equal(250, await lane.RunAsync(() => count));
    }

    // ---- 재진입: 행 대신 진단 가능한 실패 ---------------------------------------------

    [Fact]
    public async Task ReenteringTheSameLane_FailsInsteadOfHanging()
    {
        await using var lane = new ZLinkStateLane();

        var error = await lane.RunAsync(() =>
            Assert.Throws<InvalidOperationException>(() => lane.RunAsync(() => 1)));

        Assert.Contains("already runs on the state lane", error.Message);
    }

    [Fact]
    public async Task IsOnLane_IsTrueOnlyInsideATurn()
    {
        await using var lane = new ZLinkStateLane();

        Assert.False(lane.IsOnLane);
        Assert.True(await lane.RunAsync(() => lane.IsOnLane));
        Assert.False(lane.IsOnLane);
    }

    [Fact]
    public async Task ADifferentLane_IsEnterableFromInsideATurn()
    {
        //  Reentrancy is per lane. Two components must still be able to call each other.
        await using var outer = new ZLinkStateLane();
        await using var inner = new ZLinkStateLane();

        var result = await outer.RunAsync(() => inner.RunAsync(() => 5).AsTask().Result);

        Assert.Equal(5, result);
    }

    // ---- 종료 ------------------------------------------------------------------------

    [Fact]
    public async Task DisposeAsync_WaitsForQueuedWork()
    {
        var lane = new ZLinkStateLane();
        var completed = 0;

        for (var i = 0; i < 200; i++)
            lane.TryPost(() => { completed++; return ValueTask.CompletedTask; });

        await lane.DisposeAsync();

        Assert.Equal(200, completed);
    }

    [Fact]
    public async Task RunAsync_AfterDispose_Throws()
    {
        var lane = new ZLinkStateLane();
        await lane.DisposeAsync();

        Assert.Throws<ObjectDisposedException>(() => lane.RunAsync(() => 1));
    }

    [Fact]
    public async Task TryPost_AfterDispose_ReportsRefusalInsteadOfThrowing()
    {
        var lane = new ZLinkStateLane();
        await lane.DisposeAsync();

        Assert.False(lane.TryPost(() => ValueTask.CompletedTask));
    }

    [Fact]
    public async Task DisposeAsync_IsIdempotent()
    {
        var lane = new ZLinkStateLane();

        await lane.DisposeAsync();
        await lane.DisposeAsync();
    }
}
