namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RuntimeConcurrencyBoundaryTests
{
#if DEBUG
    [Fact]
    public async Task InfrastructureWaitGuard_RejectsIncompleteWaitOnStateLane()
    {
        await using var lane = new ZLinkStateLane();
        var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var error = await lane.RunAsync(() =>
            Record.Exception(() =>
                ZLinkInfrastructureWaitGuard.ThrowIfBlocking(
                    pending.Task.IsCompleted,
                    "pending request"
                )
            )
        );

        Assert.IsType<InvalidOperationException>(error);
        Assert.Contains("pending request", error.Message);
        ZLinkInfrastructureWaitGuard.ThrowIfBlocking(true, "completed request");
        ZLinkInfrastructureWaitGuard.ThrowIfBlocking(false, "outside lane");
    }

    [Fact]
    public async Task InfrastructureWaitGuard_RejectsIncompleteWaitOnLifecycleTurn()
    {
        var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var queue = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None
        );
        var pending = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Exception? observed = null;

        await queue.RunLifecycleAsync(
            _ =>
            {
                observed = Record.Exception(() =>
                    ZLinkInfrastructureWaitGuard.ThrowIfBlocking(
                        pending.Task.IsCompleted,
                        "lifecycle completion"
                    )
                );
                ZLinkInfrastructureWaitGuard.ThrowIfBlocking(true, "completed lifecycle operation");
                return ValueTask.CompletedTask;
            },
            CancellationToken.None
        );

        Assert.IsType<InvalidOperationException>(observed);
    }
#endif

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task RunnerCompletion_PreservesContextWithoutSupervisorLaneOwnership(
        bool longRunning
    )
    {
        var runner = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            CancellationToken.None
        );
        var ambient = new AsyncLocal<string?> { Value = "caller" };
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var observed = new TaskCompletionSource<(string?, bool, bool)>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        async ValueTask Callback(CancellationToken token)
        {
            entered.TrySetResult();
            await release.Task.ConfigureAwait(false);
            observed.TrySetResult(
                (ambient.Value, runner.IsCurrentExecution, ZLinkStateLane.Current is null)
            );
        }
        var execution = longRunning
            ? runner.RunLongRunning("context", Callback)
            : runner.Run("context", Callback);
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var stop = runner.StopAsync().AsTask();
        Assert.False(stop.IsCompleted);
        release.TrySetResult();
        Assert.Equal(
            ("caller", true, true),
            await observed.Task.WaitAsync(TimeSpan.FromSeconds(5))
        );
        await execution.WaitAsync(TimeSpan.FromSeconds(5));
        await stop.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(runner.IsCurrentExecution);
        Assert.Equal("caller", ambient.Value);
    }

    [Fact]
    public async Task Runner_can_stop_a_sibling_with_the_same_execution_owner()
    {
        var owner = new object();
        var reporter = new ZLinkRuntimeErrorSink();
        var root = new ZLinkRuntimeTaskRunner(reporter, CancellationToken.None, owner);
        var sibling = new ZLinkRuntimeTaskRunner(reporter, CancellationToken.None, owner);

        await root.Run("stop-sibling", async _ => await sibling.StopAsync())
            .WaitAsync(TimeSpan.FromSeconds(5));

        await root.StopAsync();
    }

    [Fact]
    public async Task BoundSessionDeferredScope_ReportsTypedPressureAndPreservesBooleanCompatibility()
    {
        const string actorId = "magic-dotnet-deferred-pressure";
        Assert.Null(
            ZLinkBoundSessionDispatchScope.TrySubmitDeferred(actorId, _ => ValueTask.CompletedTask)
        );
        var completed = 0;
        await using var scope = ZLinkBoundSessionDispatchScope.Enter(actorId);
        for (var index = 0; index < 4096; index++)
            Assert.Equal(
                ZLinkOneWaySubmitStatus.Submitted,
                ZLinkBoundSessionDispatchScope.TrySubmitDeferred(
                    actorId,
                    _ =>
                    {
                        completed++;
                        return ValueTask.CompletedTask;
                    }
                )
            );
        Assert.Equal(
            ZLinkOneWaySubmitStatus.Backpressured,
            ZLinkBoundSessionDispatchScope.TrySubmitDeferred(actorId, _ => ValueTask.CompletedTask)
        );
        Assert.Throws<InvalidOperationException>(() =>
            ZLinkBoundSessionDispatchScope.TryDefer(actorId, _ => ValueTask.CompletedTask)
        );
        await scope.DrainAsync(CancellationToken.None);
        Assert.Equal(4096, completed);
        Assert.Null(
            ZLinkBoundSessionDispatchScope.TrySubmitDeferred(actorId, _ => ValueTask.CompletedTask)
        );
    }

    [Fact]
    public async Task BoundSessionDeferredScope_DrainsOperationAddedWhileAnotherOperationIsRunning()
    {
        var firstStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseFirst = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var order = new List<string>();
        await using var scope = ZLinkBoundSessionDispatchScope.Enter("actor-1");

        Assert.True(
            ZLinkBoundSessionDispatchScope.TryDefer(
                "actor-1",
                async _ =>
                {
                    firstStarted.TrySetResult();
                    await releaseFirst.Task;
                    lock (order)
                        order.Add("first");
                }
            )
        );

        var drain = scope.DrainAsync(CancellationToken.None).AsTask();
        await firstStarted.Task;
        var added = await Task.Run(() =>
            ZLinkBoundSessionDispatchScope.TryDefer(
                "actor-1",
                _ =>
                {
                    lock (order)
                        order.Add("second");
                    return ValueTask.CompletedTask;
                }
            )
        );
        Assert.True(added);

        releaseFirst.TrySetResult();
        await drain;
        Assert.Equal(["first", "second"], order);
    }

    [Fact]
    public async Task RuntimeTaskRunner_RejectsStoppingItselfInsteadOfDeadlocking()
    {
        using var shutdown = new CancellationTokenSource();
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), shutdown.Token);
        var observed = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        runner.RunDetached(
            "self-stop",
            async _ =>
            {
                try
                {
                    await runner.StopAsync();
                }
                catch (Exception exception)
                {
                    observed.TrySetResult(exception);
                }
            }
        );

        var exception = await observed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.IsType<InvalidOperationException>(exception);
        shutdown.Cancel();
        await runner.StopAsync();
    }

    [Fact]
    public async Task RuntimeExecutionOwner_IsSharedAcrossNestedTaskRunners()
    {
        using var shutdown = new CancellationTokenSource();
        var owner = new object();
        var root = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), shutdown.Token, owner);
        var nested = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), shutdown.Token, owner);
        var observed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        nested.RunDetached(
            "nested",
            _ =>
            {
                Assert.True(root.IsCurrentExecution);
                Assert.True(ZLinkRuntimeTaskRunner.IsCurrentExecutionFor(owner));
                observed.TrySetResult();
                return ValueTask.CompletedTask;
            }
        );

        await observed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        shutdown.Cancel();
        await nested.StopAsync();
        await root.StopAsync();
    }

    [Fact]
    public async Task RuntimeTaskRunner_StopDrainsChildrenScheduledByAnAdmittedTask()
    {
        using var shutdown = new CancellationTokenSource();
        var runner = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), shutdown.Token);
        var parentStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var scheduleChild = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var childRan = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        runner.RunDetached(
            "parent",
            async _ =>
            {
                parentStarted.TrySetResult();
                await scheduleChild.Task;
                Assert.True(
                    runner.TryRunDetached(
                        "child",
                        _ =>
                        {
                            childRan.TrySetResult();
                            return ValueTask.CompletedTask;
                        }
                    )
                );
            }
        );

        await parentStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var stop = runner.StopAsync().AsTask();
        scheduleChild.TrySetResult();
        await childRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await stop.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.False(runner.TryRunDetached("external", _ => ValueTask.CompletedTask));
    }

    [Fact]
    public async Task RuntimeTaskSupervisor_DrainsChildRunnerAndItsLateRootCleanup()
    {
        using var shutdown = new CancellationTokenSource();
        var scope = new ZLinkRuntimeExecutionScope();
        var root = new ZLinkRuntimeTaskRunner(
            new ZLinkRuntimeErrorSink(),
            shutdown.Token,
            scope,
            ownsSupervisor: true
        );
        var child = new ZLinkRuntimeTaskRunner(new ZLinkRuntimeErrorSink(), shutdown.Token, scope);
        var childStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseChild = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var cleanupRan = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        child.RunDetached(
            "child",
            async _ =>
            {
                childStarted.TrySetResult();
                await releaseChild.Task;
                Assert.True(
                    root.TryRunDetached(
                        "root-cleanup",
                        _ =>
                        {
                            cleanupRan.TrySetResult();
                            return ValueTask.CompletedTask;
                        }
                    )
                );
            }
        );

        await childStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        var stop = root.StopAsync().AsTask();
        releaseChild.TrySetResult();
        await cleanupRan.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await stop.WaitAsync(TimeSpan.FromSeconds(5));
        await child.StopAsync();
    }

    [Fact]
    public async Task BoundSessionDeferredScope_RetainsFailedHeadBeforeLaterOperations()
    {
        var attempts = 0;
        var order = new List<string>();
        await using var scope = ZLinkBoundSessionDispatchScope.Enter("actor-1");
        Assert.True(
            ZLinkBoundSessionDispatchScope.TryDefer(
                "actor-1",
                _ =>
                {
                    attempts++;
                    if (attempts == 1)
                        throw new InvalidOperationException("transient");
                    order.Add("first");
                    return ValueTask.CompletedTask;
                }
            )
        );
        Assert.True(
            ZLinkBoundSessionDispatchScope.TryDefer(
                "actor-1",
                _ =>
                {
                    order.Add("second");
                    return ValueTask.CompletedTask;
                }
            )
        );

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            scope.DrainAsync(CancellationToken.None).AsTask()
        );
        Assert.Empty(order);

        await scope.DrainAsync(CancellationToken.None);
        Assert.Equal(2, attempts);
        Assert.Equal(["first", "second"], order);
    }
}
