using System.Collections.Concurrent;
using System.Diagnostics;
using System.Reflection;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Identifiers;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorHandlerActivationTests : IAsyncLifetime
{
    private readonly ActorStateLifetime _states = new();

    public Task InitializeAsync() => Task.CompletedTask;

    public Task DisposeAsync() => _states.DisposeAsync().AsTask();

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task IncomingHandoffWaitsForTerminalAfterReturningItsMailboxClaim(
        bool sharedParent
    )
    {
        using var errors = new ZLinkRuntimeErrorSink();
        var runner = new ZLinkRuntimeTaskRunner(errors, CancellationToken.None);
        await using var parent = new ZLinkSerialExecutionQueue(
            runner,
            errors,
            CancellationToken.None,
            ZLinkExecutionLanePolicy.Default,
            sharedGate: true
        );
        ZLinkSerialExecutionQueue? owner = sharedParent ? parent : null;
        var state = new ZLinkActorRuntimeState("incoming-terminal-placement");
        var mailbox = (ZLinkActorSerialExecutor)
            typeof(ZLinkActorRuntimeState)
                .GetField("_dispatchMailbox", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(state)!;
        var queue = (ZLinkSerialExecutionQueue)
            typeof(ZLinkActorSerialExecutor)
                .GetField("_queue", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(mailbox)!;
        if (sharedParent)
            mailbox.BindExecutionOwner(() => owner);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? preparing = null;
        var terminal = state
            .BeginHandlerActivationAsyncCompletion(async () =>
            {
                entered.TrySetResult();
                await release.Task;
                return await state.ExecuteLockedAsync(
                    () =>
                    {
                        state.ClearAfterDestroyOnLane();
                        owner = null;
                        return true;
                    },
                    CancellationToken.None
                );
            })
            .Completion;
        try
        {
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            // RuntimeActors performs the finite Import turn before waiting for
            // the previous terminal in PrepareForTransferredActivationAsync.
            Assert.True(
                await state.ExecuteHandoffTransitionAsync(static () => true, CancellationToken.None)
            );
            preparing = state.PrepareForTransferredActivationAsync(CancellationToken.None).AsTask();
            Assert.False(preparing.IsCompleted);
            release.TrySetResult();
            await Task.WhenAll(terminal, preparing).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(queue.HasSharedConsumer);
            await parent.DisposeAsync();
            await state.ExecuteDispatchAsync(
                static _ => ValueTask.CompletedTask,
                CancellationToken.None
            );
        }
        finally
        {
            release.TrySetResult();
            await terminal;
            if (preparing is not null)
                await preparing;
            await state.InvalidateRuntimeGenerationAfterDispatchesAsync();
            await parent.DisposeAsync();
            await runner.StopAsync();
        }
    }

    [Fact]
    public async Task ActorLifecycle_UsesLifecycleLaneBeforeQueuedApplication()
    {
        var state = _states.Track(new ZLinkActorRuntimeState("actor-lifecycle-lane"));
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var order = new ConcurrentQueue<string>();
        var active = state
            .ExecuteDispatchAsync(
                async _ =>
                {
                    started.TrySetResult();
                    await release.Task;
                },
                CancellationToken.None
            )
            .AsTask();
        await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
        try
        {
            var application = state
                .ExecuteDispatchAsync(
                    _ =>
                    {
                        order.Enqueue("application");
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask();
            var lifecycle = state
                .ExecuteLifecycleAsync(
                    _ =>
                    {
                        order.Enqueue("lifecycle");
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask();
            release.TrySetResult();
            await Task.WhenAll(active, application, lifecycle).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(new[] { "lifecycle", "application" }, order.ToArray());
        }
        finally
        {
            release.TrySetResult();
            await active;
        }
    }

    [Fact]
    public async Task Teardown_DestroyedBetweenPreparationAndParticipation_IsIdempotent()
    {
        var state = new ZLinkActorRuntimeState("actor-terminal-race");
        var nativeActor = new ZLinkBackendActorRef(RoutingId.From("entry"), state.ActorId, 1);
        state.BindNativeActorRef(nativeActor);
        var first = await state.ExecuteLockedAsync(
            () => state.BeginOrJoinTeardownAttemptOnLane(nativeActor),
            CancellationToken.None
        );
        // Reproduce the public Destroy snapshot turn before an earlier self-destroy terminal.
        var participant = await state.ExecuteLockedAsync(
            () => state.BeginOrJoinTeardownAttemptOnLane(nativeActor),
            CancellationToken.None
        );
        Assert.False(participant.OwnsExecution);
        Assert.Same(first.Completion, participant.Completion);
        await state.ExecuteLockedAsync(
            () => state.MarkNativeDestroyed(first),
            CancellationToken.None
        );
        await state.ExecuteLockedAsync(
            () => state.CompleteTeardownAttemptOnLane(first),
            CancellationToken.None
        );
        var repeated = await state.ExecuteLockedAsync(
            () => state.BeginOrJoinTeardownAttemptOnLane(nativeActor),
            CancellationToken.None
        );
        Assert.False(repeated.OwnsExecution);
        Assert.Null(await repeated.Completion);
        Assert.Null(state.NativeActorRef);
        Assert.False(state.IsTeardownPending);
        Assert.Null(await participant.Completion);
    }

    [Fact]
    public async Task Teardown_StaleSnapshot_DoesNotDestroyReplacement()
    {
        var state = new ZLinkActorRuntimeState("actor-replaced");
        var original = new ZLinkBackendActorRef(RoutingId.From("entry"), state.ActorId, 1);
        var replacement = original with { Generation = 2 };
        state.BindNativeActorRef(replacement);
        var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await state.ExecuteLockedAsync(
                () => state.BeginOrJoinTeardownAttemptOnLane(original),
                CancellationToken.None
            )
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, failure.Kind);
        Assert.Equal(replacement, state.NativeActorRef);
        Assert.False(state.IsTeardownPending);
        Assert.False(state.ContextInvalidated);
    }

    [Fact]
    public async Task Actor_Activations_Own_Separate_Handler_Instances_And_Scoped_Dependencies()
    {
        var probe = new LifetimeProbe();
        var registered = new ProbeHandler(new ScopedDependency(probe), probe);
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ScopedDependency>()
            .AddSingleton(registered)
            .BuildServiceProvider();
        var firstState = new ZLinkActorRuntimeState("actor-1", services: services);
        var secondState = new ZLinkActorRuntimeState("actor-2", services: services);

        var first = firstState.HandlerInstances.Resolve<ProbeHandler>();
        var firstAgain = firstState.HandlerInstances.Resolve<ProbeHandler>();
        var second = secondState.HandlerInstances.Resolve<ProbeHandler>();

        Assert.Same(first, firstAgain);
        Assert.NotSame(registered, first);
        Assert.NotSame(first, second);
        Assert.NotSame(first.Dependency, second.Dependency);

        await firstState.DisposeHandlerActivationAsync();
        await firstState.DisposeHandlerActivationAsync();
        Assert.Equal(1, first.DisposeCount);
        Assert.Equal(1, first.Dependency.DisposeCount);
        Assert.Equal(0, second.DisposeCount);

        await secondState.DisposeHandlerActivationAsync();
        Assert.Equal(1, second.DisposeCount);
        Assert.Equal(1, second.Dependency.DisposeCount);
    }

    [Fact]
    public async Task Runtime_Generation_Reset_Disposes_Each_Actor_Activation_Once()
    {
        var probe = new LifetimeProbe();
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ScopedDependency>()
            .BuildServiceProvider();
        var registry = new ZLinkActorSessionRegistry(services);
        var first = registry
            .GetOrCreate(ZLinkActorId.FromBoundary("actor-1", "actorId"))
            .HandlerInstances.Resolve<ProbeHandler>();
        var second = registry
            .GetOrCreate(ZLinkActorId.FromBoundary("actor-2", "actorId"))
            .HandlerInstances.Resolve<ProbeHandler>();

        await registry.ResetGenerationAsync();
        await registry.ResetGenerationAsync();

        Assert.Equal(1, first.DisposeCount);
        Assert.Equal(1, second.DisposeCount);
        Assert.Equal(2, probe.DisposedDependencies);
    }

    [Fact]
    public async Task Runtime_Generation_Reset_Fences_States_Before_The_Next_Registry_Turn()
    {
        var registry = new ZLinkActorSessionRegistry();
        var state = registry.GetOrCreate(ZLinkActorId.FromBoundary("actor-reset-fence", "actorId"));
        var lane = Assert.IsType<ZLinkStateLane>(
            typeof(ZLinkActorSessionRegistry)
                .GetField("_lane", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(registry)
        );
        var blockerStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var releaseBlocker = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        _ = lane.RunAsync(async () =>
        {
            blockerStarted.TrySetResult();
            await releaseBlocker.Task.ConfigureAwait(false);
        });
        await blockerStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        // The lane's queued turns; only their count is observed here.
        var mailbox = Assert.IsAssignableFrom<System.Collections.ICollection>(
            typeof(ZLinkStateLane)
                .GetField("_mailbox", BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(lane)
        );

        var reset = Task.Run(async () => await registry.ResetGenerationAsync());
        Assert.True(SpinWait.SpinUntil(() => mailbox.Count == 1, TimeSpan.FromSeconds(5)));
        var observedByNextTurn = lane.RunAsync(() => state.ContextInvalidated).AsTask();

        releaseBlocker.TrySetResult();

        Assert.True(await observedByNextTurn.WaitAsync(TimeSpan.FromSeconds(5)));
        await reset.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Force_Stop_Closes_All_Actor_Activations_And_Respects_Its_Deadline()
    {
        var probe = new LifetimeProbe();
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ScopedDependency>()
            .BuildServiceProvider();
        var registry = new ZLinkActorSessionRegistry(services);
        var firstState = registry.GetOrCreate(
            ZLinkActorId.FromBoundary("actor-force-1", "actorId")
        );
        var secondState = registry.GetOrCreate(
            ZLinkActorId.FromBoundary("actor-force-2", "actorId")
        );
        var firstHandler = firstState.HandlerInstances.Resolve<BlockingHandler>();
        var secondHandler = secondState.HandlerInstances.Resolve<BlockingHandler>();
        var firstDispatch = firstState
            .ExecuteDispatchAsync(firstHandler.HandleAsync, CancellationToken.None)
            .AsTask();
        var secondDispatch = secondState
            .ExecuteDispatchAsync(secondHandler.HandleAsync, CancellationToken.None)
            .AsTask();
        await Task.WhenAll(firstHandler.Started.Task, secondHandler.Started.Task)
            .WaitAsync(TimeSpan.FromSeconds(5));

        using var deadline = new CancellationTokenSource();
        var reset = registry.ResetGenerationAsync(deadline.Token).AsTask();
        Assert.Throws<InvalidOperationException>(() => firstState.HandlerInstances);
        Assert.Throws<InvalidOperationException>(() => secondState.HandlerInstances);
        Assert.Throws<ZLinkFrameworkException>(firstState.EnsureContextValid);
        Assert.Throws<ZLinkFrameworkException>(secondState.EnsureContextValid);

        deadline.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => reset);
        Assert.Equal(0, firstHandler.DisposeCount);
        Assert.Equal(0, secondHandler.DisposeCount);

        firstHandler.Release.TrySetResult();
        secondHandler.Release.TrySetResult();
        await Task.WhenAll(firstDispatch, secondDispatch).WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(() =>
            firstHandler.DisposeCount == 1
            && secondHandler.DisposeCount == 1
            && Volatile.Read(ref probe.DisposedDependencies) == 2
        );
        Assert.Equal(2, probe.DisposedDependencies);
    }

    [Fact]
    public async Task Detached_Force_Stop_Cleanup_Reports_Disposal_Failure()
    {
        Exception? reported = null;
        await using var services = new ServiceCollection().BuildServiceProvider();
        var registry = new ZLinkActorSessionRegistry(services);
        var state = registry.GetOrCreate(
            ZLinkActorId.FromBoundary("actor-force-failure", "actorId")
        );
        var handler = state.HandlerInstances.Resolve<FailingBlockingHandler>();
        var dispatch = state
            .ExecuteDispatchAsync(handler.HandleAsync, CancellationToken.None)
            .AsTask();
        await handler.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var deadline = new CancellationTokenSource();
        var reset = registry
            .ResetGenerationAsync(deadline.Token, exception => reported = exception)
            .AsTask();
        deadline.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => reset);

        handler.Release.TrySetResult();
        await dispatch.WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(() => reported is not null);
        Assert.IsType<InvalidOperationException>(reported);
    }

    [Fact]
    public void Captured_Generation_Reporter_Survives_Detach_Without_Using_Successor_Sink()
    {
        Exception? processFailure = null;
        Exception? originFailure = null;
        Exception? successorFailure = null;
        using var origin = new ZLinkRuntimeErrorSink(exception => processFailure = exception);
        origin.UnhandledCallbackException += exception => originFailure = exception;
        var reporter = origin.CaptureGenerationReporter();
        origin.Dispose();
        using var successor = new ZLinkRuntimeErrorSink();
        successor.UnhandledCallbackException += exception => successorFailure = exception;
        var failure = new InvalidOperationException("origin generation cleanup failed");

        reporter(failure);

        Assert.Same(failure, processFailure);
        Assert.Same(failure, originFailure);
        Assert.Null(successorFailure);
    }

    [Fact]
    public async Task Terminal_Cleanup_Waits_For_InFlight_Handler_And_Rejects_Recreation()
    {
        var probe = new LifetimeProbe();
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ScopedDependency>()
            .BuildServiceProvider();
        var state = new ZLinkActorRuntimeState("actor-race", services: services);
        var handler = state.HandlerInstances.Resolve<BlockingHandler>();

        var dispatch = state
            .ExecuteDispatchAsync(handler.HandleAsync, CancellationToken.None)
            .AsTask();
        await handler.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        state.BindNativeActorRef(
            new ZLinkBackendActorRef(RoutingId.From("entry"), state.ActorId, 1)
        );
        await state.ExecuteLockedAsync(
            () => state.BeginOrJoinTeardownAttemptOnLane(state.NativeActorRef!.Value),
            CancellationToken.None
        );
        var cleanup = state
            .BeginHandlerActivationCompletion(() =>
            {
                state.ClearAfterDestroyOnLane();
                return true;
            })
            .Completion;
        await Task.Delay(50);

        Assert.False(cleanup.IsCompleted);
        Assert.Throws<InvalidOperationException>(() => state.HandlerInstances);
        Assert.Equal(0, handler.DisposeCount);
        Assert.Equal(0, handler.Dependency.DisposeCount);

        handler.Release.TrySetResult();
        await dispatch.WaitAsync(TimeSpan.FromSeconds(5));
        await cleanup.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(1, handler.DisposeCount);
        Assert.Equal(1, handler.Dependency.DisposeCount);
        Assert.Throws<InvalidOperationException>(() => state.HandlerInstances);
    }

    [Fact]
    public async Task Runtime_Generation_Reset_Waits_For_Existing_Terminal_Cleanup()
    {
        var probe = new LifetimeProbe();
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ScopedDependency>()
            .BuildServiceProvider();
        var registry = new ZLinkActorSessionRegistry(services);
        var state = registry.GetOrCreate(
            ZLinkActorId.FromBoundary("actor-reset-terminal", "actorId")
        );
        var handler = state.HandlerInstances.Resolve<BlockingHandler>();

        var dispatch = state
            .ExecuteDispatchAsync(handler.HandleAsync, CancellationToken.None)
            .AsTask();
        await handler.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var terminal = state.BeginHandlerActivationCompletion(() =>
        {
            state.ClearAfterDestroyOnLane();
            return true;
        });
        var reset = registry.ResetGenerationAsync().AsTask();

        Assert.False(reset.IsCompleted);
        handler.Release.TrySetResult();

        await dispatch.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.True(await terminal.Completion.WaitAsync(TimeSpan.FromSeconds(5)));
        await reset.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(1, handler.DisposeCount);
        Assert.Equal(1, handler.Dependency.DisposeCount);
    }

    [Fact]
    public async Task Terminal_Barrier_Runs_After_Already_Accepted_Turns_And_Closes_Admission()
    {
        await using var mailbox = new ZLinkActorSerialExecutor();
        var current = await mailbox.EnterAsync(CancellationToken.None);
        var accepted = mailbox.EnterAsync(CancellationToken.None).AsTask();
        var terminal = mailbox.CloseAdmissionAndReserveLifecycleBarrier();

        await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            mailbox.EnterAsync(CancellationToken.None).AsTask()
        );

        current.Dispose();
        var acceptedTurn = await accepted.WaitAsync(TimeSpan.FromSeconds(5));
        var terminalTurn = terminal.ClaimAsync().AsTask();

        Assert.False(terminalTurn.IsCompleted);

        acceptedTurn.Dispose();
        using var claimed = await terminalTurn.WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task Self_Initiated_Teardown_Is_Completed_After_Current_Dispatch_Returns()
    {
        var probe = new LifetimeProbe();
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ScopedDependency>()
            .BuildServiceProvider();
        var state = new ZLinkActorRuntimeState("actor-self", services: services);
        var handler = state.HandlerInstances.Resolve<ProbeHandler>();
        Task<bool>? terminalTask = null;

        await state.ExecuteDispatchAsync(
            async _ =>
            {
                state.BindNativeActorRef(
                    new ZLinkBackendActorRef(RoutingId.From("entry"), state.ActorId, 1)
                );
                await state.ExecuteLockedAsync(
                    () => state.BeginOrJoinTeardownAttemptOnLane(state.NativeActorRef!.Value),
                    CancellationToken.None
                );
                var terminal = state.BeginHandlerActivationCompletion(() =>
                {
                    state.ClearAfterDestroyOnLane();
                    return true;
                });
                Assert.True(terminal.RequiresDispatchRelease);
                Assert.False(terminal.Completion.IsCompleted);
                terminalTask = terminal.Completion;
            },
            CancellationToken.None
        );

        Assert.NotNull(terminalTask);
        Assert.True(await terminalTask.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal(1, handler.DisposeCount);
        Assert.Equal(1, handler.Dependency.DisposeCount);
        Assert.Throws<InvalidOperationException>(() => state.HandlerInstances);
    }

    [Fact]
    public async Task Deferred_Join_Ownership_Marks_Teardown_For_Post_Callback_Release()
    {
        var probe = new LifetimeProbe();
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<ScopedDependency>()
            .BuildServiceProvider();
        var state = new ZLinkActorRuntimeState("actor-deferred-join", services: services);
        var handler = state.HandlerInstances.Resolve<ProbeHandler>();
        Task<bool> cleanup;

        using (state.EnterDeferredJoinExecution())
        {
            state.BindNativeActorRef(
                new ZLinkBackendActorRef(RoutingId.From("entry"), state.ActorId, 1)
            );
            await state.ExecuteLockedAsync(
                () => state.BeginOrJoinTeardownAttemptOnLane(state.NativeActorRef!.Value),
                CancellationToken.None
            );
            var terminal = state.BeginHandlerActivationCompletion(() =>
            {
                state.ClearAfterDestroyOnLane();
                return true;
            });

            // A relocated Entry Spot joined callback executes outside the
            // actor mailbox. DestroyActorAsync must therefore release the
            // terminal cleanup only after that callback returns.
            Assert.True(terminal.RequiresDispatchRelease);
            cleanup = terminal.Completion;
        }

        Assert.True(await cleanup.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal(1, handler.DisposeCount);
        Assert.Equal(1, handler.Dependency.DisposeCount);
        Assert.Throws<InvalidOperationException>(() => state.HandlerInstances);
    }

    private sealed class LifetimeProbe
    {
        public int DisposedDependencies;
    }

    private static async Task WaitUntilAsync(Func<bool> predicate)
    {
        var deadlineTimeout = TimeSpan.FromSeconds(5);
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (!predicate())
        {
            if (Stopwatch.GetElapsedTime(deadlineStarted) >= deadlineTimeout)
                throw new TimeoutException("The expected cleanup did not complete.");
            await Task.Delay(10);
        }
    }

    private sealed class ScopedDependency(LifetimeProbe probe) : IAsyncDisposable
    {
        public int DisposeCount { get; private set; }

        public ValueTask DisposeAsync()
        {
            DisposeCount++;
            Interlocked.Increment(ref probe.DisposedDependencies);
            return ValueTask.CompletedTask;
        }
    }

    private sealed class ProbeHandler(ScopedDependency dependency, LifetimeProbe probe)
        : IAsyncDisposable
    {
        public ScopedDependency Dependency { get; } = dependency;

        public int DisposeCount { get; private set; }

        public ValueTask DisposeAsync()
        {
            _ = probe;
            DisposeCount++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class BlockingHandler(ScopedDependency dependency) : IAsyncDisposable
    {
        public ScopedDependency Dependency { get; } = dependency;

        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public int DisposeCount { get; private set; }

        public async ValueTask HandleAsync(CancellationToken cancellationToken)
        {
            Started.TrySetResult();
            await Release.Task.WaitAsync(cancellationToken);
        }

        public ValueTask DisposeAsync()
        {
            DisposeCount++;
            return ValueTask.CompletedTask;
        }
    }

    private sealed class FailingBlockingHandler : IAsyncDisposable
    {
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask HandleAsync(CancellationToken cancellationToken)
        {
            Started.TrySetResult();
            await Release.Task.WaitAsync(cancellationToken);
        }

        public ValueTask DisposeAsync() =>
            ValueTask.FromException(new InvalidOperationException("detached cleanup failed"));
    }
}

internal sealed class ActorStateLifetime : IAsyncDisposable
{
    private readonly List<ZLinkActorRuntimeState> _states = [];

    internal ZLinkActorRuntimeState Track(ZLinkActorRuntimeState state)
    {
        _states.Add(state);
        return state;
    }

    public async ValueTask DisposeAsync()
    {
        var failures = new Zlink.Framework.Runtime.ZLinkFailureCollector();
        foreach (var state in _states)
            state.FenceRuntimeGeneration();
        foreach (var state in _states)
            await failures.CaptureAsync(state.InvalidateRuntimeGenerationAfterDispatchesAsync);
        failures.ThrowIfAny();
    }
}
