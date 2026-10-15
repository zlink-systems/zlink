using System.Diagnostics;

namespace Zlink.Framework.UnitTests;

public sealed class DurableRequestTests
{
    [Fact]
    public async Task Readiness_cancellation_does_not_run_completion_inside_the_cancelling_turn()
    {
        using var cancellation = new CancellationTokenSource();
        using var cancellingTurn = new ThreadLocal<bool>();
        var request = ZLinkDurableRequest
            .RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(5),
                (_, _, _) => throw new Xunit.Sdk.XunitException("Unready request submitted."),
                cancellation.Token,
                () => new(1, 0, NoTopologyChange)
            )
            .AsTask();
        var completedInsideTurn = request.ContinueWith(
            _ => cancellingTurn.Value,
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default
        );
        await Task.Run(() =>
        {
            cancellingTurn.Value = true;
            cancellation.Cancel();
            cancellingTurn.Value = false;
        });
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () => await request);
        Assert.False(await completedInsideTurn);
    }

    [Fact]
    public async Task Initial_not_ready_waits_for_the_existing_topology_edge()
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var route = new ZLinkPeerRouteObservation(1, 0, changed.Task);
        var observations = 0;
        var attempts = 0;
        var request = ZLinkDurableRequest
            .RequestAsync(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(5),
                (_, _, _) =>
                {
                    attempts++;
                    return ValueTask.FromResult("terminal");
                },
                CancellationToken.None,
                () =>
                {
                    observations++;
                    return route;
                }
            )
            .AsTask();
        await Task.Delay(30);
        Assert.False(request.IsCompleted);
        Assert.Equal(0, attempts);
        Assert.Equal(1, observations);
        route = new(1, 1, NoTopologyChange);
        changed.SetResult();
        Assert.Equal("terminal", await request.WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.Equal(1, attempts);
    }

    [Fact]
    public async Task Untyped_timeout_is_not_a_transport_replay_signal()
    {
        var expected = new TimeoutException("untyped timeout");
        var attempts = 0;
        var actual = await Assert.ThrowsAsync<TimeoutException>(async () =>
            await ZLinkDurableRequest.RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(5),
                (_, _, _) =>
                {
                    attempts++;
                    return ValueTask.FromException<string>(expected);
                },
                CancellationToken.None,
                ReadyRoute
            )
        );
        Assert.Same(expected, actual);
        Assert.Equal(1, attempts);
    }

    [Fact]
    public async Task Terminal_reply_wins_an_observed_route_change()
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reply = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var route = new ZLinkPeerRouteObservation(1, 1, changed.Task);
        var attempts = 0;
        var request = ZLinkDurableRequest
            .RequestAsync(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(5),
                (_, _, _) =>
                {
                    attempts++;
                    return new ValueTask<string>(reply.Task);
                },
                CancellationToken.None,
                () => route
            )
            .AsTask();
        reply.SetResult("terminal");
        route = new(2, 2, NoTopologyChange);
        changed.SetResult();
        Assert.Equal("terminal", await request.WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.Equal(1, attempts);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Selected_route_change_replays_only_when_the_new_route_is_ready(
        bool readyAtObservation
    )
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var route = new ZLinkPeerRouteObservation(1, 1, changed.Task);
        var cancelled = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var pending = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var attempts = 0;
        var wire = new ReadOnlyMemory<byte>[] { new byte[] { 28, 1, 2 } };
        var request = ZLinkDurableRequest
            .RequestAsync(
                wire,
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(5),
                (frames, remaining, token) =>
                {
                    Assert.Same(wire, frames);
                    Assert.True(remaining > TimeSpan.FromSeconds(3));
                    if (++attempts == 1)
                    {
                        token.Register(() =>
                        {
                            pending.TrySetCanceled(token);
                            cancelled.TrySetResult();
                        });
                        return new ValueTask<string>(pending.Task);
                    }
                    return ValueTask.FromResult("terminal");
                },
                CancellationToken.None,
                observeRoute: () => route
            )
            .AsTask();
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        route = new(2, readyAtObservation ? 2UL : 0UL, ready.Task);
        changed.SetResult();
        await cancelled.Task.WaitAsync(TimeSpan.FromSeconds(3));
        if (!readyAtObservation)
        {
            Assert.Equal(1, attempts);
            Assert.False(request.IsCompleted);
            route = new(2, 2, new TaskCompletionSource().Task);
            ready.SetResult();
        }
        Assert.Equal("terminal", await request.WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.Equal(2, attempts);
    }

    [Fact]
    public async Task Unrelated_topology_change_does_not_cancel_an_attempt()
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var reply = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var route = new ZLinkPeerRouteObservation(7, 7, changed.Task);
        var request = ZLinkDurableRequest
            .RequestAsync(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(5),
                (_, _, token) =>
                {
                    token.Register(() => reply.TrySetCanceled(token));
                    return new ValueTask<string>(reply.Task);
                },
                CancellationToken.None,
                observeRoute: () => route
            )
            .AsTask();
        route = new(7, 7, new TaskCompletionSource().Task);
        changed.SetResult();
        reply.SetResult("terminal");
        Assert.Equal("terminal", await request.WaitAsync(TimeSpan.FromSeconds(3)));
    }

    [Fact]
    public async Task CapacityRefusalWithoutWaitTokenDoesNotReplay()
    {
        var expected = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.Backpressured);
        var attempts = 0;
        var failure = await Assert.ThrowsAsync<ZlinkSubmitException>(async () =>
            await ZLinkDurableRequest.RequestAsync(
                Array.Empty<ReadOnlyMemory<byte>>(),
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(5),
                (_, _, _) =>
                {
                    attempts++;
                    return ValueTask.FromException<IReadOnlyList<Message>>(expected);
                },
                CancellationToken.None,
                ReadyRoute
            )
        );
        Assert.Same(expected, failure);
        Assert.Equal(1, attempts);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task TypedNotConnectedReplaysIdenticalWireWithWholeRemainingDeadline(
        bool wasAdmitted
    )
    {
        IReadOnlyList<ReadOnlyMemory<byte>> wire = [new byte[] { 28, 1, 47, 48, 49 }];
        var timeout = TimeSpan.FromSeconds(8);
        var started = Stopwatch.GetTimestamp();
        var attempts = 0;
        var firstTimeout = TimeSpan.Zero;
        IReadOnlyList<Message> terminal = [Message.From("terminal"u8)];
        var result = await ZLinkDurableRequest.RequestAsync(
            wire,
            started,
            timeout,
            (frames, remaining, _) =>
            {
                Assert.Same(wire, frames);
                Assert.True(
                    remaining
                        <= timeout
                            - Stopwatch.GetElapsedTime(started)
                            + TimeSpan.FromMilliseconds(10)
                );
                if (++attempts == 1)
                {
                    firstTimeout = remaining;
                    Assert.True(remaining > TimeSpan.FromSeconds(5));
                    return ValueTask.FromException<IReadOnlyList<Message>>(
                        wasAdmitted
                            ? new ZlinkRequestException(
                                ZlinkRequestException.ErrorCode.NotConnected
                            )
                            : new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected)
                    );
                }
                Assert.True(remaining < firstTimeout);
                return ValueTask.FromResult(terminal);
            },
            CancellationToken.None,
            ReadyRoute
        );
        Assert.Same(terminal, result);
        ZLinkMessageParts.DisposeAll(result);
        Assert.Equal(2, attempts);
    }

    [Fact]
    public async Task AdmittedHistorySurvivesLaterSubmitFailures()
    {
        var attempts = 0;
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkDurableRequest.RequestAsync(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromMilliseconds(100),
                (_, _, _) =>
                    ValueTask.FromException<IReadOnlyList<Message>>(
                        ++attempts == 1
                            ? new ZlinkRequestException(
                                ZlinkRequestException.ErrorCode.NotConnected
                            )
                            : new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected)
                    ),
                CancellationToken.None,
                ReadyRoute
            )
        );
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
        Assert.True(attempts > 1);
    }

    [Fact]
    public async Task CancellationAndUntypedErrorsDoNotReplay()
    {
        var failure = new InvalidOperationException("invalid durable submission");
        var attempts = 0;
        var observed = await Assert.ThrowsAsync<InvalidOperationException>(async () =>
            await ZLinkDurableRequest.RequestAsync(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(1),
                (_, _, _) =>
                {
                    attempts++;
                    return ValueTask.FromException<IReadOnlyList<Message>>(failure);
                },
                CancellationToken.None,
                ReadyRoute
            )
        );
        Assert.Same(failure, observed);
        Assert.Equal(1, attempts);
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await ZLinkDurableRequest.RequestAsync<IReadOnlyList<Message>>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(1),
                (_, _, _) => throw new Xunit.Sdk.XunitException("Cancelled operation submitted."),
                cancellation.Token,
                ReadyRoute
            )
        );
    }

    private static readonly Task NoTopologyChange = new TaskCompletionSource().Task;

    private static ZLinkPeerRouteObservation ReadyRoute() => new(1, 1, NoTopologyChange);
}
