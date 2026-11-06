using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Messaging;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class RequestSubmissionTurnTests
{
    private static readonly TimeSpan Budget = TimeSpan.FromSeconds(10);

#if DEBUG
    [Fact]
    public async Task Queued_selector_carries_only_its_counter_observer_into_nested_turns()
    {
        await using var selector = new ZLinkStateLane();
        await using var operation = new ZLinkStateLane();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var holding = selector.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task.WaitAsync(Budget);
        var events = new ConcurrentQueue<(string Stage, ZLinkStateLane? Lane)>();
        var callerContext = new AsyncLocal<string?> { Value = "caller" };
        ZLinkStateLane.WorkObserver.Value = (stage, lane) => events.Enqueue((stage, lane));
        ValueTask<ValueTask<bool>> queued;
        try
        {
            queued = selector.RunAsync(() =>
            {
                Assert.NotEqual("caller", callerContext.Value);
                return operation.RunAsync(() =>
                {
                    ZLinkStateLane.ObserveWork("submitted");
                    return true;
                });
            });
        }
        finally
        {
            ZLinkStateLane.WorkObserver.Value = null;
            release.SetResult();
            await holding;
        }
        Assert.True(await await queued);
        Assert.Equal(2, events.Count(row => row.Stage == "lane-entry"));
        Assert.Same(operation, Assert.Single(events, row => row.Stage == "submitted").Lane);
        Assert.Equal("caller", callerContext.Value);
    }

    [Fact]
    public async Task Ready_node_request_enters_only_selector_and_operation_turns()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        await using var target = new ZLinkManagedMeshNode(context, "turn-count");
        target.SetRoutingId(RoutingId.From("turn-target"));
        target.SetBind("tcp://127.0.0.1:0");
        target.Start();
        await using var source = new ZLinkManagedMeshNode(context, "turn-count");
        source.SetRoutingId(RoutingId.From("turn-source"));
        source.SetBind("tcp://127.0.0.1:0");
        source.Start();
        source.ConnectPeer(target.Status().LocalEndpoint!, target.RoutingId);
        await ReadyAsync();
        using var cancellation = new CancellationTokenSource();
        using var payload = Message.From("request");
        var entries = new ConcurrentQueue<ZLinkStateLane>();
        var stages = new ConcurrentQueue<(string Name, ZLinkStateLane? Lane)>();
        var admission = new ZLinkRequestAdmission();
        ZLinkStateLane.WorkObserver.Value = (stage, lane) =>
        {
            if (stage == "lane-entry")
                entries.Enqueue(lane!);
            else
                stages.Enqueue((stage, lane));
            if (stage == "registered")
                Assert.Equal(1, source.PendingOperationCount);
        };
        Task<Zlink.Framework.Runtime.Backend.Contracts.ZLinkBackendRouteReceived> request;
        try
        {
            request = source
                .RequestToNodeDirectAsync(
                    target.RoutingId,
                    [payload],
                    SendFlags.None,
                    default,
                    TimeSpan.FromSeconds(10),
                    cancellation.Token,
                    requestAdmission: admission
                )
                .AsTask();
        }
        finally
        {
            ZLinkStateLane.WorkObserver.Value = null;
        }
        using (var guard = new CancellationTokenSource(Budget))
            while (!admission.IsAdmitted)
            {
                guard.Token.ThrowIfCancellationRequested();
                await Task.Yield();
            }
        var beforeSubmit = entries.Count;
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => request);
        Assert.Equal(0, source.PendingOperationCount);
        Assert.True(beforeSubmit == 2, $"pre-submit lane entries: {beforeSubmit}; expected 2");
        Assert.Equal(2, entries.Count);
        Assert.Equal(2, entries.Distinct().Count());
        Assert.DoesNotContain(stages, stage => stage.Name == "intent-observer");
        Assert.Same(
            Assert.Single(stages, stage => stage.Name == "registered").Lane,
            Assert.Single(stages, stage => stage.Name == "native-submit").Lane
        );

        async Task ReadyAsync()
        {
            using var guard = new CancellationTokenSource(TimeSpan.FromSeconds(10));
            while (source.Status().AdmittedPeerCount != 1 || target.Status().AdmittedPeerCount != 1)
            {
                guard.Token.ThrowIfCancellationRequested();
                await Task.Yield();
            }
        }
    }
#endif

    [Theory]
    [InlineData("reply")]
    [InlineData("cancel")]
    [InlineData("close")]
    [InlineData("intent")]
    [InlineData("deadline")]
    public async Task Ready_request_terminal_and_late_reply_have_one_owner(string winner)
    {
        await using var pair = await Pair.CreateAsync();
        using var cancellation = new CancellationTokenSource();
#if DEBUG
        var cleaned = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var lateCleanups = 0;
        var nativeSubmits = 0;
        var laneEntries = new ConcurrentQueue<ZLinkStateLane>();
        ZLinkStateLane.WorkObserver.Value = (stage, lane) =>
        {
            if (stage == "lane-entry")
                laneEntries.Enqueue(lane!);
            if (stage == "native-submit")
                Interlocked.Increment(ref nativeSubmits);
            if (stage == "late-reply-cleanup")
            {
                Interlocked.Increment(ref lateCleanups);
                cleaned.TrySetResult();
            }
        };
#endif
        try
        {
            using var payload = Message.From("request");
            var caller = pair
                .Source.RequestToNodeDirectAsync(
                    pair.Target.RoutingId,
                    [payload],
                    SendFlags.None,
                    default,
                    Budget,
                    cancellation.Token
                )
                .AsTask();
#if DEBUG
            ZLinkStateLane.WorkObserver.Value = null;
#endif
            using var received = new MeshReceiveBatch();
            await pair.ReceiveAsync(received);
            Assert.Equal(1, pair.Source.PendingOperationCount);
            if (winner == "cancel")
                cancellation.Cancel();
            else if (winner == "close")
                pair.Source.CloseSubmissions();
            else if (winner == "intent")
                pair.Source.RemovePeerConnection(pair.Intent);
            else if (winner == "deadline")
            {
                pair.Clock.Timestamp = Budget.Ticks - 1;
                pair.Source.ExpireOperations();
                Assert.False(caller.IsCompleted);
                Assert.Equal(1, pair.Source.PendingOperationCount);
                pair.Clock.Timestamp++;
                pair.Source.ExpireOperations();
            }
            Exception? terminal = null;
            if (winner != "reply")
            {
                terminal = await Record.ExceptionAsync(() => caller.WaitAsync(Budget));
                Assert.NotNull(terminal);
                Assert.Equal(0, pair.Source.PendingOperationCount);
                if (winner == "cancel")
                    Assert.Equal(
                        cancellation.Token,
                        Assert
                            .IsAssignableFrom<OperationCanceledException>(terminal)
                            .CancellationToken
                    );
                else if (winner == "intent")
                    Assert.Equal(
                        ZLinkFrameworkErrorKind.Unavailable,
                        Assert.IsType<ZLinkFrameworkException>(terminal).Kind
                    );
                else if (winner == "deadline")
                    Assert.Equal(
                        ZlinkRequestException.ErrorCode.TimedOut,
                        Assert.IsAssignableFrom<ZlinkRequestException>(terminal).Result
                    );
                else
                    Assert.Equal(
                        RequestResult.Terminated,
                        Assert.IsType<ZLinkRequestTerminalException>(terminal).Result
                    );
            }
            using var reply = Message.From("reply");
            var submitted = received[0].Reply([reply]);
            if (winner == "reply")
            {
                Assert.Equal(SubmitResult.Ok, submitted);
                using var result = await caller.WaitAsync(Budget);
                Assert.Equal(
                    "reply",
                    System.Text.Encoding.UTF8.GetString(
                        result.ApplicationPayloadView!.Value.GetSpan(0)
                    )
                );
#if DEBUG
                Assert.Equal(2, laneEntries.Count);
#endif
                cancellation.Cancel();
                pair.Source.CloseSubmissions();
                Assert.True(caller.IsCompletedSuccessfully);
            }
            else
            {
                Assert.Same(terminal, await Record.ExceptionAsync(() => caller));
#if DEBUG
                // Cancellation and owner expiration retire the logical entry while
                // leaving the admitted native request available for its late reply.
                if (winner is "cancel" or "deadline")
                {
                    await cleaned.Task.WaitAsync(Budget);
                    Assert.Equal(1, lateCleanups);
                }
#endif
            }
            Assert.Equal(0, pair.Source.PendingOperationCount);
            Assert.Equal(0, pair.Clock.Timers);
#if DEBUG
            Assert.Equal(1, nativeSubmits);
#endif
        }
        finally
        {
#if DEBUG
            ZLinkStateLane.WorkObserver.Value = null;
#endif
        }
    }

    [Fact]
    public async Task Not_ready_request_keeps_its_entry_and_deadline_when_route_becomes_ready()
    {
        await using var pair = await Pair.CreateAsync(connect: false);
        using var payload = Message.From("waiting");
        var request = pair
            .Source.RequestToNodeDirectAsync(
                pair.Target.RoutingId,
                [payload],
                SendFlags.None,
                default,
                Budget,
                CancellationToken.None
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        Assert.Equal(1, pair.Source.PendingOperationCount);
        pair.Clock.Timestamp = TimeSpan.FromSeconds(3).Ticks;
        pair.Connect();
        await pair.ReadyAsync();
        using var received = new MeshReceiveBatch();
        await pair.ReceiveAsync(received);
        pair.Clock.Timestamp = Budget.Ticks - 1;
        pair.Source.ExpireOperations();
        Assert.False(request.IsCompleted);
        pair.Clock.Timestamp++;
        pair.Source.ExpireOperations();
        var failure = await Assert.ThrowsAsync<ZlinkRequestException>(() => request);
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, failure.Result);
        Assert.Equal(0, pair.Source.PendingOperationCount);
        Assert.Equal(0, pair.Clock.Timers);
    }

    [Fact]
    public async Task Operation_owner_preserves_UTC_high_water_and_synchronous_reply_registration()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        var clock = new ControlledClock();
        await using var node = new ZLinkManagedMeshNode(
            context,
            "owner-clock",
            deadlineTimeProvider: clock
        );
#if DEBUG
        var registrations = 0;
        ZLinkStateLane.WorkObserver.Value = (stage, lane) =>
        {
            if (stage == "registered")
            {
                Assert.NotNull(lane);
                registrations++;
            }
        };
#endif
        var deadlines = new List<ulong>();
        var epoch = (ulong)clock.Utc.ToUnixTimeMilliseconds();
        for (var index = 0; index < 3; index++)
        {
            if (index == 1)
            {
                clock.Utc -= TimeSpan.FromDays(1);
                clock.Timestamp += TimeSpan.FromSeconds(1).Ticks;
            }
            if (index == 2)
                clock.Utc = DateTimeOffset.FromUnixTimeMilliseconds((long)epoch).AddSeconds(20);
            await node.RunDeadlineRequestAsync(
                Budget,
                CancellationToken.None,
                completion => completion.Parts.Count,
                produce: null,
                submit: pending =>
                {
                    Assert.Equal(1, node.PendingOperationCount);
                    deadlines.Add(pending.DeadlineUnixMs);
                    _ = node.ObserveNativeDeadlineReplyAsync(
                        (ZLinkManagedMeshNode.PendingOperation<int>)pending,
                        Task.FromResult<IReadOnlyList<Message>>([]),
                        completion => completion.Parts.Count
                    );
                }
            );
            Assert.Equal(0, node.PendingOperationCount);
        }
#if DEBUG
        ZLinkStateLane.WorkObserver.Value = null;
        Assert.Equal(3, registrations);
#endif
        Assert.Equal(new[] { epoch + 10_000, epoch + 11_000, epoch + 30_000 }, deadlines);
        Assert.Equal(0, clock.Timers);
    }

    [Fact]
    public async Task Intent_retirement_follows_queued_registration_without_a_lifetime_observer()
    {
        await using var pair = await Pair.CreateAsync();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var occupied = pair.OperationLane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task;
        using var payload = Message.From("queued-registration");
        var request = pair
            .Source.RequestToNodeDirectAsync(
                pair.Target.RoutingId,
                [payload],
                SendFlags.None,
                default,
                Budget,
                CancellationToken.None
            )
            .AsTask();
        Assert.False(request.IsCompleted);
        var removal = Task.Run(() => pair.Source.RemovePeerConnection(pair.Intent));
        try
        {
            using var guard = new CancellationTokenSource(Budget);
            while (pair.OperationLane.QueuedWorkCount < 2)
            {
                guard.Token.ThrowIfCancellationRequested();
                await Task.Yield();
            }
        }
        finally
        {
            release.SetResult();
            await occupied;
        }
        await removal.WaitAsync(Budget);
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => request);
        Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, error.Kind);
        Assert.Equal(0, pair.Source.PendingOperationCount);
    }

    private sealed class ControlledClock : TimeProvider
    {
        internal long Timestamp;
        internal DateTimeOffset Utc = DateTimeOffset.FromUnixTimeMilliseconds(1_700_000_000_000);
        internal int Timers;
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        public override long GetTimestamp() => Volatile.Read(ref Timestamp);

        public override DateTimeOffset GetUtcNow() => Utc;

        public override ITimer CreateTimer(
            TimerCallback callback,
            object? state,
            TimeSpan dueTime,
            TimeSpan period
        )
        {
            Interlocked.Increment(ref Timers);
            throw new InvalidOperationException("A request must use the owner's deadline index.");
        }
    }

    private sealed class Pair : IAsyncDisposable
    {
        private readonly IContext _context = Systems.Zlink.Zlink.CreateContext();
        internal readonly ControlledClock Clock = new();
        internal readonly ZLinkStateLane OperationLane = new();
        internal readonly ZLinkManagedMeshNode Source;
        internal readonly ZLinkManagedMeshNode Target;
        internal ulong Intent;

        private Pair()
        {
            Source = new ZLinkManagedMeshNode(
                _context,
                "turn-contract",
                deadlineTimeProvider: Clock,
                operationLane: OperationLane
            );
            Target = new ZLinkManagedMeshNode(_context, "turn-contract");
            Source.SetRoutingId(RoutingId.From("turn-contract-source"));
            Target.SetRoutingId(RoutingId.From("turn-contract-target"));
            Source.SetBind("tcp://127.0.0.1:0");
            Target.SetBind("tcp://127.0.0.1:0");
            Target.Start();
            Source.Start();
        }

        internal static async Task<Pair> CreateAsync(bool connect = true)
        {
            var pair = new Pair();
            if (connect)
            {
                pair.Connect();
                await pair.ReadyAsync();
            }
            return pair;
        }

        internal void Connect() =>
            Intent = Source.ConnectPeer(Target.Status().LocalEndpoint!, Target.RoutingId);

        internal Task ReadyAsync() =>
            UntilAsync(() =>
                Source.Status().AdmittedPeerCount == 1 && Target.Status().AdmittedPeerCount == 1
            );

        internal Task ReceiveAsync(MeshReceiveBatch batch) =>
            UntilAsync(() =>
            {
                using var ready = new MeshReadyBatch();
                Target.DrainReady(MeshReadyDomains.Application, ready, RecvFlags.DontWait);
                if (ready.Count == 0)
                    return false;
                using var claim = ready.TakeClaim(0);
                Assert.True(claim.Receive(batch, RecvFlags.DontWait));
                Assert.Equal(MeshRecordKind.NodeRequest, batch[0].Kind);
                return true;
            });

        private static async Task UntilAsync(Func<bool> condition)
        {
            using var guard = new CancellationTokenSource(Budget);
            while (!condition())
            {
                guard.Token.ThrowIfCancellationRequested();
                await Task.Yield();
            }
        }

        public async ValueTask DisposeAsync()
        {
            await Source.DisposeAsync();
            await Target.DisposeAsync();
            _context.Dispose();
        }
    }
}
