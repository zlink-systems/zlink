using System.Diagnostics;
using System.Runtime.CompilerServices;
using Systems.Zlink.Framework.Runtime.Protocol;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

// Spec 30 §14 step 1 (D-097): the mesh node follows the host's shutdown
// admission seal. After the seal it neither starts nor accepts peer admission,
// already-admitted peers still receive the Draining Update, and peer loss
// (transport disconnect, liveness expiry, failed control send) never undoes
// the published Draining. Service-wire §5: a repeated Hello/Admit that carries
// the current descriptor is idempotent - it completes the admission again
// without re-admitting the peer or resetting its descriptor/liveness epoch.
public sealed class MeshNodeShutdownSealTests(Xunit.Abstractions.ITestOutputHelper output)
{
    private const string MeshName = "orders";
    private const string EphemeralTcpEndpoint = "tcp://127.0.0.1:0";

    [Fact]
    public async Task OperationDeadline_ExpiresAtOccupiedTicksWithoutExpiringEarly()
    {
        var clock = new OperationDeadlineClock();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: clock
        );
        const System.Reflection.BindingFlags flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        var begin = typeof(ZLinkManagedMeshNode)
            .GetMethods(flags)
            .Single(method =>
                method.Name == "TryBeginOperation" && method.GetParameters().Length == 3
            );
        var expire = typeof(ZLinkManagedMeshNode).GetMethod("ExpireOperations", flags)!;
        var operations = typeof(ZLinkManagedMeshNode)
            .GetField("_operations", flags)!
            .GetValue(node)!;
        var count = operations.GetType().GetProperty("Count")!;
        Assert.True(
            (bool)
                begin.Invoke(
                    node,
                    [MeshOperationKind.NodeRequest, TimeSpan.FromMilliseconds(20), null]
                )!
        );
        Assert.True(
            (bool)
                begin.Invoke(
                    node,
                    [
                        MeshOperationKind.NodeRequest,
                        TimeSpan.FromMilliseconds(10) + TimeSpan.FromTicks(1),
                        null,
                    ]
                )!
        );

        // Approved §10.1 schedules the earliest absolute deadline in L+R,
        // rather than a rounds-only slot. It must never expire before that value.
        Assert.Equal(
            TimeSpan.FromMilliseconds(10) + TimeSpan.FromTicks(1),
            expire.Invoke(node, null)
        );
        clock.Timestamp = 9 * TimeSpan.TicksPerMillisecond;
        Assert.Equal(
            TimeSpan.FromMilliseconds(1) + TimeSpan.FromTicks(1),
            expire.Invoke(node, null)
        );
        Assert.Equal(2, count.GetValue(operations));
        clock.Timestamp = 10 * TimeSpan.TicksPerMillisecond;
        Assert.Equal(TimeSpan.FromTicks(1), expire.Invoke(node, null));
        Assert.Equal(2, count.GetValue(operations));
        clock.Timestamp = 11 * TimeSpan.TicksPerMillisecond;
        Assert.Equal(TimeSpan.FromMilliseconds(9), expire.Invoke(node, null));
        Assert.Equal(1, count.GetValue(operations));
        clock.Timestamp = 20 * TimeSpan.TicksPerMillisecond;
        Assert.Equal(Timeout.InfiniteTimeSpan, expire.Invoke(node, null));
        Assert.Equal(0, count.GetValue(operations));
        Assert.Equal(Timeout.InfiniteTimeSpan, expire.Invoke(node, null));
        Assert.Equal(0, count.GetValue(operations));
    }

    [Fact]
    public async Task OperationDeadline_ReplyAndTimeoutRaceHasOneAtomicTakeOwner()
    {
        var clock = new OperationDeadlineClock();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: clock
        );
        var deadline = OperationDeadlineAccess.For(node);

        // The reply takes the operation first: its deadline leaves the index, the
        // expiry turn finds nothing. I0 waits for readiness without a periodic deadline.
        var replied = deadline.Begin(TimeSpan.FromMilliseconds(10));
        clock.Timestamp = 5 * TimeSpan.TicksPerMillisecond;
        Assert.Equal(TimeSpan.FromMilliseconds(5), deadline.Expire());
        Assert.True(deadline.TakeForReply(replied));
        clock.Timestamp = 10 * TimeSpan.TicksPerMillisecond;
        Assert.Equal(Timeout.InfiniteTimeSpan, deadline.Expire());
        Assert.False(deadline.Token(replied).IsCancellationRequested);
        Assert.Equal(0, deadline.Count);

        // The deadline takes the operation first: it completes as timed out
        // and the late reply finds no owner.
        var timedOut = deadline.Begin(TimeSpan.FromMilliseconds(10));
        Assert.Equal(TimeSpan.FromMilliseconds(10), deadline.Expire());
        clock.Timestamp = 20 * TimeSpan.TicksPerMillisecond;
        Assert.Equal(Timeout.InfiniteTimeSpan, deadline.Expire());
        Assert.True(deadline.Token(timedOut).IsCancellationRequested);
        Assert.False(deadline.TakeForReply(timedOut));
        Assert.Equal(0, deadline.Count);
    }

    [Fact]
    public async Task OperationDeadline_ExpiryAtTheBudgetReturnsAZeroBoundForTheRestOfTheSlot()
    {
        var clock = new OperationDeadlineClock();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: clock
        );
        var deadline = OperationDeadlineAccess.For(node);
        var first = deadline.Begin(TimeSpan.FromMilliseconds(1));
        deadline.Begin(TimeSpan.FromMilliseconds(1));
        deadline.Begin(TimeSpan.FromMilliseconds(30));
        // The first terminal spends the whole management budget on the injected clock.
        using var slowTerminal = deadline
            .Token(first)
            .Register(() =>
                clock.Timestamp +=
                    Zlink.Framework.Runtime.Dispatch.ZLinkReceiveBatchBudget.MaximumMilliseconds
                    * TimeSpan.TicksPerMillisecond
            );
        clock.Timestamp = TimeSpan.TicksPerMillisecond;

        // The rotation stops after one expiry; the bound read in that turn is
        // zero because the rest of the slot is already due, so the receive owner
        // does not block before it resumes the slot.
        Assert.Equal(TimeSpan.Zero, deadline.Expire());
        Assert.Equal(2, deadline.Count);
        Assert.Equal(TimeSpan.FromMilliseconds(28), deadline.Expire());
        Assert.Equal(1, deadline.Count);
    }

    [Fact]
    public async Task OperationDeadline_CloseDrainCancelsPendingOperationsAndEmptiesTheIndex()
    {
        var clock = new OperationDeadlineClock();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var node = new ZLinkManagedMeshNode(context, MeshName, deadlineTimeProvider: clock);
        var deadline = OperationDeadlineAccess.For(node);
        var pending = new[]
        {
            deadline.Begin(TimeSpan.FromMilliseconds(5)),
            deadline.Begin(TimeSpan.FromMilliseconds(600)),
        };
        Assert.Equal(TimeSpan.FromMilliseconds(5), deadline.Expire());

        await node.DisposeAsync();

        // Close drain owns these terminals. A later expiry turn finds no
        // deadline without an owner and reports the idle bound.
        Assert.All(
            pending,
            operation => Assert.True(deadline.Token(operation).IsCancellationRequested)
        );
        Assert.Equal(0, deadline.Count);
        clock.Timestamp = 600 * TimeSpan.TicksPerMillisecond;
        Assert.Equal(Timeout.InfiniteTimeSpan, deadline.Expire());
    }

    [Fact]
    public async Task OperationDeadline_CompletionDeliveryConsumesTheNextWaitBound()
    {
        var clock = new OperationDeadlineClock();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: clock
        );
        var deadline = OperationDeadlineAccess.For(node);
        var first = deadline.Begin(TimeSpan.FromMilliseconds(1));
        deadline.Begin(TimeSpan.FromMilliseconds(2));
        using var terminal = deadline
            .Token(first)
            .Register(() =>
                clock.Timestamp +=
                    Zlink.Framework.Runtime.Dispatch.ZLinkReceiveBatchBudget.MaximumMilliseconds
                    * TimeSpan.TicksPerMillisecond
            );
        clock.Timestamp = TimeSpan.TicksPerMillisecond;

        // Completion delivery exhausts the rotation's budget and reaches the
        // next occupied tick. The next rotation must not wait the duration
        // that remained before that completion was delivered.
        Assert.Equal(TimeSpan.Zero, deadline.Expire());
        Assert.Equal(1, deadline.Count);
        Assert.Equal(Timeout.InfiniteTimeSpan, deadline.Expire());
        Assert.Equal(0, deadline.Count);
    }

    [Fact]
    public async Task RequestTerminal_TakesWithoutEnteringTheOperationLane()
    {
        await using var lane = new ZLinkStateLane();
        await using var fixture = new NativeDeadlineRequest(operationLane: lane);
        await fixture.Ready.WaitAsync(TimeSpan.FromSeconds(5));
        using var cancellation = new CancellationTokenSource();
        var pending = fixture.Request(TimeSpan.FromMinutes(1), cancellation.Token).AsTask();
        using var received = Received.Create();
        Assert.True(fixture.Server.Recv(received));
        Assert.Equal(1, fixture.Node.PendingOperationCount);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var occupied = lane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task;
        try
        {
            cancellation.Cancel();
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                pending.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(0, fixture.Node.PendingOperationCount);
            Assert.False(occupied.IsCompleted);
        }
        finally
        {
            release.SetResult();
            await occupied;
        }
    }

    [Fact]
    public async Task RequestDeadline_IncludesTimeWaitingForTheOperationOwner()
    {
        var clock = new OperationDeadlineClock();
        await using var lane = new ZLinkStateLane();
        await using var fixture = new NativeDeadlineRequest(clock, lane);
        await fixture.Ready.WaitAsync(TimeSpan.FromSeconds(5));
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var occupied = lane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task;
        var sampled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var requestThread = 0;
        var reads = 0;
        clock.TimestampRead = () =>
        {
            if (
                Environment.CurrentManagedThreadId == Volatile.Read(ref requestThread)
                && ++reads == 2
            )
                sampled.TrySetResult();
        };
        var pending = Task.Run(async () =>
        {
            Volatile.Write(ref requestThread, Environment.CurrentManagedThreadId);
            return await fixture.Request(TimeSpan.FromMilliseconds(10));
        });
        try
        {
            await sampled.Task.WaitAsync(TimeSpan.FromSeconds(5));
            clock.Timestamp = 20 * TimeSpan.TicksPerMillisecond;
        }
        finally
        {
            clock.TimestampRead = null;
            release.SetResult();
            await occupied;
        }
        fixture.Node.SignalReceiveOwner();
        var error = await Assert.ThrowsAsync<ZlinkRequestException>(() =>
            pending.WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, error.Result);
        Assert.Equal(0, fixture.Node.PendingOperationCount);
    }

    [Fact]
    public void RequestIndex_ThousandEntriesUseNoRequestTimersAndReleaseCompletedReferences()
    {
        var rotation = ZLinkRequestDeadlineIndex.DefaultRotation.Ticks;
        var index = new ZLinkDeadlineIndex<DeadlineEntry>(rotation);
        var before = Timer.ActiveCount;
        var references = RegisterCompletedEntries(index);
        Assert.True(Timer.ActiveCount <= before);
        var visited = 0;
        index.Visit(
            rotation,
            (_, _) =>
            {
                visited++;
                return false;
            },
            static () => false,
            () => rotation
        );
        Assert.Equal(1000, visited);
        Assert.Null(index.Head);
        Assert.Equal(long.MaxValue, index.NextVisit);
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        Assert.All(references, reference => Assert.False(reference.TryGetTarget(out _)));
        var next = new DeadlineEntry(rotation + 1);
        index.Register(next, rotation);
        visited = 0;
        index.Visit(
            rotation + 1,
            (_, _) =>
            {
                visited++;
                return false;
            },
            static () => false,
            () => rotation + 1
        );
        Assert.Equal(1, visited);
        Assert.Null(index.Head);
        output.WriteLine(
            "request_count=1000 request_timers=0 completed_references=0 next_visit_completed_entries=0"
        );
    }

    [System.Runtime.CompilerServices.MethodImpl(
        System.Runtime.CompilerServices.MethodImplOptions.NoInlining
    )]
    private static WeakReference<DeadlineEntry>[] RegisterCompletedEntries(
        ZLinkDeadlineIndex<DeadlineEntry> index
    )
    {
        var references = new WeakReference<DeadlineEntry>[1000];
        for (var i = 0; i < references.Length; i++)
        {
            var entry = new DeadlineEntry(TimeSpan.FromMinutes(1).Ticks);
            index.Register(entry, 0);
            references[i] = new(entry);
        }
        return references;
    }

    [Fact]
    public async Task RequestIndex_DoesNotEnterOwnerBeforeNextVisit()
    {
        await using var lane = new ZLinkStateLane();
        await using var fixture = new NativeDeadlineRequest(operationLane: lane);
        await fixture.Ready.WaitAsync(TimeSpan.FromSeconds(5));
        var admission = new ZLinkRequestAdmission();
        var pending = fixture
            .Request(TimeSpan.FromMinutes(1), requestAdmission: admission)
            .AsTask();
        output.WriteLine($"before_receive admitted={admission.IsAdmitted} status={pending.Status}");
        Assert.False(
            pending.IsCompleted,
            $"request status={pending.Status} failure={pending.Exception}"
        );
        using var received = Received.Create();
        try
        {
            Assert.True(fixture.Server.Recv(received));
        }
        catch
        {
            output.WriteLine(
                $"receive_failed admitted={admission.IsAdmitted} status={pending.Status} failure={pending.Exception}"
            );
            throw;
        }
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var occupied = lane.RunAsync(async () =>
        {
            entered.SetResult();
            await release.Task;
        });
        await entered.Task;
        try
        {
            var firstWait = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            fixture.Poller.BeforeWait = () => firstWait.TrySetResult();
            fixture.Node.SignalReceiveOwner();
            await firstWait.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var nextWait = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            fixture.Poller.BeforeWait = () => nextWait.TrySetResult();
            fixture.Node.SignalReceiveOwner();
            await nextWait.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(1, fixture.Node.PendingOperationCount);
            Assert.False(pending.IsCompleted);
            Assert.False(occupied.IsCompleted);
            using var reply = Message.From("reply-while-operation-owner-is-busy");
            received.Reply().Message(reply).Submit();
            var parts = await pending.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(
                "reply-while-operation-owner-is-busy",
                System.Text.Encoding.UTF8.GetString(Assert.Single(parts).AsReadOnlySpan())
            );
            ZLinkMessageParts.DisposeAll(parts);
            Assert.Equal(0, fixture.Node.PendingOperationCount);
            Assert.False(occupied.IsCompleted);
        }
        finally
        {
            fixture.Poller.BeforeWait = null;
            release.SetResult();
            await occupied;
        }
    }

    [Fact]
    public async Task ActorCallerCancellation_RemovesTheFrameworkEntryImmediately()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        await using var wrapper =
            new Zlink.Framework.Runtime.Backend.DotNet.Wrappers.ZLinkBackendSpotNodeWrapper(node);
        using var canceled = new CancellationTokenSource();
        canceled.Cancel();
        var part = Message.From("request");
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            wrapper
                .RequestToActorAsync(
                    new Zlink.Framework.Runtime.Backend.Contracts.ZLinkBackendActorRef(
                        RoutingId.From("target"),
                        "actor",
                        1
                    ),
                    [part],
                    TimeSpan.FromMinutes(1),
                    canceled.Token,
                    static _ => true,
                    static failure => failure
                )
                .AsTask()
        );
        Assert.Equal(0, node.PendingOperationCount);
    }

    [Fact]
    public async Task RequestIndex_ThousandExpiriesUseOneManagementTurn()
    {
        const int requestCount = 1000;
        var clock = new OperationDeadlineClock();
        await using var lane = new ZLinkStateLane();
        await using var fixture = new NativeDeadlineRequest(clock, lane);
        await fixture.Ready.WaitAsync(TimeSpan.FromSeconds(5));
        var timeout = TimeSpan.FromMinutes(1);
        var pending = new Task<IReadOnlyList<Message>>[requestCount];
        for (var i = 0; i < pending.Length; i++)
        {
            pending[i] = fixture.Request(timeout).AsTask();
            Assert.False(
                pending[i].IsCompleted,
                $"request={i} status={pending[i].Status} failure={pending[i].Exception}"
            );
        }
        Assert.Equal(requestCount, fixture.Node.PendingOperationCount);
        Assert.All(
            pending,
            request =>
                Assert.False(
                    request.IsCompleted,
                    $"status={request.Status} failure={request.Exception}"
                )
        );
        Task<int>? interposedRead = null;
        using var queued = new ManualResetEventSlim();
        var observedFirstExpiry = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        clock.TimestampRead = () =>
        {
            if (fixture.Node.PendingOperationCount != requestCount - 1)
                return;
            clock.TimestampRead = null;
            using (ExecutionContext.SuppressFlow())
                interposedRead = Task.Run(async () =>
                {
                    var read = lane.RunAsync(() => fixture.Node.PendingOperationCount);
                    queued.Set();
                    return await read;
                });
            observedFirstExpiry.TrySetResult(queued.Wait(TimeSpan.FromSeconds(5)));
        };
        try
        {
            clock.Timestamp = timeout.Ticks;
            fixture.Node.SignalReceiveOwner();
            Assert.True(await observedFirstExpiry.Task.WaitAsync(TimeSpan.FromSeconds(5)));
            Assert.NotNull(interposedRead);
            Assert.Equal(0, await interposedRead.WaitAsync(TimeSpan.FromSeconds(5)));
            foreach (var request in pending)
            {
                var failure = await Assert.ThrowsAsync<ZlinkRequestException>(() =>
                    request.WaitAsync(TimeSpan.FromSeconds(5))
                );
                Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, failure.Result);
            }
            Assert.Equal(0, fixture.Node.PendingOperationCount);
            output.WriteLine(
                "request_count=1000 first_expiry_pending=999 interposed_owner_read=0 expiry_management_turns=1"
            );
        }
        finally
        {
            clock.TimestampRead = null;
        }
    }

    [Fact]
    public async Task RequestShutdown_CompletesManagedCallersBeforeDispatcherDrain()
    {
        await using var fixture = new NativeDeadlineRequest();
        await fixture.Ready.WaitAsync(TimeSpan.FromSeconds(5));
        var pending = fixture.Request(TimeSpan.FromMinutes(1)).AsTask();
        using var received = Received.Create();
        Assert.True(fixture.Server.Recv(received));
        Assert.Equal(1, fixture.Node.PendingOperationCount);
        Assert.False(fixture.Node.Completions.CompletionDrained.IsCompleted);
        fixture.Node.CloseSubmissions();
        var error = await Assert.ThrowsAsync<ZLinkRequestTerminalException>(() =>
            pending.WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.Equal(RequestResult.Terminated, error.Result);
        await fixture.Node.Completions.CompletionDrained.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, fixture.Node.PendingOperationCount);
    }

    [Fact]
    public void RequestIndex_UsesAbsoluteWindowAndDoesNotVisitRoundsOnlySlots()
    {
        var index = new ZLinkDeadlineIndex<DeadlineEntry>(
            ZLinkRequestDeadlineIndex.DefaultRotation.Ticks
        );
        var first = new DeadlineEntry(TimeSpan.FromMilliseconds(1000).Ticks);
        index.Register(first, 0);
        Assert.Equal(TimeSpan.FromMilliseconds(256).Ticks, index.NextVisit);
        var now = TimeSpan.FromMilliseconds(232).Ticks;
        Assert.Equal(TimeSpan.FromMilliseconds(24).Ticks, index.NextVisit - now);
        now = TimeSpan.FromMilliseconds(256).Ticks;
        index.Visit(
            now,
            static (entry, at) => entry.AbsoluteDeadline > at,
            static () => false,
            () => now
        );
        var second = new DeadlineEntry(TimeSpan.FromMilliseconds(756).Ticks);
        index.Register(second, now);
        Assert.Equal(TimeSpan.FromMilliseconds(256).Ticks, index.NextVisit - now);
        index.Register(new DeadlineEntry(TimeSpan.FromMilliseconds(276).Ticks), now);
        Assert.Equal(TimeSpan.FromMilliseconds(20).Ticks, index.NextVisit - now);
        now = TimeSpan.FromMilliseconds(276).Ticks;
        index.Visit(
            now,
            static (entry, at) => entry.AbsoluteDeadline > at,
            static () => false,
            () => now
        );
        Assert.Equal(TimeSpan.FromMilliseconds(256).Ticks, index.NextVisit - now);
        Assert.Same(first, index.Head);
        Assert.Same(second, first.DeadlineNext);
        Assert.Null(second.DeadlineNext);
    }

    [Fact]
    public void RequestIndex_ThousandRegistrationsDoNotEnumerateTheIndex()
    {
        var index = new ZLinkDeadlineIndex<DeadlineEntry>(
            ZLinkRequestDeadlineIndex.DefaultRotation.Ticks
        );
        var entries = new DeadlineEntry[1000];
        for (var i = 0; i < entries.Length; i++)
        {
            entries[i] = new DeadlineEntry(TimeSpan.FromMinutes(1).Ticks);
            index.Register(entries[i], 0);
        }
        Assert.True(entries.Sum(entry => entry.LinkReads) <= 2 * entries.Length);
        // Control input: traversing the old entries for each registration must exceed the linear bound.
        for (var i = 0; i < entries.Length; i++)
        for (var at = 0; at < i; at++)
            _ = entries[at].DeadlineNext;
        Assert.True(entries.Sum(entry => entry.LinkReads) > 2 * entries.Length);
        output.WriteLine(
            "concurrent_pending=1000 registration_index_traversals=0 traversal_control_rejected=true"
        );
    }

    [Fact]
    public void RequestIndex_ReplyRetainsReservationUntilTheManagementVisit()
    {
        var index = new ZLinkDeadlineIndex<DeadlineEntry>(
            ZLinkRequestDeadlineIndex.DefaultRotation.Ticks
        );
        var first = new DeadlineEntry(TimeSpan.FromMilliseconds(10).Ticks);
        index.Register(first, 0);
        index.Remove(first);
        Assert.Null(index.Head);
        Assert.Equal(TimeSpan.FromMilliseconds(10).Ticks, index.NextVisit);
        index.Register(new DeadlineEntry(TimeSpan.FromMilliseconds(1000).Ticks), 0);
        Assert.Equal(TimeSpan.FromMilliseconds(10).Ticks, index.NextVisit);
        var now = TimeSpan.FromMilliseconds(10).Ticks;
        index.Visit(
            now,
            static (entry, at) => entry.AbsoluteDeadline > at,
            static () => false,
            () => now
        );
        Assert.Equal(ZLinkRequestDeadlineIndex.DefaultRotation.Ticks, index.NextVisit - now);
    }

    [Fact]
    public void RequestIndex_EmptyAtVisitStopsAndRegistrationStartsANewWindow()
    {
        var index = new ZLinkDeadlineIndex<DeadlineEntry>(
            ZLinkRequestDeadlineIndex.DefaultRotation.Ticks
        );
        index.Register(new DeadlineEntry(TimeSpan.FromMilliseconds(257).Ticks), 0);
        var now = TimeSpan.FromMilliseconds(256).Ticks;
        index.Visit(
            now,
            static (entry, at) => entry.AbsoluteDeadline > at,
            static () => false,
            () => now
        );
        Assert.Equal(TimeSpan.FromMilliseconds(1).Ticks, index.NextVisit - now);
        now = TimeSpan.FromMilliseconds(257).Ticks;
        index.Visit(
            now,
            static (entry, at) => entry.AbsoluteDeadline > at,
            static () => false,
            () => now
        );
        Assert.Equal(long.MaxValue, index.NextVisit);
        now = TimeSpan.FromMilliseconds(10_000).Ticks;
        index.Visit(
            now,
            static (entry, at) => entry.AbsoluteDeadline > at,
            static () => false,
            () => now
        );
        Assert.Equal(long.MaxValue, index.NextVisit);
        index.Register(new DeadlineEntry(now + TimeSpan.FromMilliseconds(50).Ticks), now);
        Assert.Equal(TimeSpan.FromMilliseconds(50).Ticks, index.NextVisit - now);
    }

    private sealed class DeadlineEntry(long deadline) : IZLinkDeadlineEntry<DeadlineEntry>
    {
        private DeadlineEntry? _next;
        private DeadlineEntry? _previous;
        internal int LinkReads;
        public long AbsoluteDeadline => deadline;
        public DeadlineEntry? DeadlineNext
        {
            get
            {
                LinkReads++;
                return _next;
            }
            set => _next = value;
        }
        public DeadlineEntry? DeadlinePrevious
        {
            get
            {
                LinkReads++;
                return _previous;
            }
            set => _previous = value;
        }
    }

    private sealed class NativeDeadlineRequest : IAsyncDisposable
    {
        private readonly IContext _context = Systems.Zlink.Zlink.CreateContext();
        internal readonly IRouterSocket Server;
        internal readonly ZLinkManagedMeshNode Node;
        internal readonly MeshReceiveWakeTests.ControlledReceivePoller Poller;
        internal readonly Task Ready;
        private readonly RoutingId _target = RoutingId.From("deadline-target");

        internal NativeDeadlineRequest(
            TimeProvider? clock = null,
            ZLinkStateLane? operationLane = null
        )
        {
            Server = _context.CreateRouterSocket();
            Server.SetRoutingId(_target);
            Server.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
            Poller = new MeshReceiveWakeTests.ControlledReceivePoller { CaptureIdleWait = true };
            Poller.CanObserveIdleWait = () => Poller.MonitorRegistered.Task.IsCompleted;
            var endpoint = $"inproc://deadline-request-{Guid.NewGuid():N}";
            Node = new ZLinkManagedMeshNode(
                _context,
                MeshName,
                deadlineTimeProvider: clock ?? new OperationDeadlineClock(),
                operationLane: operationLane,
                receivePollerFactory: () => Poller
            );
            Node.SetRoutingId(RoutingId.From("deadline-source"));
            Node.SetBind(endpoint);
            Node.Start();
            Ready = Poller.MonitorRegistered.Task;
            Server.Connect(endpoint);
            using var poller = Systems.Zlink.Zlink.CreatePoller();
            poller.Add(Server, PollEventFlags.PollRoute | PollEventFlags.PollCompletion, 1);
            var events = new PollEvent[1];
            while (Server.RoutesSnapshot().Count == 0)
                Assert.True(poller.Wait(events, TimeSpan.FromSeconds(5)) > 0);
        }

        internal ValueTask<IReadOnlyList<Message>> Request(
            TimeSpan timeout,
            CancellationToken token = default,
            ZLinkRequestAdmission? requestAdmission = null
        ) =>
            Node.RequestDirectWireAsync(
                _target,
                new ReadOnlyMemory<byte>[] { "request"u8.ToArray() },
                timeout,
                token,
                requestAdmission: requestAdmission
            );

        public async ValueTask DisposeAsync()
        {
            await Node.DisposeAsync();
            Server.Dispose();
            _context.Dispose();
        }
    }

    private sealed class OperationDeadlineAccess
    {
        private const System.Reflection.BindingFlags Flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        private static readonly Type NodeType = typeof(ZLinkManagedMeshNode);
        private static readonly System.Reflection.MethodInfo BeginMethod = NodeType
            .GetMethods(Flags)
            .Single(method =>
                method.Name == "TryBeginOperation" && method.GetParameters().Length == 3
            );
        private static readonly System.Reflection.MethodInfo ExpireMethod = NodeType.GetMethod(
            "ExpireOperations",
            Flags
        )!;
        private static readonly System.Reflection.MethodInfo TakeMethod = NodeType
            .GetMethods(Flags)
            .Single(method =>
                method.Name == "TryRemoveOperation"
                && method.GetParameters()[0].ParameterType == typeof(ulong)
            );
        private readonly ZLinkManagedMeshNode _node;
        private readonly object _operations;

        private OperationDeadlineAccess(ZLinkManagedMeshNode node)
        {
            _node = node;
            _operations = NodeType.GetField("_operations", Flags)!.GetValue(node)!;
        }

        internal static OperationDeadlineAccess For(ZLinkManagedMeshNode node) => new(node);

        internal int Count =>
            (int)_operations.GetType().GetProperty("Count")!.GetValue(_operations)!;

        internal object Begin(TimeSpan timeout)
        {
            object?[] registration = [MeshOperationKind.NodeRequest, timeout, null];
            Assert.True((bool)BeginMethod.Invoke(_node, registration)!);
            return registration[2]!;
        }

        internal TimeSpan Expire() => (TimeSpan)ExpireMethod.Invoke(_node, null)!;

        // The reply path's atomic take (spec E5): exactly one terminal owner.
        internal bool TakeForReply(object operation)
        {
            var identity = (MeshOperationId)
                operation.GetType().GetProperty("OperationId", Flags)!.GetValue(operation)!;
            return (bool)TakeMethod.Invoke(_node, [identity.Low, null])!;
        }

        internal CancellationToken Token(object operation) =>
            (CancellationToken)
                operation.GetType().GetProperty("Token", Flags)!.GetValue(operation)!;
    }

    [Fact]
    public async Task OperationDeadline_PreservesAbsoluteDeadlineAndResumesABudgetedVisit()
    {
        var clock = new OperationDeadlineClock();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: clock
        );
        const System.Reflection.BindingFlags flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        var type = typeof(ZLinkManagedMeshNode);
        var begin = type.GetMethods(flags)
            .Single(method =>
                method.Name == "TryBeginOperation" && method.GetParameters().Length == 3
            );
        var expire = type.GetMethod("ExpireOperations", flags)!;
        var operations = type.GetField("_operations", flags)!.GetValue(node)!;
        var count = operations.GetType().GetProperty("Count")!;
        object?[] longRegistration =
        [
            MeshOperationKind.NodeRequest,
            TimeSpan.FromMilliseconds(513),
            null,
        ];
        Assert.True((bool)begin.Invoke(node, longRegistration)!);
        // §10.1: skipped rotations cannot expire an entry before its absolute deadline.
        foreach (var tick in new[] { 1, 257, 512 })
        {
            clock.Timestamp = tick * TimeSpan.TicksPerMillisecond;
            expire.Invoke(node, null);
            Assert.Equal(1, count.GetValue(operations));
        }
        clock.Timestamp = 513 * TimeSpan.TicksPerMillisecond;
        expire.Invoke(node, null);
        Assert.Equal(0, count.GetValue(operations));
        object?[] firstRegistration =
        [
            MeshOperationKind.NodeRequest,
            TimeSpan.FromMilliseconds(1),
            null,
        ];
        Assert.True((bool)begin.Invoke(node, firstRegistration)!);
        var first = firstRegistration[2]!;
        var token = (CancellationToken)
            first.GetType().GetProperty("Token", flags)!.GetValue(first)!;
        // The first terminal spends the whole management budget on the injected clock.
        using var slowTerminal = token.Register(() =>
            clock.Timestamp +=
                Zlink.Framework.Runtime.Dispatch.ZLinkReceiveBatchBudget.MaximumMilliseconds
                * TimeSpan.TicksPerMillisecond
        );
        for (var index = 0; index < 2; index++)
            Assert.True(
                (bool)
                    begin.Invoke(
                        node,
                        [MeshOperationKind.NodeRequest, TimeSpan.FromMilliseconds(1), null]
                    )!
            );
        clock.Timestamp = 514 * TimeSpan.TicksPerMillisecond;
        expire.Invoke(node, null);
        Assert.Equal(2, count.GetValue(operations));
        expire.Invoke(node, null);
        Assert.Equal(0, count.GetValue(operations));
    }

    [Fact]
    public async Task OperationDeadline_VisitsBeyondWindowYieldAtTheBudgetAndResumeFromTheCursor()
    {
        var clock = new OperationDeadlineClock();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: clock
        );
        const System.Reflection.BindingFlags flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        var type = typeof(ZLinkManagedMeshNode);
        var begin = type.GetMethods(flags)
            .Single(method =>
                method.Name == "TryBeginOperation" && method.GetParameters().Length == 3
            );
        var expire = type.GetMethod("ExpireOperations", flags)!;
        var operations = type.GetField("_operations", flags)!.GetValue(node)!;
        var count = operations.GetType().GetProperty("Count")!;
        for (var index = 0; index < 8; index++)
            Assert.True(
                (bool)
                    begin.Invoke(
                        node,
                        [MeshOperationKind.NodeRequest, TimeSpan.FromMilliseconds(257), null]
                    )!
            );
        Assert.True(
            (bool)
                begin.Invoke(
                    node,
                    [MeshOperationKind.NodeRequest, TimeSpan.FromMilliseconds(1), null]
                )!
        );
        // §4.2: every slot visit, a rounds-only one included, reads the management budget
        // counted from entry; four reads exhaust it.
        clock.Timestamp = TimeSpan.TicksPerMillisecond;
        clock.Step = TimeSpan.TicksPerMillisecond / 4;
        expire.Invoke(node, null);
        Assert.Equal(9, count.GetValue(operations));
        var turns = 1;
        while ((int)count.GetValue(operations)! == 9)
        {
            Assert.True(++turns < 9);
            expire.Invoke(node, null);
        }
        Assert.Equal(8, count.GetValue(operations));
        clock.Step = 0;
        clock.Timestamp = 257 * TimeSpan.TicksPerMillisecond;
        expire.Invoke(node, null);
        Assert.Equal(0, count.GetValue(operations));
    }

    [Fact]
    public async Task Typed_receiver_releases_caller_codec_and_value_while_deadline_index_retains_entry()
    {
        var index = new ZLinkDeadlineIndex<ZLinkManagedMeshNode.PendingOperation>(
            ZLinkRequestDeadlineIndex.DefaultRotation.Ticks
        );
        var retained = await DispatchAsync(index);
        Assert.Same(retained.Entry, index.Head);
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        Assert.False(retained.Caller.TryGetTarget(out _));
        Assert.False(retained.Codec.TryGetTarget(out _));
        Assert.False(retained.Value.TryGetTarget(out _));
        Assert.Same(retained.Entry, index.Head);

        [MethodImpl(MethodImplOptions.NoInlining)]
        static async Task<(
            ZLinkManagedMeshNode.PendingOperation<object> Entry,
            WeakReference<Task<object>> Caller,
            WeakReference<Func<ZLinkManagedMeshNode.ManagedRequestCompletion, object>> Codec,
            WeakReference<object> Value
        )> DispatchAsync(ZLinkDeadlineIndex<ZLinkManagedMeshNode.PendingOperation> index)
        {
            object? captured = new object();
            Func<ZLinkManagedMeshNode.ManagedRequestCompletion, object>? codec = _ => captured!;
            var entry = new ZLinkManagedMeshNode.PendingOperation<object>(
                new MeshOperationId(1, 1),
                MeshOperationKind.NodeRequest,
                default,
                codec
            )
            {
                Deadline = TimeSpan.FromSeconds(1),
            };
            index.Register(entry, 0);
            var completions = new ZLinkMeshCompletionTable();
            completions.ReserveDispatcher();
            entry.DispatcherOwner = completions;
            Task<object>? caller = entry.Completion;
            object? value = new object();
            var references = (
                Entry: entry,
                Caller: new WeakReference<Task<object>>(caller),
                Codec: new WeakReference<
                    Func<ZLinkManagedMeshNode.ManagedRequestCompletion, object>
                >(codec),
                Value: new WeakReference<object>(value)
            );
            entry.Complete();
            entry.DispatchReply(value);
            await caller;
            await completions.CompletionDrained;
            caller = null;
            codec = null;
            value = null;
            captured = null;
            return references;
        }
    }

    [Fact]
    public async Task Canonical_prepare_registers_original_entry_before_readiness_and_owner_expiry()
    {
        var clock = new OperationDeadlineClock();
        await using var native = Systems.Zlink.Zlink.CreateContext();
        var context = new CanonicalRequestContext(native);
        var target = RoutingId.From("never-ready-target");
        var peer = new ZLinkMeshPeer(1)
        {
            RoutingId = target,
            PhysicalRoutingId = target,
            LifecycleGeneration = 1,
        };
        var peers = new Dictionary<RoutingId, ZLinkMeshPeer> { [target] = peer };
        var poller = new MeshReceiveWakeTests.ControlledReceivePoller { CaptureIdleWait = true };
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            deadlineTimeProvider: clock,
            receivePollerFactory: () => new CanonicalRequestPoller(poller),
            peersByRid: peers
        );
        node.SetRoutingId(RoutingId.From("never-ready-source"));
        var endpoint = $"inproc://never-ready-{Guid.NewGuid():N}";
        node.SetBind(endpoint);
        node.Start();
        await poller.MonitorRegistered.Task.WaitAsync(TimeSpan.FromSeconds(5));
        using var server = native.CreateRouterSocket();
        server.SetRoutingId(target);
        server.Connect(endpoint);
        using var routePoller = Systems.Zlink.Zlink.CreatePoller();
        routePoller.Add(server, PollEventFlags.PollRoute, 1);
        var events = new PollEvent[1];
        while (server.RoutesSnapshot().Count == 0)
            Assert.True(routePoller.Wait(events, TimeSpan.FromSeconds(5)) > 0);
        var payload = ZLinkRelocationTransferPayload.Create(
            new ZLinkRelocationEnvelope(
                Guid.NewGuid(),
                1,
                new byte[32],
                [
                    new ZLinkRelocationParticipantEnvelope(
                        new ZLinkAuthorityKey("actor:mesh:actor"),
                        ZLinkPlacementObjectKind.Actor,
                        1,
                        1,
                        new byte[] { 1 },
                        [],
                        []
                    ),
                ]
            ),
            1024
        );
        var source = node.RoutingId;
        var prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
            new(1, 1),
            1,
            new("source-owner", 1, source, 1, "v-source"),
            new(target, 1, "target-owner", 1),
            1,
            new(1, string.Empty, "actor", 1, 1),
            source,
            1,
            (ulong)payload.TotalLength,
            (uint)payload.ChunkCount,
            payload.ChecksumCrc32c,
            1,
            checked((ulong)DateTimeOffset.UtcNow.AddMinutes(1).ToUnixTimeMilliseconds())
        );
        using var cleanup = new CancellationTokenSource();
        var request = node.PrepareCanonicalRelocationAsync(
                target,
                prepare,
                payload,
                TimeSpan.FromMilliseconds(100),
                cleanup.Token
            )
            .AsTask();
        try
        {
            await RegisteredAsync().WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(1, node.PendingOperationCount);
            Assert.Equal(1, node.PendingCanonicalRelocationPrepareCount);
            Assert.Equal(0, context.NativeRequests);
            Assert.False(request.IsCompleted);
            clock.Timestamp = 100 * TimeSpan.TicksPerMillisecond;
            node.SignalReceiveOwner();
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                request.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
            Assert.Equal(0, node.PendingOperationCount);
            Assert.Equal(0, node.PendingCanonicalRelocationPrepareCount);
            Assert.Equal(0, context.NativeRequests);
            peer.RouteGeneration = node.ObservePeerRoute(target).SelectedGeneration;
            peer.Admitted = true;
            node.AddChannel("after-expiry");
            Assert.NotEqual(0UL, node.ObservePeerRoute(target).ReadyGeneration);
            node.SignalReceiveOwner();
            await Task.Yield();
            Assert.Equal(0, context.NativeRequests);
            Assert.Equal(0, node.PendingOperationCount);
        }
        finally
        {
            cleanup.Cancel();
            try
            {
                await request;
            }
            catch (OperationCanceledException) { }
            catch (ZLinkFrameworkException) { }
        }
        async Task RegisteredAsync()
        {
            while (node.PendingOperationCount == 0 && !request.IsCompleted)
                await Task.Yield();
        }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task Canonical_prepare_late_physical_reply_keeps_original_cancel_and_shared_ready(
        bool malformed,
        bool expireOwner
    )
    {
        await using var fixture = new CanonicalPhysicalRequest();
        await fixture.MakeReadyAsync();
        var expectedReady = new ZLinkServiceWireCodec.RelocationReadyRecord(
            fixture.Prepare.RelocationId,
            fixture.Prepare.TargetAttemptGeneration,
            fixture.Prepare.Coordinator,
            fixture.Prepare.Target,
            fixture.Prepare.Object,
            2
        );
        Assert.True(
            ZLinkServiceWireCodec.TryDecodeRelocationReady(
                ZLinkServiceWireCodec.EncodeRelocationReady(expectedReady),
                out var roundtrip,
                out var codecError
            )
        );
        Assert.Equal(ZLinkServiceWireCodec.DecodeError.None, codecError);
        Assert.Equal(expectedReady, roundtrip);
        using var cancellation = new CancellationTokenSource();
        var first = fixture.Request(cancellation.Token);
        using var firstReceived = fixture.ReceivePrepareAndChunks();
        if (expireOwner)
            fixture.AdvanceClock(TimeSpan.FromMilliseconds(125));
        var second = fixture.Request();
        using var secondReceived = fixture.ReceivePrepareAndChunks();
        Assert.Equal(2, fixture.Node.PendingOperationCount);
        Assert.Equal(1, fixture.Node.PendingCanonicalRelocationPrepareCount);
        var submissions = fixture.Context.Submissions.ToArray();
        Assert.Equal(2, submissions.Length);
        ZLinkFrameworkException? originalFailure = null;
        if (expireOwner)
        {
            fixture.AdvanceClock(TimeSpan.FromMilliseconds(250));
            originalFailure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                first.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, originalFailure.Kind);
        }
        else
        {
            cancellation.Cancel();
            var canceled = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
            Assert.Equal(cancellation.Token, canceled.CancellationToken);
        }
        Assert.Equal(1, fixture.Node.PendingOperationCount);
        Assert.False(second.IsCompleted);
        var ready = expectedReady;
        using var firstReply = Message.From(
            malformed ? new byte[] { 0xff } : ZLinkServiceWireCodec.EncodeRelocationReady(ready)
        );
        firstReceived.Reply().Message(firstReply).Submit();
        var firstParts = await submissions[0].Reply.WaitAsync(TimeSpan.FromSeconds(5));
        if (malformed)
        {
            await PartsDisposedAsync(firstParts).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(second.IsCompleted);
        }
        else
            Assert.Equal(ready, await second.WaitAsync(TimeSpan.FromSeconds(5)));
        // Settle the second physical leg even when the shared READY already won.
        using var secondReply = Message.From(ZLinkServiceWireCodec.EncodeRelocationReady(ready));
        secondReceived.Reply().Message(secondReply).Submit();
        var secondParts = await submissions[1].Reply.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ready, await second.WaitAsync(TimeSpan.FromSeconds(5)));
        await PartsDisposedAsync(firstParts).WaitAsync(TimeSpan.FromSeconds(5));
        await PartsDisposedAsync(secondParts).WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(0, fixture.Node.PendingOperationCount);
        Assert.Equal(0, fixture.Node.PendingCanonicalRelocationPrepareCount);
        Assert.Equal(2, fixture.Context.NativeRequests);
        if (expireOwner)
            Assert.Same(
                originalFailure,
                await Assert.ThrowsAsync<ZLinkFrameworkException>(() => first)
            );
        else
        {
            Assert.True(first.IsCanceled);
            var unchanged = await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);
            Assert.Equal(cancellation.Token, unchanged.CancellationToken);
        }
    }

    [Fact]
    public async Task Canonical_prepare_observes_completed_physical_reply_before_chunk_call_returns()
    {
        using var gate = new ChunkReturnGate();
        await using var fixture = new CanonicalPhysicalRequest(gate);
        await fixture.MakeReadyAsync();
        using var responder = new CanonicalPrepareResponder(fixture);
        responder.Start();
        long requestCalled = 0;
        var caller = Task.Run(() =>
        {
            requestCalled = Stopwatch.GetTimestamp();
            return fixture.Request();
        });
        try
        {
            await gate.Held.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var heldObserved = Stopwatch.GetTimestamp();
            var submission = Assert.Single(fixture.Context.Submissions);
            var parts = await submission.Reply.WaitAsync(TimeSpan.FromSeconds(5));
            var physicalObserved = Stopwatch.GetTimestamp();
            Assert.True(submission.Reply.IsCompletedSuccessfully);
            Assert.False(caller.IsCompleted);
            Assert.Equal(1, fixture.Node.PendingOperationCount);
            gate.Release();
            responder.Join();
            fixture.ReceiveChunks();
            Assert.Equal(responder.Ready, await caller.WaitAsync(TimeSpan.FromSeconds(5)));
            await PartsDisposedAsync(parts).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(0, fixture.Node.PendingOperationCount);
            Assert.Equal(1, fixture.Context.NativeRequests);
            output.WriteLine(
                $"request_call={requestCalled}; prepare_received={responder.PrepareReceived}; "
                    + $"ready_submitted={responder.ReadySubmitted}; held_observed={heldObserved}; "
                    + $"physical_success_observed={physicalObserved}; timestamp_frequency={Stopwatch.Frequency}; responder_alive={responder.IsAlive}"
            );
        }
        finally
        {
            gate.Release();
        }
    }

    [Fact]
    public async Task Canonical_prepare_withheld_physical_reply_hits_original_native_deadline()
    {
        using var gate = new ChunkReturnGate();
        await using var fixture = new CanonicalPhysicalRequest(gate);
        await fixture.MakeReadyAsync();
        using var responder = new CanonicalPrepareResponder(fixture, withholdReady: true);
        responder.Start();
        long requestCalled = 0;
        var caller = Task.Run(() =>
        {
            requestCalled = Stopwatch.GetTimestamp();
            return fixture.Request();
        });
        try
        {
            await gate.Held.Task.WaitAsync(TimeSpan.FromSeconds(5));
            await responder.Received.WaitAsync(TimeSpan.FromSeconds(5));
            var submission = Assert.Single(fixture.Context.Submissions);
            var timeout = await Assert.ThrowsAsync<ZlinkRequestException>(() =>
                submission.Reply.WaitAsync(TimeSpan.FromSeconds(5))
            );
            var timeoutObserved = Stopwatch.GetTimestamp();
            Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, timeout.Result);
            Assert.False(caller.IsCompleted);
            Assert.Equal(1, fixture.Node.PendingOperationCount);
            responder.Release();
            gate.Release();
            responder.Join();
            fixture.ReceiveChunks();
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                caller.WaitAsync(TimeSpan.FromSeconds(5))
            );
            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
            Assert.Equal(0, fixture.Node.PendingOperationCount);
            Assert.Equal(0, fixture.Node.PendingCanonicalRelocationPrepareCount);
            Assert.Equal(1, fixture.Context.NativeRequests);
            output.WriteLine(
                $"request_call={requestCalled}; prepare_received={responder.PrepareReceived}; "
                    + $"native_timeout_observed={timeoutObserved}; ready_submitted={responder.ReadySubmitted}; "
                    + $"timestamp_frequency={Stopwatch.Frequency}; responder_alive={responder.IsAlive}"
            );
        }
        finally
        {
            responder.Release();
            gate.Release();
        }
    }

    private sealed class CanonicalPrepareResponder : IDisposable
    {
        private readonly ManualResetEventSlim _started = new(false);
        private readonly ManualResetEventSlim _ready;
        private readonly Thread _thread;
        private readonly TaskCompletionSource _received = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly TaskCompletionSource _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        internal Task Received => _received.Task;
        internal ZLinkServiceWireCodec.RelocationReadyRecord Ready { get; }
        internal long PrepareReceived { get; private set; }
        internal long ReadySubmitted { get; private set; }
        internal bool IsAlive => _thread.IsAlive;

        internal CanonicalPrepareResponder(
            CanonicalPhysicalRequest fixture,
            bool withholdReady = false
        )
        {
            Ready = new(
                fixture.Prepare.RelocationId,
                fixture.Prepare.TargetAttemptGeneration,
                fixture.Prepare.Coordinator,
                fixture.Prepare.Target,
                fixture.Prepare.Object,
                2
            );
            _ready = new(!withholdReady);
            // The server has no poller after construction. Join transfers it back before chunks.
            _thread = new Thread(() =>
            {
                try
                {
                    _started.Set();
                    using var received = fixture.ReceivePrepare();
                    PrepareReceived = Stopwatch.GetTimestamp();
                    _received.TrySetResult();
                    Assert.True(_ready.Wait(TimeSpan.FromSeconds(5)));
                    using var reply = Message.From(
                        ZLinkServiceWireCodec.EncodeRelocationReady(Ready)
                    );
                    received.Reply().Message(reply).Submit();
                    ReadySubmitted = Stopwatch.GetTimestamp();
                    _completion.TrySetResult();
                }
                catch (Exception error)
                {
                    _received.TrySetException(error);
                    _completion.TrySetException(error);
                }
            });
        }

        internal void Start()
        {
            _thread.Start();
            Assert.True(_started.Wait(TimeSpan.FromSeconds(5)));
        }

        internal void Release() => _ready.Set();

        internal void Join()
        {
            Assert.True(_thread.Join(TimeSpan.FromSeconds(5)));
            _completion.Task.GetAwaiter().GetResult();
        }

        public void Dispose()
        {
            Release();
            try
            {
                if ((_thread.ThreadState & System.Threading.ThreadState.Unstarted) == 0)
                    Join();
            }
            finally
            {
                if (!_thread.IsAlive)
                {
                    _started.Dispose();
                    _ready.Dispose();
                }
            }
        }
    }

    private sealed class ChunkReturnGate : IDisposable
    {
        private readonly ManualResetEventSlim _return = new(false);
        internal readonly TaskCompletionSource Held = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        internal void Hold()
        {
            Held.TrySetResult();
            Assert.True(_return.Wait(TimeSpan.FromSeconds(5)));
        }

        internal void Release() => _return.Set();

        public void Dispose() => _return.Dispose();
    }

    private static async Task PartsDisposedAsync(IReadOnlyList<Message> parts)
    {
        foreach (var part in parts)
        {
            while (true)
            {
                try
                {
                    _ = part.Size;
                }
                catch (ObjectDisposedException)
                {
                    break;
                }
                await Task.Yield();
            }
        }
    }

    private sealed class CanonicalPhysicalRequest : IAsyncDisposable
    {
        private readonly IContext _native = Systems.Zlink.Zlink.CreateContext();
        private readonly IRouterSocket _server;
        private readonly OperationDeadlineClock _clock = new();
        private readonly ZLinkMeshPeer _peer;
        private readonly ZLinkRelocationTransferPayload _payload;
        internal readonly CanonicalRequestContext Context;
        internal readonly ZLinkManagedMeshNode Node;
        internal readonly ZLinkServiceWireCodec.RelocationPrepareRecord Prepare;

        internal CanonicalPhysicalRequest(ChunkReturnGate? chunkGate = null)
        {
            Context = new CanonicalRequestContext(_native) { ChunkGate = chunkGate };
            var target = RoutingId.From("late-physical-target");
            _peer = new ZLinkMeshPeer(1)
            {
                RoutingId = target,
                PhysicalRoutingId = target,
                LifecycleGeneration = 1,
            };
            var poller = new MeshReceiveWakeTests.ControlledReceivePoller
            {
                CaptureIdleWait = true,
            };
            Node = new ZLinkManagedMeshNode(
                Context,
                MeshName,
                deadlineTimeProvider: _clock,
                receivePollerFactory: () => new CanonicalRequestPoller(poller),
                peersByRid: new Dictionary<RoutingId, ZLinkMeshPeer> { [target] = _peer }
            );
            Node.SetRoutingId(RoutingId.From("late-physical-source"));
            var endpoint = $"inproc://late-physical-{Guid.NewGuid():N}";
            Node.SetBind(endpoint);
            Node.Start();
            _server = _native.CreateRouterSocket();
            _server.SetRoutingId(target);
            _server.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
            _server.Connect(endpoint);
            using var routePoller = Systems.Zlink.Zlink.CreatePoller();
            routePoller.Add(_server, PollEventFlags.PollRoute, 1);
            var events = new PollEvent[1];
            while (_server.RoutesSnapshot().Count == 0)
                Assert.True(routePoller.Wait(events, TimeSpan.FromSeconds(5)) > 0);
            _payload = ZLinkRelocationTransferPayload.Create(
                new ZLinkRelocationEnvelope(
                    Guid.NewGuid(),
                    1,
                    new byte[32],
                    [
                        new ZLinkRelocationParticipantEnvelope(
                            new ZLinkAuthorityKey("actor:mesh:actor"),
                            ZLinkPlacementObjectKind.Actor,
                            1,
                            1,
                            new byte[] { 1 },
                            [],
                            []
                        ),
                    ]
                ),
                1024
            );
            var source = Node.RoutingId;
            Prepare = new ZLinkServiceWireCodec.RelocationPrepareRecord(
                new(1, 1),
                1,
                new("source-owner", 1, source, 1, "v-source"),
                new(target, 1, "target-owner", 1),
                1,
                new(1, string.Empty, "actor", 1, 1),
                source,
                1,
                (ulong)_payload.TotalLength,
                (uint)_payload.ChunkCount,
                _payload.ChecksumCrc32c,
                1,
                checked((ulong)DateTimeOffset.UtcNow.AddMinutes(1).ToUnixTimeMilliseconds())
            );
        }

        internal async Task MakeReadyAsync()
        {
            await WaitRouteAsync().WaitAsync(TimeSpan.FromSeconds(5));
            _peer.RouteGeneration = Node.ObservePeerRoute(_peer.RoutingId).SelectedGeneration;
            _peer.Admitted = true;
            Node.AddChannel("physical-ready");
            Assert.NotEqual(0UL, Node.ObservePeerRoute(_peer.RoutingId).ReadyGeneration);

            async Task WaitRouteAsync()
            {
                while (Node.ObservePeerRoute(_peer.RoutingId).SelectedGeneration == 0)
                {
                    Node.SignalReceiveOwner();
                    await Task.Yield();
                }
            }
        }

        internal void AdvanceClock(TimeSpan elapsed)
        {
            _clock.Timestamp = elapsed.Ticks;
            Node.SignalReceiveOwner();
        }

        internal Task<ZLinkServiceWireCodec.RelocationReadyRecord> Request(
            CancellationToken token = default
        ) =>
            Node.PrepareCanonicalRelocationAsync(
                    _peer.RoutingId,
                    Prepare,
                    _payload,
                    TimeSpan.FromMilliseconds(250),
                    token
                )
                .AsTask();

        internal Received ReceivePrepareAndChunks()
        {
            var request = ReceivePrepare();
            ReceiveChunks();
            return request;
        }

        internal Received ReceivePrepare()
        {
            var request = Received.Create();
            while (true)
            {
                Assert.True(_server.Recv(request));
                if (
                    request.MessageType == ReceivedMessageType.Request
                    && ZLinkServiceWireCodec.TryDecodeRelocationPrepare(
                        request.Parts[0].AsReadOnlySpan(),
                        out _,
                        out _
                    )
                )
                    break;
            }
            return request;
        }

        internal void ReceiveChunks()
        {
            using var chunk = Received.Create();
            for (var ordinal = 0; ordinal < _payload.ChunkCount; )
            {
                Assert.True(_server.Recv(chunk));
                if (
                    ZLinkServiceWireCodec.TryDecodeRelocationState(
                        chunk.Parts[0].AsReadOnlySpan(),
                        out var state,
                        out _
                    )
                )
                {
                    Assert.Equal(Prepare.RelocationId, state.RelocationId);
                    ordinal++;
                }
            }
        }

        public async ValueTask DisposeAsync()
        {
            await Node.DisposeAsync();
            _server.Dispose();
            await _native.DisposeAsync();
        }
    }

    private sealed class CanonicalRequestContext(IContext inner) : IContext
    {
        internal int NativeRequests;
        internal ChunkReturnGate? ChunkGate;
        internal readonly System.Collections.Concurrent.ConcurrentQueue<RequestSubmission> Submissions =
            new();
        public IContextOptions Options => inner.Options;

        public IRouterSocket CreateRouterSocket() =>
            new CountingRouter(inner.CreateRouterSocket(), this);

        public IPairSocket CreatePairSocket() => inner.CreatePairSocket();

        public IDealerSocket CreateDealerSocket() => inner.CreateDealerSocket();

        public IPubSocket CreatePubSocket() => inner.CreatePubSocket();

        public ISubSocket CreateSubSocket() => inner.CreateSubSocket();

        public IXPubSocket CreateXPubSocket() => inner.CreateXPubSocket();

        public IXSubSocket CreateXSubSocket() => inner.CreateXSubSocket();

        public IStreamSocket CreateStreamSocket() => inner.CreateStreamSocket();

        public void Shutdown() => inner.Shutdown();

        public void RecalculateAutoHwm() => inner.RecalculateAutoHwm();

        public CoreHwmBudgetSnapshot GetCoreHwmBudgetSnapshot() => inner.GetCoreHwmBudgetSnapshot();

        public void ResetCoreHwmBudgetMetrics() => inner.ResetCoreHwmBudgetMetrics();

        public void Dispose() => inner.Dispose();

        public ValueTask DisposeAsync() => inner.DisposeAsync();

        private sealed class CapturedSend(SendOperation send, CanonicalRequestContext owner)
            : SendOperation,
                SendSubmitOperation
        {
            private SendSubmitOperation? _submit;
            private bool _chunk;

            public SendSubmitOperation Message(Message message)
            {
                if (_submit is null)
                    _chunk =
                        owner.ChunkGate is not null
                        && ZLinkServiceWireCodec.TryDecodeRelocationState(
                            message.AsReadOnlySpan(),
                            out _,
                            out _
                        );
                _submit = _submit is null ? send.Message(message) : _submit.Message(message);
                return this;
            }

            public void Submit() => _submit!.Submit();

            public SendSubmission Async(CancellationToken cancellationToken = default)
            {
                var submission = _submit!.Async(cancellationToken);
                if (_chunk)
                    owner.ChunkGate!.Hold();
                return submission;
            }
        }

        private sealed class CapturedRequest(
            RequestOperation request,
            CanonicalRequestContext owner
        ) : RequestOperation, RequestSubmitOperation
        {
            private RequestSubmitOperation? _submit;

            public RequestSubmitOperation Message(Message message)
            {
                _submit = _submit is null ? request.Message(message) : _submit.Message(message);
                return this;
            }

            public RequestSubmitOperation Timeout(TimeSpan timeout)
            {
                _submit = _submit!.Timeout(timeout);
                return this;
            }

            public IReadOnlyList<Message> Submit() => _submit!.Submit();

            public RequestSubmission Async(CancellationToken cancellationToken = default)
            {
                Interlocked.Increment(ref owner.NativeRequests);
                var submission = _submit!.Async(cancellationToken);
                owner.Submissions.Enqueue(submission);
                return submission;
            }
        }

        internal sealed class CountingRouter(IRouterSocket inner, CanonicalRequestContext owner)
            : PressureSocket<IRouterSocket>(inner),
                IRouterSocket
        {
            internal IRouterSocket Native => inner;
            public new RouterSocketOptions Options => inner.Options;

            public void SetRoutingId(RoutingId rid) => inner.SetRoutingId(rid);

            public RoutingId GetRoutingId() => inner.GetRoutingId();

            public SendOperation Send(RoutingId rid) => new CapturedSend(inner.Send(rid), owner);

            public RequestOperation Request(RoutingId rid) =>
                new CapturedRequest(inner.Request(rid), owner);

            public ReplyOperation Reply(RoutingId rid, ReplyToken token) => inner.Reply(rid, token);

            public IReadOnlyList<RouterRoute> RoutesSnapshot() => inner.RoutesSnapshot();
        }
    }

    private sealed class CanonicalRequestPoller(IPoller inner) : IPoller
    {
        private static IZlinkSocket Native(IZlinkSocket socket) =>
            socket is CanonicalRequestContext.CountingRouter router ? router.Native : socket;

        public int Size => inner.Size;

        public int Wait(Span<PollEvent> events, TimeSpan timeout) => inner.Wait(events, timeout);

        public void Add(IZlinkSocket socket, PollEventFlags events, nuint slot) =>
            inner.Add(Native(socket), events, slot);

        public void Add(ISocketMonitor monitor, PollEventFlags events, nuint slot) =>
            inner.Add(monitor, events, slot);

        public void Add(IZlinkTimer timer, nuint slot) => inner.Add(timer, slot);

        public void AddFd(int fd, PollEventFlags events, nuint slot) =>
            inner.AddFd(fd, events, slot);

        public void Modify(IZlinkSocket socket, PollEventFlags events) =>
            inner.Modify(Native(socket), events);

        public void Modify(ISocketMonitor monitor, PollEventFlags events) =>
            inner.Modify(monitor, events);

        public void ModifyFd(int fd, PollEventFlags events) => inner.ModifyFd(fd, events);

        public bool Remove(IZlinkSocket socket) => inner.Remove(Native(socket));

        public bool Remove(ISocketMonitor monitor) => inner.Remove(monitor);

        public bool Remove(IZlinkTimer timer) => inner.Remove(timer);

        public bool Remove(int fd) => inner.Remove(fd);

        public void Clear() => inner.Clear();

        public void Close() => inner.Close();

        public void Dispose() => inner.Dispose();

        public ValueTask DisposeAsync() => inner.DisposeAsync();
    }

    private sealed class OperationDeadlineClock : TimeProvider
    {
        internal long Timestamp;
        internal long Step;
        internal Action? TimestampRead;
        public override long TimestampFrequency => TimeSpan.TicksPerSecond;

        // Each read advances the clock by Step.
        public override long GetTimestamp()
        {
            var timestamp = Timestamp;
            Timestamp += Step;
            TimestampRead?.Invoke();
            return timestamp;
        }

        public override DateTimeOffset GetUtcNow() => DateTimeOffset.UnixEpoch;
    }

    [Fact]
    public async Task OperationTerminal_OnlyTheAtomicTakeOwnerCancelsItsLifetime()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        const System.Reflection.BindingFlags flags =
            System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic;
        var type = typeof(ZLinkManagedMeshNode);
        var begin = type.GetMethods(flags)
            .Single(method =>
                method.Name == "TryBeginOperation" && method.GetParameters().Length == 3
            );
        object?[] registration = [MeshOperationKind.NodeRequest, TimeSpan.FromHours(1), null];
        Assert.True((bool)begin.Invoke(node, registration)!);
        var pending = registration[2]!;
        var pendingType = pending.GetType();
        var identity = (MeshOperationId)
            pendingType.GetProperty("OperationId", flags)!.GetValue(pending)!;
        var take = type.GetMethods(flags)
            .Single(method =>
                method.Name == "TryRemoveOperation"
                && method.GetParameters()[0].ParameterType == typeof(ulong)
            );
        object?[] terminal = [identity.Low, null];
        Assert.True((bool)take.Invoke(node, terminal)!);
        type.GetMethod("RemoveManagedOperation", flags)!.Invoke(node, [pending]);
        var token = (CancellationToken)pendingType.GetProperty("Token", flags)!.GetValue(pending)!;
        Assert.False(token.IsCancellationRequested);
        pendingType.GetMethod("Complete", flags)!.Invoke(pending, null);
        Assert.True(token.IsCancellationRequested);
    }

    [Fact]
    public void DrainGate_SealedForShutdown_OnlyByTheShutdownOwner()
    {
        var gate = new ZLinkDrainAdmissionGate();
        Assert.False(gate.IsSealedForShutdown);

        // A relocation fence seals application admission but is not a shutdown.
        Assert.True(
            gate.TryBeginRelocationFence(_ =>
            {
                gate.Seal();
                return true;
            })
        );
        Assert.True(gate.IsSealed);
        Assert.False(gate.IsSealedForShutdown);

        gate.ClaimShutdown();
        Assert.True(gate.IsSealedForShutdown);

        gate.Reset();
        Assert.False(gate.IsSealedForShutdown);
    }

    [Fact]
    public async Task CrossedHelloAdmit_CompletesOnceOnBothSides()
    {
        var scheduler = new GatedTaskScheduler();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var left = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler
        );
        await using var right = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler
        );
        var suffix = Guid.NewGuid().ToString("N");
        var leftRid = RoutingId.From($"crossed-left-{suffix}");
        var rightRid = RoutingId.From($"crossed-right-{suffix}");
        left.SetRoutingId(leftRid);
        left.SetBind(EphemeralTcpEndpoint);
        left.AddChannel(MeshName);
        right.SetRoutingId(rightRid);
        right.SetBind(EphemeralTcpEndpoint);
        right.AddChannel(MeshName);
        using var leftMonitor = left.OpenMonitor();
        using var rightMonitor = right.OpenMonitor();

        // Both sides connect and announce, so each Hello is accepted and the
        // Admit that answers it arrives after the receiver already admitted
        // the same descriptor (the ZoneWorld E5 restart shape).
        try
        {
            // The actual sockets choose and hold their ports. Keep both Hello
            // submissions queued until both outbound intents are configured.
            left.Start();
            right.Start();
            var leftEndpoint = left.Status().LocalEndpoint;
            var rightEndpoint = right.Status().LocalEndpoint;
            Assert.NotEqual(leftEndpoint, rightEndpoint);
            // Core keeps the pipe started by the smaller RID (left) and, when
            // the other pipe was selected first, hands the RID over to it with
            // a new route generation, which ends that admission (Core socket
            // README routing ID duplicate policy, ROUTER §10.1; transport
            // liveness §5). Left's Hello is queued once left selected its own
            // pipe, so the reverse pipe is created after the kept one.
            left.ConnectPeer(rightEndpoint, rightRid);
            await scheduler.Queued.WaitAsync(TimeSpan.FromSeconds(5));
            right.ConnectPeer(leftEndpoint, leftRid);
        }
        finally
        {
            scheduler.Release();
        }
        (
            IMeshNodeMonitor Monitor,
            RoutingId PeerRid,
            MeshNodeStatus Status,
            MeshNodePeer[] Peers
        )[] observations = [];
        await WaitUntilAsync(() =>
        {
            observations = new[]
            {
                (
                    Monitor: leftMonitor,
                    PeerRid: rightRid,
                    Status: left.Status(),
                    Peers: left.Peers()
                ),
                (
                    Monitor: rightMonitor,
                    PeerRid: leftRid,
                    Status: right.Status(),
                    Peers: right.Peers()
                ),
            };
            return observations.All(observation =>
                observation.Status.State == MeshNodeState.Ready
                && observation.Status.AdmittedPeerCount == 1
                && observation.Peers.Length == 1
                && observation.Peers[0].RoutingId == observation.PeerRid
                && observation.Peers[0].State == MeshPeerState.Admitted
            );
        });
        // Preserve both nodes' complete event order before an assertion can fail.
        (MeshMonitorStatus Status, List<MeshMonitorEvent> Events) ObserveMonitor(
            IMeshNodeMonitor monitor,
            RoutingId peerRid,
            string phase
        )
        {
            var events = new List<MeshMonitorEvent>();
            while (monitor.Recv(RecvFlags.DontWait) is { } meshEvent)
                events.Add(meshEvent);
            var status = monitor.Status();
            output.WriteLine(
                "{0} monitor peer={1} status={2} events=[{3}]",
                phase,
                peerRid,
                System.Text.Json.JsonSerializer.Serialize(status),
                string.Join(
                    ", ",
                    events.Select(meshEvent => $"{meshEvent.Kind}:{meshEvent.PeerRid}")
                )
            );
            return (status, events);
        }
        var monitorObservations = observations
            .Select(observation =>
                (
                    Observation: observation,
                    Snapshot: ObserveMonitor(observation.Monitor, observation.PeerRid, "Initial")
                )
            )
            .ToArray();
        foreach (var (observation, (status, events)) in monitorObservations)
        {
            var (_, peerRid, nodeStatus, peers) = observation;
            Assert.Single(
                events,
                meshEvent =>
                    meshEvent.PeerRid == peerRid
                    && meshEvent.Kind == MeshMonitorEventKind.PeerAdmitted
            );
            Assert.DoesNotContain(
                events,
                meshEvent =>
                    meshEvent.PeerRid == peerRid
                    && meshEvent.Kind == MeshMonitorEventKind.PeerClosed
            );
            Assert.Equal(1UL, status.PeerAdmitted);
            Assert.Equal(0UL, status.ProtocolErrors);
            Assert.Equal(MeshNodeState.Ready, nodeStatus.State);
            Assert.Equal(1U, nodeStatus.AdmittedPeerCount);
            var admitted = Assert.Single(peers, peer => peer.RoutingId == peerRid);
            Assert.Equal(MeshPeerState.Admitted, admitted.State);
        }
        var finalObservations = monitorObservations
            .Select(first =>
                (
                    PeerRid: first.Observation.PeerRid,
                    InitialStatus: first.Snapshot.Status,
                    Snapshot: ObserveMonitor(
                        first.Observation.Monitor,
                        first.Observation.PeerRid,
                        "Final"
                    )
                )
            )
            .ToArray();
        foreach (var (peerRid, initialStatus, (status, events)) in finalObservations)
        {
            Assert.DoesNotContain(
                events,
                meshEvent =>
                    meshEvent.PeerRid == peerRid
                    && meshEvent.Kind
                        is MeshMonitorEventKind.PeerAdmitted
                            or MeshMonitorEventKind.PeerClosed
            );
            Assert.Equal(initialStatus.PeerAdmitted, status.PeerAdmitted);
            Assert.Equal(initialStatus.ProtocolErrors, status.ProtocolErrors);
            Assert.Equal(initialStatus.PeerRejected, status.PeerRejected);
        }
    }

    [Fact]
    public async Task InboundAdmissionBeforeEndpointIntent_KeepsOnePeerForTheRid()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var left = new ZLinkManagedMeshNode(context, MeshName);
        await using var right = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var leftRid = RoutingId.From($"inbound-left-{suffix}");
        var rightRid = RoutingId.From($"inbound-right-{suffix}");
        left.SetRoutingId(leftRid);
        left.SetBind(EphemeralTcpEndpoint);
        left.AddChannel(MeshName);
        right.SetRoutingId(rightRid);
        right.SetBind(EphemeralTcpEndpoint);
        right.AddChannel(MeshName);
        left.Start();
        right.Start();
        right.ConnectPeer(left.Status().LocalEndpoint, leftRid);
        await WaitUntilAsync(() =>
            left.Status().AdmittedPeerCount == 1 && right.Status().AdmittedPeerCount == 1
        );

        var intent = left.ConnectPeer(right.Status().LocalEndpoint);
        var peer = Assert.Single(left.Peers());
        Assert.Equal(rightRid, peer.RoutingId);
        Assert.Equal(intent, peer.ConnectionIntentId);
        Assert.Equal(MeshPeerState.Admitted, peer.State);

        left.RemovePeerConnection(intent);
        Assert.Equal(0U, left.Status().AdmittedPeerCount);
        Assert.DoesNotContain(left.Peers(), candidate => candidate.ConnectionIntentId == intent);
        Assert.DoesNotContain(left.Peers(), candidate => candidate.RoutingId == rightRid);
    }

    [Fact]
    public async Task FailedAdmitSend_KeepsConfiguredNotRequiredPeer()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("not-required-send-failure");
        const ulong routeGeneration = 17;
        node.ConnectPeer("tcp://127.0.0.1:20002", rid);
        var peer = new ZLinkMeshPeer(999)
        {
            RoutingId = rid,
            PhysicalRoutingId = rid,
            RouteGeneration = routeGeneration,
            State = MeshPeerState.NotRequired,
        };
        const System.Reflection.BindingFlags Private =
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
        var index =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                typeof(ZLinkManagedMeshNode).GetField("_peersByRid", Private)!.GetValue(node)!;
        index.Add(rid, peer);
        typeof(ZLinkManagedMeshNode)
            .GetMethod("ClosePeerAfterControlSendFailure", Private)!
            .Invoke(node, [rid, routeGeneration]);

        Assert.Equal(MeshPeerState.NotRequired, peer.State);
        Assert.True(index.ContainsKey(rid));
        Assert.Equal(0UL, peer.RouteGeneration);
    }

    [Fact]
    public async Task NewHandshakeRoute_FencesControlQueuedForOldSnapshot()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("handover-control-peer");
        node.ConnectPeer("tcp://127.0.0.1:20004", rid);
        const System.Reflection.BindingFlags Private =
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
        var routes = (ZLinkMeshSelectedRoutes)
            typeof(ZLinkManagedMeshNode).GetField("_selectedRoutes", Private)!.GetValue(node)!;
        routes.Apply([new RouterRoute(rid, 11)]);
        var index =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                typeof(ZLinkManagedMeshNode).GetField("_peersByRid", Private)!.GetValue(node)!;
        index.Add(rid, new ZLinkMeshPeer(999) { RoutingId = rid, RouteGeneration = 12 });
        var hasTarget = typeof(ZLinkManagedMeshNode).GetMethod("HasCurrentControlTarget", Private)!;

        Assert.False(
            (bool)hasTarget.Invoke(node, [rid, 11UL, ServiceWireConstants.Command.Admit])!
        );
        Assert.True((bool)hasTarget.Invoke(node, [rid, 12UL, ServiceWireConstants.Command.Admit])!);
    }

    [Fact]
    public async Task RejectedNewHandshake_FencesOldControlWithoutSnapshotUpdate()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("rejected-handover-peer");
        const string endpoint = "tcp://127.0.0.1:20005";
        node.ConnectPeer(endpoint, rid);
        const System.Reflection.BindingFlags Private =
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
        var routes = (ZLinkMeshSelectedRoutes)
            typeof(ZLinkManagedMeshNode).GetField("_selectedRoutes", Private)!.GetValue(node)!;
        routes.Apply([new RouterRoute(rid, 11)]);
        var index =
            (Dictionary<RoutingId, ZLinkMeshPeer>)
                typeof(ZLinkManagedMeshNode).GetField("_peersByRid", Private)!.GetValue(node)!;
        index.Add(
            rid,
            new ZLinkMeshPeer(999)
            {
                RoutingId = rid,
                PhysicalRoutingId = rid,
                RouteGeneration = 11,
                Admitted = true,
                State = MeshPeerState.Admitted,
            }
        );
        var wrongMesh = new ZLinkServiceWireCodec.AdmissionRecord(
            "different-mesh",
            ZLinkServiceSecurityIdentity.Plaintext,
            endpoint,
            7,
            1,
            new Dictionary<string, uint>(),
            1,
            1,
            (byte)ZLinkMeshNodeObjectRole.Server,
            100,
            0,
            0,
            0,
            0,
            new Dictionary<byte, byte[]>(),
            []
        );
        typeof(ZLinkManagedMeshNode)
            .GetMethod("ProcessAdmissionCore", Private)!
            .Invoke(node, [rid, 12UL, ServiceWireConstants.Command.Hello, wrongMesh, null]);

        var hasTarget = typeof(ZLinkManagedMeshNode).GetMethod("HasCurrentControlTarget", Private)!;
        Assert.False(
            (bool)hasTarget.Invoke(node, [rid, 11UL, ServiceWireConstants.Command.Admit])!
        );
        Assert.False(
            (bool)hasTarget.Invoke(node, [rid, 12UL, ServiceWireConstants.Command.Admit])!
        );
        Assert.True(index.TryGetValue(rid, out var disconnected));
        Assert.Equal(0UL, disconnected.RouteGeneration);
        Assert.False(disconnected.Admitted);
    }

    [Fact]
    public async Task SameRid_DifferentEndpointIntent_AdmitsTheConnectedEndpoint()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = new ZLinkManagedMeshNode(context, MeshName);
        await using var remote = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var remoteRid = RoutingId.From($"intent-remote-{suffix}");
        owner.SetRoutingId(RoutingId.From($"intent-owner-{suffix}"));
        owner.SetBind(EphemeralTcpEndpoint);
        owner.AddChannel(MeshName);
        remote.SetRoutingId(remoteRid);
        remote.SetBind(EphemeralTcpEndpoint);
        remote.AddChannel(MeshName);
        remote.Start();
        owner.Start();

        var obsoleteIntent = owner.ConnectPeer("tcp://127.0.0.1:1", remoteRid);
        owner.ConnectPeer(remote.Status().LocalEndpoint, remoteRid);
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1);
        owner.RemovePeerConnection(obsoleteIntent);
        Assert.Equal(1U, owner.Status().AdmittedPeerCount);
    }

    [Fact]
    public async Task SameRid_SameEndpoint_UsesMatchingSecurityIntent()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = new ZLinkManagedMeshNode(context, MeshName);
        await using var remote = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var remoteRid = RoutingId.From($"security-remote-{suffix}");
        owner.SetRoutingId(RoutingId.From($"security-owner-{suffix}"));
        owner.SetBind(EphemeralTcpEndpoint);
        owner.AddChannel(MeshName);
        remote.SetRoutingId(remoteRid);
        remote.SetBind(EphemeralTcpEndpoint);
        remote.AddChannel(MeshName);
        remote.Start();
        var endpoint = remote.Status().LocalEndpoint;
        owner.ConnectPeer(endpoint, remoteRid, "unavailable-security-identity");
        owner.ConnectPeer(endpoint, remoteRid);
        owner.Start();

        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1);
        Assert.Equal(
            remoteRid,
            Assert
                .Single(owner.Peers().Where(peer => peer.State == MeshPeerState.Admitted))
                .RoutingId
        );
    }

    [Fact]
    public async Task RepeatedEndpointConfiguration_ReusesOnePendingIntent()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var rid = RoutingId.From("pending-intent-peer");
        const string endpoint = "tcp://127.0.0.1:20003";

        var first = node.ConnectPeer(endpoint, rid);
        var repeated = node.ConnectPeer(endpoint, rid);

        Assert.Equal(first, repeated);
        Assert.Equal(first, Assert.Single(node.Peers()).ConnectionIntentId);
    }

    [Fact]
    public async Task EndpointIntent_LearnsReplacementRidAfterRouteLoss()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var owner = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        owner.SetRoutingId(RoutingId.From($"replacement-owner-{suffix}"));
        owner.SetBind(EphemeralTcpEndpoint);
        owner.AddChannel(MeshName);
        owner.Start();

        var firstRid = RoutingId.From($"replacement-first-{suffix}");
        var first = new ZLinkManagedMeshNode(context, MeshName);
        first.SetRoutingId(firstRid);
        first.SetBind(EphemeralTcpEndpoint);
        first.AddChannel(MeshName);
        first.Start();
        var endpoint = first.Status().LocalEndpoint;
        owner.ConnectPeer(endpoint);
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 1);
        await first.DisposeAsync();
        await WaitUntilAsync(() => owner.Status().AdmittedPeerCount == 0);

        await using var replacement = new ZLinkManagedMeshNode(context, MeshName);
        var replacementRid = RoutingId.From($"replacement-second-{suffix}");
        replacement.SetRoutingId(replacementRid);
        replacement.SetBind(endpoint);
        replacement.AddChannel(MeshName);
        replacement.Start();
        await WaitUntilAsync(() =>
            owner
                .Peers()
                .Any(peer =>
                    peer.RoutingId == replacementRid && peer.State == MeshPeerState.Admitted
                )
        );
        Assert.DoesNotContain(owner.Peers(), peer => peer.RoutingId == firstRid);
    }

    [Fact]
    public async Task HelloBeforeRouteObservation_AdmitIsSentOnTheRecordRoute()
    {
        // Core ROUTER §10.1: a record carries the generation of the route
        // that delivered it. The receive loop can dispatch a Hello before its
        // snapshot observes that route; the admission epoch is the record's
        // route, so the observation arriving later must not fence the Admit.
        var scheduler = new GatedTaskScheduler();
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler
        );
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"record-route-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        var peerRid = RoutingId.From($"record-route-peer-{suffix}");
        const System.Reflection.BindingFlags Private =
            System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance;
        var routes = (ZLinkMeshSelectedRoutes)
            typeof(ZLinkManagedMeshNode).GetField("_selectedRoutes", Private)!.GetValue(node)!;
        var lane = (Zlink.Framework.Runtime.Execution.ZLinkStateLane)
            typeof(ZLinkManagedMeshNode).GetField("_lane", Private)!.GetValue(node)!;
        try
        {
            node.Start();
            using var peer = context.CreateDealerSocket();
            peer.SetRoutingId(peerRid);
            peer.Connect(node.Status().LocalEndpoint);
            await WaitUntilAsync(() => routes.GenerationOf(peerRid) != 0);
            var generation = routes.GenerationOf(peerRid);

            // The snapshot has not observed the route yet.
            await lane.RunAsync(routes.Clear);
            await SendAsync(
                peer,
                ZLinkServiceWireCodec.EncodeRouteAdmission(
                    ServiceWireConstants.Command.Hello,
                    MeshName,
                    $"inproc://record-route-peer-{suffix}",
                    lifecycleGeneration: 7,
                    descriptorRevision: 3,
                    new Dictionary<string, uint>(StringComparer.Ordinal),
                    objectRole: (byte)ZLinkMeshNodeObjectRole.Server
                )
            );
            await WaitUntilAsync(() =>
                node.Peers().Any(candidate => candidate.RoutingId == peerRid)
            );

            scheduler.Release();

            await ReceiveAdmitAsync(peer);
            Assert.Equal(0U, node.Status().AdmittedPeerCount);
            await lane.RunAsync(() => routes.Apply([new RouterRoute(peerRid, generation)]));
        }
        finally
        {
            scheduler.Release();
        }
    }

    [Fact]
    public async Task IdempotentAdmit_CompletesWithoutReadmittingOrResettingTheEpoch()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"idempotent-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;

        var peerRid = RoutingId.From($"idempotent-peer-{suffix}");
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(peerRid);
        peer.Connect(endpoint);
        byte[] Descriptor(ServiceWireConstants.Command command) =>
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                command,
                MeshName,
                $"inproc://idempotent-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: 3,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
            );

        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello));
        using var initialAdmission = await RawMeshPeerAdmission.CompleteAsync(
            peer,
            MeshName,
            $"inproc://idempotent-peer-{suffix}",
            7,
            3
        );
        await WaitUntilAsync(() => node.Status().AdmittedPeerCount == 1);
        var admitted = Assert.Single(node.Peers());
        Assert.Equal(1UL, monitor.Status().PeerAdmitted);

        // The same descriptor again, first as Admit then as Hello: both are
        // idempotent completions. The Hello still gets its Admit reply; the
        // peer is neither re-admitted nor moved to a new epoch.
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Admit));
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello));
        await ReceiveAdmitAsync(peer);

        var status = monitor.Status();
        Assert.Equal(1UL, status.PeerAdmitted);
        Assert.Equal(0UL, status.PeerRejected);
        Assert.Equal(0UL, status.ProtocolErrors);
        Assert.Equal(1U, node.Status().AdmittedPeerCount);
        var current = Assert.Single(node.Peers());
        Assert.Equal(MeshPeerState.Admitted, current.State);
        Assert.Equal(admitted.LifecycleGeneration, current.LifecycleGeneration);
        Assert.Equal(admitted.DescriptorRevision, current.DescriptorRevision);
        Assert.Equal(admitted.LastChangedMs, current.LastChangedMs);
    }

    [Fact]
    public async Task SameConnectionHello_WithoutPendingCandidate_PreservesEpochAndLivenessDeadline()
    {
        DeferredReadyMonitor? transportMonitor = null;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            decorateSocketMonitor: monitor => transportMonitor = new DeferredReadyMonitor(monitor)
        );
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"same-connection-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(RoutingId.From($"same-connection-peer-{suffix}"));

        transportMonitor!.ReleaseReady();
        peer.Connect(endpoint);
        await transportMonitor.ReadyCaptured;
        await transportMonitor.ReadyApplied;

        byte[] Hello() =>
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Hello,
                MeshName,
                $"inproc://same-connection-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: 3,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
            );

        await SendAsync(peer, Hello());
        using var initialAdmission = await RawMeshPeerAdmission.CompleteAsync(
            peer,
            MeshName,
            $"inproc://same-connection-peer-{suffix}",
            7,
            3
        );
        await WaitUntilAsync(() => node.Status().AdmittedPeerCount == 1);
        var admitted = Assert.Single(node.Peers());

        // The first Hello consumed the only READY candidate. The same physical
        // connection has no pending replacement candidate, so this Hello keeps
        // both the connection epoch and its liveness deadline.
        await SendAsync(peer, Hello());
        await ReceiveAdmitAsync(peer);

        Assert.Equal(1UL, monitor.Status().PeerAdmitted);
        Assert.Equal(0UL, monitor.Status().PeerRejected);
        Assert.Equal(0UL, monitor.Status().ProtocolErrors);
        var current = Assert.Single(node.Peers());
        Assert.Equal(admitted.LifecycleGeneration, current.LifecycleGeneration);
        Assert.Equal(admitted.DescriptorRevision, current.DescriptorRevision);
        Assert.Equal(admitted.LastChangedMs, current.LastChangedMs);
    }

    [Fact]
    public async Task AdmittedPeer_SurvivesConnectionReadyDeliveredAfterHello()
    {
        var scheduler = new GatedTaskScheduler();
        DeferredReadyMonitor? transportMonitor = null;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(
            context,
            MeshName,
            routedSubmitScheduler: scheduler,
            decorateSocketMonitor: monitor => transportMonitor = new DeferredReadyMonitor(monitor)
        );
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"late-ready-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(RoutingId.From($"late-ready-peer-{suffix}"));
        peer.Connect(endpoint);

        try
        {
            using (
                var hello = Message.From(
                    ZLinkServiceWireCodec.EncodeRouteAdmission(
                        ServiceWireConstants.Command.Hello,
                        MeshName,
                        $"inproc://late-ready-peer-{suffix}",
                        lifecycleGeneration: 7,
                        descriptorRevision: 3,
                        new Dictionary<string, uint>(StringComparer.Ordinal),
                        objectRole: (byte)ZLinkMeshNodeObjectRole.Server
                    )
                )
            )
                peer.Send().Message(hello).Submit();

            // Complete admission before releasing the retained transport observation.
            await scheduler.Queued.WaitAsync(TimeSpan.FromSeconds(2));
            await scheduler.ReleaseNextAsync();
            using var initialAdmission = await RawMeshPeerAdmission.CompleteAsync(
                peer,
                MeshName,
                $"inproc://late-ready-peer-{suffix}",
                7,
                3
            );
            await WaitUntilAsync(() => node.Status().AdmittedPeerCount == 1);
            var pending = Assert.Single(node.Peers());
            await transportMonitor!.ReadyCaptured;
            transportMonitor.ReleaseReady();
            // Releasing the fixture's retained event must notify the owner;
            // the real monitor source was drained when the event was captured.
            node.SignalReceiveOwner();
            await transportMonitor.ReadyApplied;
            var afterReady = Assert.Single(node.Peers());
            Assert.Equal(pending.LifecycleGeneration, afterReady.LifecycleGeneration);
            Assert.Equal(pending.DescriptorRevision, afterReady.DescriptorRevision);
            Assert.Equal(pending.LastChangedMs, afterReady.LastChangedMs);
            Assert.Equal(1U, node.Status().AdmittedPeerCount);

            // The descriptor UPDATE follows the completed admission on the same route.
            // Await the released scheduler batch before reading its socket output.
            node.PublishDraining();
            await scheduler.ReleaseAsync();
            Assert.Equal(
                ServiceWireConstants.Command.Update,
                await ReceiveAdmissionCommandAsync(peer)
            );

            Assert.True(
                SpinWait.SpinUntil(
                    () => node.Status().AdmittedPeerCount == 1,
                    TimeSpan.FromSeconds(5)
                )
            );
            Assert.Equal(1UL, monitor.Status().PeerAdmitted);
            Assert.Equal(0UL, monitor.Status().PeerRejected);
            Assert.Equal(0UL, monitor.Status().ProtocolErrors);
            var current = Assert.Single(node.Peers());
            Assert.Equal(pending.LifecycleGeneration, current.LifecycleGeneration);
            Assert.Equal(pending.DescriptorRevision, current.DescriptorRevision);
        }
        finally
        {
            transportMonitor?.ReleaseReady();
            scheduler.Release();
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task InboundHello_FollowsTheHostShutdownSeal(bool sealedForShutdown)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var gate = new ZLinkDrainAdmissionGate();
        var sealObserved = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        node.SetPeerAdmissionSealGate(() =>
        {
            sealObserved.TrySetResult();
            return gate.IsSealedForShutdown;
        });
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"inbound-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        if (sealedForShutdown)
        {
            gate.ClaimShutdown();
            node.PublishDraining();
        }
        var state = node.Status().State;
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(RoutingId.From($"inbound-peer-{suffix}"));
        peer.Connect(endpoint);

        await SendAsync(
            peer,
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                ServiceWireConstants.Command.Hello,
                MeshName,
                $"inproc://inbound-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: 3,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
            )
        );
        // The inbound admission turn observes the shutdown gate.
        await sealObserved.Task.WaitAsync(TimeSpan.FromSeconds(2));

        if (sealedForShutdown)
        {
            Assert.Equal(state, node.Status().State);
            Assert.Equal(0U, node.Status().AdmittedPeerCount);
            Assert.Empty(node.Peers());
            Assert.Equal(0UL, monitor.Status().PeerAdmitted);
            Assert.Equal(0UL, monitor.Status().PeerRejected);
            await AssertNoAdmitAsync(peer);
        }
        else
        {
            using var initialAdmission = await RawMeshPeerAdmission.CompleteAsync(
                peer,
                MeshName,
                $"inproc://inbound-peer-{suffix}",
                7,
                3
            );
            await WaitUntilAsync(() => node.Status().AdmittedPeerCount == 1);
            Assert.Equal(1U, node.Status().AdmittedPeerCount);
            Assert.Equal(MeshPeerState.Admitted, Assert.Single(node.Peers()).State);
            Assert.Equal(1UL, monitor.Status().PeerAdmitted);
        }
    }

    // Transport liveness §5: local ADMIT submission does not establish send readiness.
    [Fact]
    public async Task InboundHello_CountsOnlyPeersWhoseRemoteAdmitArrived()
    {
        const int peerCount = 32;
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"admit-order-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        var peers = new List<IDealerSocket>();
        try
        {
            for (var index = 0; index < peerCount; index++)
            {
                var peer = context.CreateDealerSocket();
                peers.Add(peer);
                peer.SetRoutingId(RoutingId.From($"admit-order-peer-{index}-{suffix}"));
                peer.Connect(endpoint);
            }
            await Task.WhenAll(
                peers.Select(
                    (peer, index) =>
                        SendAsync(
                            peer,
                            ZLinkServiceWireCodec.EncodeRouteAdmission(
                                ServiceWireConstants.Command.Hello,
                                MeshName,
                                $"inproc://admit-order-peer-{index}-{suffix}",
                                lifecycleGeneration: 7,
                                descriptorRevision: 3,
                                new Dictionary<string, uint>(StringComparer.Ordinal),
                                objectRole: (byte)ZLinkMeshNodeObjectRole.Server
                            )
                        )
                )
            );
            for (var received = 1; received <= peerCount; received++)
            {
                Assert.Equal((uint)(received - 1), node.Status().AdmittedPeerCount);
                using var initialAdmission = await RawMeshPeerAdmission.CompleteAsync(
                    peers[received - 1],
                    MeshName,
                    $"inproc://admit-order-peer-{received - 1}-{suffix}",
                    7,
                    3
                );
                await WaitUntilAsync(() => node.Status().AdmittedPeerCount == (uint)received);
                Assert.Equal((uint)received, node.Status().AdmittedPeerCount);
            }
        }
        finally
        {
            foreach (var peer in peers)
                peer.Dispose();
        }
    }

    [Fact]
    public async Task SealedNode_IgnoresRepeatedHello_ButProcessesAdmittedPeerUpdate()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var gate = new ZLinkDrainAdmissionGate();
        node.SetPeerAdmissionSealGate(() => gate.IsSealedForShutdown);
        var suffix = Guid.NewGuid().ToString("N");
        node.SetRoutingId(RoutingId.From($"update-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);
        using var monitor = node.OpenMonitor();
        node.Start();
        var endpoint = node.Status().LocalEndpoint;
        using var peer = context.CreateDealerSocket();
        peer.SetRoutingId(RoutingId.From($"update-peer-{suffix}"));
        peer.Connect(endpoint);
        byte[] Descriptor(ServiceWireConstants.Command command, ulong revision, byte state) =>
            ZLinkServiceWireCodec.EncodeRouteAdmission(
                command,
                MeshName,
                $"inproc://update-peer-{suffix}",
                lifecycleGeneration: 7,
                descriptorRevision: revision,
                new Dictionary<string, uint>(StringComparer.Ordinal),
                objectRole: (byte)ZLinkMeshNodeObjectRole.Server,
                runtimeState: state
            );

        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello, 3, 1));
        using var initialAdmission = await RawMeshPeerAdmission.CompleteAsync(
            peer,
            MeshName,
            $"inproc://update-peer-{suffix}",
            7,
            3
        );
        await WaitUntilAsync(() => node.Status().AdmittedPeerCount == 1);
        Assert.Equal(1UL, monitor.Status().PeerAdmitted);
        gate.ClaimShutdown();
        node.PublishDraining();

        // If the sealed Hello mutates the descriptor to revision 99, the
        // following revision-4 Update cannot be accepted as the next revision.
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Hello, 99, 1));
        await SendAsync(peer, Descriptor(ServiceWireConstants.Command.Update, 4, 2));
        await WaitUntilAsync(() =>
            node.Peers()
                .Any(remote =>
                    remote.DescriptorRevision == 4 && remote.State == MeshPeerState.Draining
                )
        );

        Assert.Equal(1U, node.Status().AdmittedPeerCount);
        Assert.Equal(MeshNodeState.Draining, node.Status().State);
        Assert.Equal(0UL, monitor.Status().PeerRejected);
        Assert.Equal(0UL, monitor.Status().ProtocolErrors);
        await AssertNoAdmitAsync(peer);
    }

    [Fact]
    public async Task SealedNode_StartsNoPeerAdmissionAfterPeerLoss_AndKeepsDraining()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var gate = new ZLinkDrainAdmissionGate();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"sealed-peer-{suffix}");
        string peerEndpoint;
        node.SetPeerAdmissionSealGate(() => gate.IsSealedForShutdown);
        node.SetRoutingId(RoutingId.From($"sealed-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);

        await using (var peer = StartPeer(context, peerRid, EphemeralTcpEndpoint))
        {
            peerEndpoint = peer.Status().LocalEndpoint;
            node.ConnectPeer(peerEndpoint, peerRid);
            node.Start();
            await WaitUntilAsync(() =>
                node.Status().AdmittedPeerCount == 1 && peer.Status().AdmittedPeerCount == 1
            );

            // ZLinkFrameworkRuntime shutdown order: seal host admission, then
            // publish Draining. The admitted peer still receives that Update.
            gate.ClaimShutdown();
            node.PublishDraining();
            await WaitUntilAsync(() =>
                peer.Peers()
                    .Any(remote =>
                        remote.RoutingId == node.RoutingId && remote.State == MeshPeerState.Draining
                    )
            );
        }

        // Peer loss demotes the outbound intent to a reconnecting epoch but
        // never moves the sealed node back before Draining.
        await WaitUntilAsync(() =>
            node.Status().AdmittedPeerCount == 0
            && node.Peers().All(remote => remote.State == MeshPeerState.Connecting)
        );
        Assert.Equal(MeshNodeState.Draining, node.Status().State);

        // The peer restarts at the same endpoint. Core reconnects the pipe,
        // but the sealed node submits no Hello, so the restarted peer never
        // learns about it.
        await using (var restarted = StartPeer(context, peerRid, peerEndpoint))
        {
            await Task.Delay(TimeSpan.FromSeconds(2));
            Assert.Equal(0U, restarted.Status().AdmittedPeerCount);
            Assert.Empty(restarted.Peers());
            Assert.Equal(0U, node.Status().AdmittedPeerCount);
            Assert.Equal(MeshNodeState.Draining, node.Status().State);
            Assert.Equal(MeshPeerState.Connecting, Assert.Single(node.Peers()).State);

            // Drain completes: the node stops without waiting on the withheld
            // admission.
            await node.DisposeAsync().AsTask().WaitAsync(TimeSpan.FromSeconds(10));
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task UnsealedNode_ReadmitsRestartedPeer(bool relocationDraining)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        var gate = new ZLinkDrainAdmissionGate();
        await using var node = new ZLinkManagedMeshNode(context, MeshName);
        var suffix = Guid.NewGuid().ToString("N");
        var peerRid = RoutingId.From($"unsealed-peer-{suffix}");
        string peerEndpoint;
        node.SetPeerAdmissionSealGate(() => gate.IsSealedForShutdown);
        node.SetRoutingId(RoutingId.From($"unsealed-node-{suffix}"));
        node.SetBind(EphemeralTcpEndpoint);
        node.AddChannel(MeshName);

        await using (var peer = StartPeer(context, peerRid, EphemeralTcpEndpoint))
        {
            peerEndpoint = peer.Status().LocalEndpoint;
            node.ConnectPeer(peerEndpoint, peerRid);
            node.Start();
            await WaitUntilAsync(() =>
                node.Status().AdmittedPeerCount == 1 && peer.Status().AdmittedPeerCount == 1
            );
            if (relocationDraining)
            {
                // Relocate publishes Draining without sealing the host: the
                // node keeps admitting peers.
                node.PublishDraining();
                await WaitUntilAsync(() =>
                    peer.Peers()
                        .Any(remote =>
                            remote.RoutingId == node.RoutingId
                            && remote.State == MeshPeerState.Draining
                        )
                );
            }
        }

        await WaitUntilAsync(() =>
            node.Status().AdmittedPeerCount == 0
            && node.Peers().All(remote => remote.State == MeshPeerState.Connecting)
        );

        await using var restarted = StartPeer(context, peerRid, peerEndpoint);
        await WaitUntilAsync(
            () => node.Status().AdmittedPeerCount == 1 && restarted.Status().AdmittedPeerCount == 1,
            TimeSpan.FromSeconds(15)
        );
        Assert.False(gate.IsSealedForShutdown);
        Assert.Equal(
            relocationDraining ? MeshNodeState.Draining : MeshNodeState.Ready,
            node.Status().State
        );
        Assert.Equal(
            relocationDraining ? MeshPeerState.Draining : MeshPeerState.Admitted,
            Assert.Single(restarted.Peers()).State
        );
    }

    private sealed class DeferredReadyMonitor(ISocketMonitor inner) : ISocketMonitor
    {
        private readonly Queue<MonitorEvent> _ready = new();
        private readonly TaskCompletionSource _captured = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private readonly TaskCompletionSource _applied = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _release;
        private bool _delivered;

        internal Task ReadyCaptured => _captured.Task;
        internal Task ReadyApplied => _applied.Task;

        internal void ReleaseReady()
        {
            Volatile.Write(ref _release, 1);
        }

        public MonitorEvent? Recv(RecvFlags flags = RecvFlags.None)
        {
            // The owner calls Recv again only after handling the prior event.
            if (_delivered)
            {
                _delivered = false;
                _applied.TrySetResult();
            }
            if (Volatile.Read(ref _release) != 0 && _ready.TryDequeue(out var ready))
            {
                _delivered = true;
                return ready;
            }
            while (inner.Recv(flags) is { } value)
            {
                if (
                    value.Event == MonitorEventType.ConnectionReady
                    && (value.Flags & MonitorEventFlags.ConnectionReadyEdge) != 0
                )
                {
                    _ready.Enqueue(value);
                    _captured.TrySetResult();
                    if (Volatile.Read(ref _release) != 0)
                    {
                        _delivered = true;
                        return _ready.Dequeue();
                    }
                    continue;
                }
                return value;
            }
            return null;
        }

        public MonitorStatus Status() => inner.Status();

        public void Close() => inner.Close();

        public void Dispose() => inner.Dispose();

        public ValueTask DisposeAsync() => inner.DisposeAsync();
    }

    private static ZLinkManagedMeshNode StartPeer(
        IContext context,
        RoutingId routingId,
        string endpoint
    )
    {
        var peer = new ZLinkManagedMeshNode(context, MeshName);
        peer.SetRoutingId(routingId);
        peer.SetBind(endpoint);
        peer.AddChannel(MeshName);
        peer.Start();
        return peer;
    }

    private static async Task SendAsync(IDealerSocket socket, byte[] head)
    {
        var deadlineTimeout = TimeSpan.FromSeconds(2);
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (true)
        {
            try
            {
                using var message = Message.From(head);
                await socket.Send().Message(message).Async(CancellationToken.None).Admitted;
                return;
            }
            catch (ZlinkSubmitException)
                when (Stopwatch.GetElapsedTime(deadlineStarted) < deadlineTimeout)
            {
                await Task.Delay(10);
            }
        }
    }

    // Skips liveness probes; returns once a route Admit arrives.
    private static async Task ReceiveAdmitAsync(IDealerSocket socket)
    {
        while (await ReceiveAdmissionCommandAsync(socket) != ServiceWireConstants.Command.Admit) { }
    }

    // Skips records that are not route admission records (liveness probes) and returns
    // the next admission command.
    private static async Task<ServiceWireConstants.Command> ReceiveAdmissionCommandAsync(
        IDealerSocket socket
    )
    {
        var deadlineTimeout = TimeSpan.FromSeconds(2);
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (Stopwatch.GetElapsedTime(deadlineStarted) < deadlineTimeout)
        {
            using var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
            {
                if (
                    ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                        received.FirstPart().AsSpan(),
                        out var command,
                        out _,
                        out _
                    )
                )
                    return command;
                continue;
            }
            await Task.Delay(10);
        }
        throw new TimeoutException("No route admission record was received.");
    }

    private static async Task AssertNoAdmitAsync(IDealerSocket socket)
    {
        var started = Stopwatch.GetTimestamp();
        while (Stopwatch.GetElapsedTime(started) < TimeSpan.FromMilliseconds(200))
        {
            using var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
            {
                if (
                    ZLinkServiceWireCodec.TryDecodeRouteAdmission(
                        received.FirstPart().AsSpan(),
                        out var command,
                        out _,
                        out _
                    )
                )
                    Assert.NotEqual(ServiceWireConstants.Command.Admit, command);
                continue;
            }
            await Task.Delay(10);
        }
    }

    private static async Task WaitUntilAsync(Func<bool> condition, TimeSpan? timeout = null)
    {
        var deadlineTimeout = timeout ?? TimeSpan.FromSeconds(5);
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (!condition())
        {
            if (Stopwatch.GetElapsedTime(deadlineStarted) >= deadlineTimeout)
                throw new TimeoutException("The managed MeshNode condition was not reached.");
            await Task.Delay(10);
        }
    }
}
