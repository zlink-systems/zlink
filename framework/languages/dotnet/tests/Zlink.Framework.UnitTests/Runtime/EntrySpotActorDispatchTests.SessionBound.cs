using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Spots;
using BindingIdentity = Zlink.Framework.Contracts.Actors.ZLinkSessionBindingIdentity;
using WireCodec = Zlink.Framework.Runtime.Service.ZLinkServiceWireCodec;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    public static IEnumerable<object[]> SessionBoundRepetitions() =>
        Enumerable.Range(0, 40).Select(index => new object[] { index });

    public static IEnumerable<object[]> SessionBoundGateCases()
    {
        foreach (
            var mode in new[]
            {
                ZLinkUserSpotExecutionMode.SpotWide,
                ZLinkUserSpotExecutionMode.PerActor,
            }
        )
        foreach (var index in Enumerable.Range(0, 40))
            yield return new object[] { mode, index };
    }

    [Theory]
    [MemberData(nameof(SessionBoundRepetitions))]
    public async Task SessionBound_NotifiesAfterCommit_ForFirstIdempotentAndReplacementBind(
        int iteration
    )
    {
        _ = iteration;
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync();
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        var identities = new List<BindingIdentity>();
        var failures = new List<Exception>();
        runtime.ErrorSink.UnhandledCallbackException += failures.Add;
        actor.Bound = (identity, _) =>
        {
            Assert.True(state.OwnsCurrentDispatch);
            Assert.True(state.TryGetBoundSession(out var current));
            Assert.Equal(current.BindingGeneration, identity.BindingGeneration);
            Assert.Equal(RequestResult.Ok, replies[^1].Result);
            identities.Add(identity);
            return ValueTask.CompletedTask;
        };
        try
        {
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "first", 1, replies);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "first", 1, replies);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "second", 2, replies);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.Empty(failures);
            Assert.Equal(
                new ulong[] { 1, 1, 2 },
                identities.Select(value => value.BindingGeneration)
            );
            Assert.All(
                identities,
                identity =>
                {
                    Assert.Equal(sessionNode, identity.SessionOwnerNodeRid);
                    Assert.Equal(61UL, identity.SessionOwnerNodeGeneration);
                }
            );
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SessionBound_DuplicateDecisionPrecedesNotification_AndRejectionDoesNotNotify(
        bool accept
    )
    {
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync();
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        var calls = new List<string>();
        try
        {
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "first", 1, replies);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            actor.Duplicate = (_, _, _) =>
            {
                calls.Add("decision");
                return ValueTask.FromResult<string?>(accept ? null : "keep-current");
            };
            actor.Bound = (_, _) =>
            {
                calls.Add("bound");
                return ValueTask.CompletedTask;
            };
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "second", 2, replies);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.Equal(accept ? new[] { "decision", "bound" } : new[] { "decision" }, calls);
            Assert.Equal(accept ? RequestResult.Ok : RequestResult.Rejected, replies[^1].Result);
            Assert.True(state.TryGetBoundSession(out var current));
            Assert.Equal(accept ? 2UL : 1UL, current.BindingGeneration);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SessionBound_FailureReportsOnce_WithoutChangingSuccessfulBind(bool canceled)
    {
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync();
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var error = canceled
            ? (Exception)new OperationCanceledException("application-cancel")
            : new InvalidOperationException("bound-failed");
        var failures = new List<Exception>();
        runtime.ErrorSink.UnhandledCallbackException += failures.Add;
        actor.Bound = (_, _) => ValueTask.FromException(error);
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        try
        {
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "first", 1, replies);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.Equal(RequestResult.Ok, Assert.Single(replies).Result);
            Assert.Same(error, Assert.Single(failures));
            Assert.True(state.TryGetBoundSession(out var current));
            Assert.Equal(1UL, current.BindingGeneration);
            var executed = false;
            await state.ExecuteDispatchAsync(
                _ =>
                {
                    executed = true;
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            );
            Assert.True(executed);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [MemberData(nameof(SessionBoundGateCases))]
    public async Task SessionBound_HoldsActorGateAcrossAwait_WhileBindReplyIsAlreadySuccessful(
        ZLinkUserSpotExecutionMode mode,
        int iteration
    )
    {
        _ = iteration;
        Assert.NotNull(typeof(IZLinkActor).GetMethod("OnSessionBoundAsync"));
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync(userSpotMode: mode);
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? binding = null;
        Task? sibling = null;
        try
        {
            var created = await runtime.CreateAsync<JoinTargetSpot>();
            var activation = Assert.Single(
                runtime.GetSpotNodeRuntime("entry").Catalog.Spots,
                value => value.SpotId == created.Spot.SpotId
            );
            await activation.JoinActorAsync(actor, ZLinkMessage.Empty, CancellationToken.None);
            var replies = new List<(RequestResult Result, uint Code, string? Message)>();
            actor.Bound = async (_, _) =>
            {
                Assert.Same(activation, ZLinkSpotAmbientContext.CurrentOrDefault);
                started.TrySetResult();
                await release.Task;
                Assert.True(state.OwnsCurrentDispatch);
                Assert.Same(activation, ZLinkSpotAmbientContext.CurrentOrDefault);
            };
            binding = BindDuplicateSessionAsync(
                runtime,
                actorRef,
                sessionNode,
                "first",
                1,
                replies
            );
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(RequestResult.Ok, Assert.Single(replies).Result);
            var siblingEntered = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            sibling = activation
                ._serial.ExecuteAsync(
                    (_, _) =>
                    {
                        siblingEntered.TrySetResult();
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask();
            if (mode == ZLinkUserSpotExecutionMode.PerActor)
                await siblingEntered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            else
                Assert.False(siblingEntered.Task.IsCompleted);
            release.TrySetResult();
            await Task.WhenAll(binding, sibling).WaitAsync(TimeSpan.FromSeconds(5));
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
        }
        finally
        {
            release.TrySetResult();
            if (binding is not null)
                await binding;
            if (sibling is not null)
                await sibling;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SessionBound_TimeoutReportsOnce_AndRetainsGateUntilLateCallbackFinishes()
    {
        var time = new BoundSessionReplacementLifecycleTests.ReplacementTimeProvider();
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync(callbackTime: time);
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var canceled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var failed = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var failures = new List<Exception>();
        runtime.ErrorSink.UnhandledCallbackException += error =>
        {
            failures.Add(error);
            failed.TrySetResult(error);
        };
        actor.Bound = async (_, ct) =>
        {
            using var registration = ct.Register(() => canceled.TrySetResult());
            started.TrySetResult();
            await release.Task;
            Assert.True(state.OwnsCurrentDispatch);
            throw new InvalidOperationException("late-bound-failure");
        };
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        Task? binding = null;
        try
        {
            binding = BindDuplicateSessionAsync(
                runtime,
                actorRef,
                sessionNode,
                "first",
                1,
                replies
            );
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await time.TimerScheduled.Task.WaitAsync(TimeSpan.FromSeconds(5));
            time.Advance(TimeSpan.FromSeconds(30));
            await canceled.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.IsType<TimeoutException>(await failed.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            await binding.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(RequestResult.Ok, Assert.Single(replies).Result);
            Assert.True(state.TryGetBoundSession(out var current));
            Assert.Equal(1UL, current.BindingGeneration);
            var entered = false;
            var next = state
                .ExecuteDispatchAsync(
                    _ =>
                    {
                        entered = true;
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask();
            Assert.False(entered);
            release.TrySetResult();
            await next.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Single(failures);
        }
        finally
        {
            release.TrySetResult();
            if (binding is not null)
                await binding;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SessionBound_RuntimeShutdownCancelsCallbackWithoutReportingFailure()
    {
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync();
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var canceled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var failures = new List<Exception>();
        runtime.ErrorSink.UnhandledCallbackException += failures.Add;
        actor.Bound = async (_, ct) =>
        {
            using var registration = ct.Register(() => canceled.TrySetResult());
            started.TrySetResult();
            await Task.Delay(Timeout.InfiniteTimeSpan, ct);
        };
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        try
        {
            var binding = BindDuplicateSessionAsync(
                runtime,
                actorRef,
                sessionNode,
                "first",
                1,
                replies
            );
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await runtime.StopAsync(CancellationToken.None);
            await canceled.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await binding.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(RequestResult.Ok, Assert.Single(replies).Result);
            Assert.Empty(failures);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [MemberData(nameof(SessionBoundRepetitions))]
    public async Task SessionBound_QueuedBindsUseCurrentIdentityAtTheirActorTurn(int iteration)
    {
        _ = iteration;
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync();
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        var calls = new List<(ulong Current, ulong Incoming)>();
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? second = null;
        Task? third = null;
        try
        {
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "first", 1, replies);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            actor.Duplicate = async (current, incoming, _) =>
            {
                calls.Add((current.BindingGeneration, incoming.BindingGeneration));
                if (incoming.BindingGeneration == 2)
                {
                    started.TrySetResult();
                    await release.Task;
                }
                return null;
            };
            second = BindDuplicateSessionAsync(
                runtime,
                actorRef,
                sessionNode,
                "second",
                2,
                replies
            );
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            third = BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "third", 3, replies);
            Assert.False(third.IsCompleted);
            release.TrySetResult();
            await Task.WhenAll(second, third).WaitAsync(TimeSpan.FromSeconds(5));
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.Equal(new[] { (1UL, 2UL), (2UL, 3UL) }, calls);
        }
        finally
        {
            release.TrySetResult();
            if (second is not null)
                await second;
            if (third is not null)
                await third;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SessionBound_ActorTerminationCancellationIsNotReported()
    {
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync();
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var failures = new List<Exception>();
        runtime.ErrorSink.UnhandledCallbackException += failures.Add;
        var calls = 0;
        actor.Bound = (_, _) =>
        {
            calls++;
            state.FenceRuntimeGeneration(ZLinkObjectExecutionStatus.Stopped);
            return ValueTask.FromException(new OperationCanceledException("Actor stopped"));
        };
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        try
        {
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "first", 1, replies);
            await runtime.StopAsync(CancellationToken.None);
            Assert.Equal(1, calls);
            Assert.Equal(RequestResult.Ok, Assert.Single(replies).Result);
            Assert.Empty(failures);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SessionBound_EntryActorUsesLifecyclePriorityAndWaitsForItsGate()
    {
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync();
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var order = new List<string>();
        actor.Bound = (_, _) =>
        {
            order.Add("bound");
            return ValueTask.CompletedTask;
        };
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        Task? handler = null;
        Task? application = null;
        Task? binding = null;
        try
        {
            handler = state
                .ExecuteDispatchAsync(
                    async _ =>
                    {
                        entered.TrySetResult();
                        await release.Task;
                    },
                    CancellationToken.None
                )
                .AsTask();
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            application = state
                .ExecuteDispatchAsync(
                    _ =>
                    {
                        order.Add("application");
                        return ValueTask.CompletedTask;
                    },
                    CancellationToken.None
                )
                .AsTask();
            binding = BindDuplicateSessionAsync(
                runtime,
                actorRef,
                sessionNode,
                "first",
                1,
                replies
            );
            Assert.False(binding.IsCompleted);
            Assert.Empty(replies);
            release.TrySetResult();
            await Task.WhenAll(handler, application, binding).WaitAsync(TimeSpan.FromSeconds(5));
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.Equal(new[] { "bound", "application" }, order);
        }
        finally
        {
            release.TrySetResult();
            if (handler is not null)
                await handler;
            if (application is not null)
                await application;
            if (binding is not null)
                await binding;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task SessionBound_DeadlineCancelsSynchronousCallbackBeforeItReturns()
    {
        var time = new BoundSessionReplacementLifecycleTests.ReplacementTimeProvider();
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync(callbackTime: time);
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var canceled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var release = new ManualResetEventSlim();
        var failures = new List<Exception>();
        runtime.ErrorSink.UnhandledCallbackException += failures.Add;
        actor.Bound = (_, ct) =>
        {
            using var registration = ct.Register(() =>
            {
                canceled.TrySetResult();
                release.Set();
            });
            started.TrySetResult();
            if (!release.Wait(TimeSpan.FromSeconds(5)))
                throw new InvalidOperationException(
                    "Synchronous callback did not receive cancellation."
                );
            return ValueTask.CompletedTask;
        };
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        var binding = Task.Run(() =>
            BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "first", 1, replies)
        );
        try
        {
            await started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            time.Advance(TimeSpan.FromSeconds(30));
            await canceled.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await binding.WaitAsync(TimeSpan.FromSeconds(5));
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            Assert.IsType<TimeoutException>(Assert.Single(failures));
            Assert.Equal(RequestResult.Ok, Assert.Single(replies).Result);
        }
        finally
        {
            release.Set();
            await binding;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [MemberData(nameof(SessionBoundRepetitions))]
    public async Task SessionBound_RetirementTransmissionDoesNotBorrowActorExecutionPermission(
        int iteration
    )
    {
        _ = iteration;
        var node = new SessionBoundNotificationNode();
        var (runtime, _, actorRef, original, state, sessionNode) =
            await CreateDuplicateBindFixtureAsync(nodeOverride: node);
        var actor = new SessionBoundActor(original.Context);
        state.BindActorInstance(actor);
        node.ReadActorPermission = () => state.OwnsCurrentDispatch;
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        actor.Bound = async (incoming, _) =>
        {
            if (incoming.BindingGeneration != 2)
                return;
            Assert.True(state.OwnsCurrentDispatch);
            entered.TrySetResult();
            await release.Task;
        };
        var replies = new List<(RequestResult Result, uint Code, string? Message)>();
        Task? binding = null;
        try
        {
            await BindDuplicateSessionAsync(runtime, actorRef, sessionNode, "first", 1, replies);
            await state.ExecuteDispatchAsync(_ => ValueTask.CompletedTask, CancellationToken.None);
            binding = BindDuplicateSessionAsync(
                runtime,
                actorRef,
                sessionNode,
                "second",
                2,
                replies
            );
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(await node.Sent.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            await binding.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(RequestResult.Ok, replies[^1].Result);
        }
        finally
        {
            release.TrySetResult();
            if (binding is not null)
                await binding;
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed class SessionBoundNotificationNode
        : CapturingSpotNode,
            IZLinkBackendBoundSessionReplacementNotifications
    {
        internal Func<bool>? ReadActorPermission;
        internal TaskCompletionSource<bool> Sent { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void SetBoundSessionReplacedNotificationHandler(
            Action<RoutingId, WireCodec.BoundSessionReplacedRecord> handler
        ) { }

        public bool TrySendBoundSessionReplacedNotification(
            RoutingId targetNodeRid,
            WireCodec.BoundSessionReplacedRecord record
        )
        {
            Sent.TrySetResult(ReadActorPermission?.Invoke() ?? false);
            return true;
        }
    }

    private sealed class SessionBoundActor(IZLinkActorContext context)
        : ProbeActor(context.ActorId, context),
            IZLinkActor
    {
        internal Func<BindingIdentity, CancellationToken, ValueTask>? Bound;
        internal Func<
            BindingIdentity,
            BindingIdentity,
            CancellationToken,
            ValueTask<string?>
        >? Duplicate;

        public ValueTask OnSessionBoundAsync(
            BindingIdentity incoming,
            CancellationToken cancellationToken
        ) => Bound?.Invoke(incoming, cancellationToken) ?? ValueTask.CompletedTask;

        public new ValueTask<string?> OnDuplicateBindAsync(
            BindingIdentity current,
            BindingIdentity incoming,
            CancellationToken cancellationToken
        ) =>
            Duplicate?.Invoke(current, incoming, cancellationToken)
            ?? ValueTask.FromResult<string?>(null);
    }
}
