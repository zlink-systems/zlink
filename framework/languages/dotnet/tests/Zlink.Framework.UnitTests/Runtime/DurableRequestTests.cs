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
    public async Task Selected_route_change_preserves_the_started_binding_request(
        bool readyAtObservation
    )
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var route = new ZLinkPeerRouteObservation(1, 1, changed.Task);
        using var cancellation = new CancellationTokenSource();
        var pending = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var attempts = 0;
        var observations = 0;
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
                    Assert.Equal(cancellation.Token, token);
                    attempts++;
                    token.Register(() => pending.TrySetCanceled(token));
                    return new ValueTask<string>(pending.Task);
                },
                cancellation.Token,
                observeRoute: () =>
                {
                    observations++;
                    return route;
                }
            )
            .AsTask();
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        route = new(2, readyAtObservation ? 2UL : 0UL, ready.Task);
        changed.SetResult();
        Assert.False(request.IsCompleted);
        if (!readyAtObservation)
        {
            Assert.Equal(1, attempts);
            Assert.False(request.IsCompleted);
            route = new(2, 2, new TaskCompletionSource().Task);
            ready.SetResult();
        }
        pending.SetResult("terminal");
        Assert.Equal("terminal", await request.WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.Equal(1, attempts);
        Assert.Equal(1, observations);
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
    public async Task TypedNotConnectedPropagatesFirstBindingFailureWithOriginalBudget(
        bool wasAdmitted
    )
    {
        IReadOnlyList<ReadOnlyMemory<byte>> wire = [new byte[] { 28, 1, 47, 48, 49 }];
        var time = new ControllableTimeProvider();
        var timeout = TimeSpan.FromSeconds(8);
        var started = time.GetTimestamp();
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var route = new ZLinkPeerRouteObservation(1, 0, changed.Task);
        using var cancellation = new CancellationTokenSource();
        Exception expected = wasAdmitted
            ? new ZlinkRequestException(ZlinkRequestException.ErrorCode.NotConnected)
            : new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        var attempts = 0;
        var request = ZLinkDurableRequest
            .RequestAsync<string>(
                wire,
                started,
                timeout,
                (frames, remaining, token) =>
                {
                    Assert.Same(wire, frames);
                    Assert.Equal(TimeSpan.FromSeconds(5), remaining);
                    Assert.Equal(cancellation.Token, token);
                    attempts++;
                    return ValueTask.FromException<string>(expected);
                },
                cancellation.Token,
                () => route,
                timeProvider: time
            )
            .AsTask();
        Assert.Equal(0, attempts);
        time.Advance(TimeSpan.FromSeconds(3));
        route = new(1, 1, NoTopologyChange);
        changed.SetResult();
        var observed = await Record.ExceptionAsync(async () => await request);
        Assert.Same(expected, observed);
        Assert.Equal(1, attempts);
        Assert.Equal(0, time.ActiveTimerCount);
    }

    [Fact]
    public async Task ReadinessExpiryWaitsForTheExistingOwnerTerminalWithoutSubmitting()
    {
        var time = new ControllableTimeProvider();
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var terminal = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var route = new ZLinkPeerRouteObservation(1, 0, changed.Task);
        var expected = new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.DeadlineExceeded,
            "Owner deadline expired."
        );
        var attempts = 0;
        var request = ZLinkDurableRequest
            .RequestAsync<string>(
                [],
                time.GetTimestamp(),
                TimeSpan.FromSeconds(1),
                (_, _, _) =>
                {
                    attempts++;
                    return ValueTask.FromResult("unexpected");
                },
                CancellationToken.None,
                () => route,
                timeProvider: time,
                terminal: terminal.Task
            )
            .AsTask();
        time.Advance(TimeSpan.FromSeconds(1));
        route = new(1, 1, NoTopologyChange);
        changed.SetResult();
        Assert.False(request.IsCompleted);
        Assert.Equal(0, attempts);
        Assert.Equal(0, time.ActiveTimerCount);
        terminal.SetException(expected);
        var observed = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () => await request);
        Assert.Same(expected, observed);
        Assert.Equal(0, attempts);
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

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Connection_failure_is_terminal_even_after_recovery(bool recovered)
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var route = new ZLinkPeerRouteObservation(
            0,
            0,
            changed.Task,
            new ConnectionAttempt(
                "tcp://owner",
                11,
                1,
                0,
                ConnectionAttemptState.Connecting,
                default
            )
        );
        var submissions = 0;
        var request = ZLinkDurableRequest
            .RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(30),
                (_, _, _) =>
                {
                    submissions++;
                    return ValueTask.FromResult("unexpected");
                },
                CancellationToken.None,
                () => route
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        route = new(
            recovered ? 7UL : 0,
            recovered ? 7UL : 0,
            NoTopologyChange,
            new ConnectionAttempt(
                "tcp://owner",
                11,
                3,
                9,
                recovered ? ConnectionAttemptState.Connected : ConnectionAttemptState.AttemptFailed,
                default
            )
        );
        changed.SetResult();
        var error = await Assert.ThrowsAsync<ZlinkSubmitException>(async () =>
            await request.WaitAsync(TimeSpan.FromSeconds(3))
        );
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, error.Result);
        Assert.Equal(0, submissions);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Waiting_intent_removal_or_replacement_prevents_submission(bool replaced)
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var route = new ZLinkPeerRouteObservation(
            0,
            0,
            changed.Task,
            new ConnectionAttempt(
                "tcp://owner",
                11,
                1,
                0,
                ConnectionAttemptState.Connecting,
                default
            )
        );
        var submissions = 0;
        var request = ZLinkDurableRequest
            .RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(30),
                (_, _, _) =>
                {
                    submissions++;
                    return ValueTask.FromResult("unexpected");
                },
                CancellationToken.None,
                () => route
            )
            .AsTask();
        route = new(
            7,
            7,
            NoTopologyChange,
            replaced
                ? new ConnectionAttempt(
                    "tcp://owner",
                    12,
                    2,
                    0,
                    ConnectionAttemptState.Connected,
                    default
                )
                : null
        );
        changed.SetResult();
        var error = await Assert.ThrowsAsync<ZlinkSubmitException>(async () =>
            await request.WaitAsync(TimeSpan.FromSeconds(3))
        );
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, error.Result);
        Assert.Equal(0, submissions);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Missing_initial_core_row_preserves_first_failure_history(bool failed)
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var route = new ZLinkPeerRouteObservation(0, 0, changed.Task);
        var submissions = 0;
        var request = ZLinkDurableRequest
            .RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(30),
                (_, _, _) =>
                {
                    submissions++;
                    return ValueTask.FromResult("reply");
                },
                CancellationToken.None,
                () => route
            )
            .AsTask();
        Assert.Equal(0, submissions);
        route = new(
            7,
            7,
            NoTopologyChange,
            new ConnectionAttempt(
                "tcp://owner",
                11,
                2,
                failed ? 9UL : 0,
                ConnectionAttemptState.Connected,
                default
            )
        );
        changed.SetResult();
        if (failed)
        {
            var error = await Assert.ThrowsAsync<ZlinkSubmitException>(async () =>
                await request.WaitAsync(TimeSpan.FromSeconds(3))
            );
            Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, error.Result);
            Assert.Equal(0, submissions);
        }
        else
        {
            Assert.Equal("reply", await request.WaitAsync(TimeSpan.FromSeconds(3)));
            Assert.Equal(1, submissions);
        }
    }

    [Theory]
    [InlineData(0UL, false)]
    [InlineData(9UL, false)]
    [InlineData(0UL, true)]
    [InlineData(9UL, true)]
    public async Task Ready_snapshot_is_scanned_only_for_an_already_waiting_request(
        ulong failureId,
        bool initiallyMissing
    )
    {
        var changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var attempt = new ConnectionAttempt(
            "tcp://owner",
            11,
            1,
            0,
            ConnectionAttemptState.Connecting,
            default
        );
        var route = new ZLinkPeerRouteObservation(
            0,
            0,
            changed.Task,
            initiallyMissing ? null : attempt,
            attempt.Endpoint,
            initiallyMissing ? [] : [attempt]
        );
        var submissions = 0;
        var request = ZLinkDurableRequest
            .RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(30),
                (_, _, _) =>
                {
                    submissions++;
                    return ValueTask.FromResult("reply");
                },
                CancellationToken.None,
                () => route
            )
            .AsTask();
        route = new(
            7,
            7,
            NoTopologyChange,
            Attempts:
            [
                attempt with
                {
                    Revision = 2,
                    LastFailureId = failureId,
                    State = ConnectionAttemptState.Connected,
                },
            ]
        );
        changed.SetResult();
        if (failureId == 0)
        {
            Assert.Equal("reply", await request.WaitAsync(TimeSpan.FromSeconds(3)));
            Assert.Equal(1, submissions);
        }
        else
        {
            var error = await Assert.ThrowsAsync<ZlinkSubmitException>(async () =>
                await request.WaitAsync(TimeSpan.FromSeconds(3))
            );
            Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, error.Result);
            Assert.Equal(0, submissions);
        }
    }

    [Fact]
    public async Task Late_connection_and_admission_submit_once_with_the_remaining_budget()
    {
        var connected = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var admitted = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var observedConnection = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var attempt = new ConnectionAttempt(
            "inproc://late-owner",
            11,
            1,
            0,
            ConnectionAttemptState.Connecting,
            default
        );
        var route = new ZLinkPeerRouteObservation(0, 0, connected.Task, attempt);
        var submissions = 0;
        var timeout = TimeSpan.FromSeconds(30);
        var request = ZLinkDurableRequest
            .RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                timeout,
                (_, remaining, _) =>
                {
                    Assert.True(remaining > TimeSpan.Zero && remaining < timeout);
                    submissions++;
                    return ValueTask.FromResult("reply");
                },
                CancellationToken.None,
                () =>
                {
                    var current = route;
                    if (current.SelectedGeneration != 0)
                        observedConnection.TrySetResult();
                    return current;
                }
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        Assert.Equal(0, submissions);
        route = new(
            7,
            0,
            admitted.Task,
            attempt with
            {
                Revision = 2,
                State = ConnectionAttemptState.Connected,
            }
        );
        connected.SetResult();
        await observedConnection.Task.WaitAsync(TimeSpan.FromSeconds(3));
        Assert.False(request.IsCompleted);
        Assert.Equal(0, submissions);
        route = route with { ReadyGeneration = 7, Changed = NoTopologyChange };
        admitted.SetResult();
        Assert.Equal("reply", await request.WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.Equal(1, submissions);
    }

    [Fact]
    public async Task Ready_route_ignores_unrelated_outgoing_failure_history()
    {
        var submissions = 0;
        Assert.Equal(
            "reply",
            await ZLinkDurableRequest.RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.FromSeconds(30),
                (_, _, _) =>
                {
                    submissions++;
                    return ValueTask.FromResult("reply");
                },
                CancellationToken.None,
                () =>
                    new(
                        7,
                        7,
                        NoTopologyChange,
                        new ConnectionAttempt(
                            "tcp://owner",
                            11,
                            2,
                            9,
                            ConnectionAttemptState.AttemptFailed,
                            default
                        )
                    )
            )
        );
        Assert.Equal(1, submissions);
    }

    [Fact]
    public async Task Deadline_before_submission_is_deadline_exceeded()
    {
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await ZLinkDurableRequest.RequestAsync<string>(
                [],
                Stopwatch.GetTimestamp(),
                TimeSpan.Zero,
                (_, _, _) => throw new Xunit.Sdk.XunitException("Expired request submitted."),
                CancellationToken.None,
                ReadyRoute
            )
        );
        Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, error.Kind);
    }

    private static readonly Task NoTopologyChange = new TaskCompletionSource().Task;

    private static ZLinkPeerRouteObservation ReadyRoute() => new(1, 1, NoTopologyChange);
}
