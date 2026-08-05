using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Contracts.Errors;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorHandlerActivationTests
{
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
        var first = registry.GetOrCreate("actor-1")
            .HandlerInstances.Resolve<ProbeHandler>();
        var second = registry.GetOrCreate("actor-2")
            .HandlerInstances.Resolve<ProbeHandler>();

        await registry.ResetGenerationAsync();
        await registry.ResetGenerationAsync();

        Assert.Equal(1, first.DisposeCount);
        Assert.Equal(1, second.DisposeCount);
        Assert.Equal(2, probe.DisposedDependencies);
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
        var firstState = registry.GetOrCreate("actor-force-1");
        var secondState = registry.GetOrCreate("actor-force-2");
        var firstHandler = firstState.HandlerInstances.Resolve<BlockingHandler>();
        var secondHandler = secondState.HandlerInstances.Resolve<BlockingHandler>();
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "force-stop-handler",
            ZlinkStreamMetadata.Empty);
        var firstDispatch = firstState.ExecuteDispatchAsync(
                header,
                firstHandler.HandleAsync,
                CancellationToken.None)
            .AsTask();
        var secondDispatch = secondState.ExecuteDispatchAsync(
                header,
                secondHandler.HandleAsync,
                CancellationToken.None)
            .AsTask();
        await Task.WhenAll(
            firstHandler.Started.Task,
            secondHandler.Started.Task).WaitAsync(TimeSpan.FromSeconds(5));

        using var deadline = new CancellationTokenSource();
        var reset = registry.ResetGenerationAsync(deadline.Token).AsTask();
        Assert.Throws<InvalidOperationException>(() => firstState.HandlerInstances);
        Assert.Throws<InvalidOperationException>(() => secondState.HandlerInstances);
        Assert.Throws<ZLinkFrameworkException>(
            firstState.EnsureContextValid);
        Assert.Throws<ZLinkFrameworkException>(
            secondState.EnsureContextValid);

        deadline.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => reset);
        Assert.Equal(0, firstHandler.DisposeCount);
        Assert.Equal(0, secondHandler.DisposeCount);

        firstHandler.Release.TrySetResult();
        secondHandler.Release.TrySetResult();
        await Task.WhenAll(firstDispatch, secondDispatch)
            .WaitAsync(TimeSpan.FromSeconds(5));
        await WaitUntilAsync(
            () => firstHandler.DisposeCount == 1
                  && secondHandler.DisposeCount == 1
                  && Volatile.Read(ref probe.DisposedDependencies) == 2);
        Assert.Equal(2, probe.DisposedDependencies);
    }

    [Fact]
    public async Task Detached_Force_Stop_Cleanup_Reports_Disposal_Failure()
    {
        Exception? reported = null;
        await using var services = new ServiceCollection()
            .BuildServiceProvider();
        var registry = new ZLinkActorSessionRegistry(services);
        var state = registry.GetOrCreate("actor-force-failure");
        var handler = state.HandlerInstances.Resolve<FailingBlockingHandler>();
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "force-stop-failure",
            ZlinkStreamMetadata.Empty);
        var dispatch = state.ExecuteDispatchAsync(
                header,
                handler.HandleAsync,
                CancellationToken.None)
            .AsTask();
        await handler.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        using var deadline = new CancellationTokenSource();
        var reset = registry.ResetGenerationAsync(
                deadline.Token,
                exception => reported = exception)
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
        using var origin = new ZLinkRuntimeErrorSink(
            exception => processFailure = exception);
        origin.UnhandledCallbackException +=
            exception => originFailure = exception;
        var reporter = origin.CaptureGenerationReporter();
        origin.Dispose();
        using var successor = new ZLinkRuntimeErrorSink();
        successor.UnhandledCallbackException +=
            exception => successorFailure = exception;
        var failure = new InvalidOperationException(
            "origin generation cleanup failed");

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
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "blocking-handler",
            ZlinkStreamMetadata.Empty);

        var dispatch = state.ExecuteDispatchAsync(
                header,
                handler.HandleAsync,
                CancellationToken.None)
            .AsTask();
        await handler.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        state.BeginTeardown();
        var cleanup = state.BeginHandlerActivationCompletion(
                () =>
                {
                    state.ClearAfterDestroy();
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
        var state = registry.GetOrCreate("actor-reset-terminal");
        var handler = state.HandlerInstances.Resolve<BlockingHandler>();
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "reset-terminal",
            ZlinkStreamMetadata.Empty);

        var dispatch = state.ExecuteDispatchAsync(
                header,
                handler.HandleAsync,
                CancellationToken.None)
            .AsTask();
        await handler.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));

        var terminal = state.BeginHandlerActivationCompletion(
            () =>
            {
                state.ClearAfterDestroy();
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
        var mailbox = new ZLinkActorDispatchMailbox();
        var current = await mailbox.EnterAsync(CancellationToken.None);
        var accepted = mailbox.EnterAsync(CancellationToken.None).AsTask();
        var terminal = mailbox.CloseAdmissionAndReserveLifecycleBarrier();

        await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => mailbox.EnterAsync(CancellationToken.None).AsTask());

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
        var header = new ZlinkStreamHeader(
            ZlinkStreamMessageKind.Send,
            ZlinkStreamCodec.Json,
            ZlinkStreamHeaderFlags.None,
            null,
            "self-teardown",
            ZlinkStreamMetadata.Empty);
        Task<bool>? terminalTask = null;

        await state.ExecuteDispatchAsync(
            header,
            _ =>
            {
                state.BeginTeardown();
                var terminal = state.BeginHandlerActivationCompletion(
                    () =>
                    {
                        state.ClearAfterDestroy();
                        return true;
                    });
                Assert.True(terminal.RequiresDispatchRelease);
                Assert.False(terminal.Completion.IsCompleted);
                terminalTask = terminal.Completion;
                return ValueTask.CompletedTask;
            },
            CancellationToken.None);

        Assert.NotNull(terminalTask);
        Assert.True(
            await terminalTask.WaitAsync(TimeSpan.FromSeconds(5)));
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
            state.BeginTeardown();
            var terminal = state.BeginHandlerActivationCompletion(
                () =>
                {
                    state.ClearAfterDestroy();
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
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(5);
        while (!predicate())
        {
            if (DateTime.UtcNow >= deadline)
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

    private sealed class ProbeHandler(
        ScopedDependency dependency,
        LifetimeProbe probe) : IAsyncDisposable
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

    private sealed class BlockingHandler(ScopedDependency dependency)
        : IAsyncDisposable
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
            ValueTask.FromException(
                new InvalidOperationException("detached cleanup failed"));
    }
}
