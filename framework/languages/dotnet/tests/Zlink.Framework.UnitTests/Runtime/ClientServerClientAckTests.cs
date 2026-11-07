using System.Diagnostics.Tracing;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed class ClientServerClientAckTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Fact]
    public async Task Runtime_disposal_cancels_the_pending_probe_before_closing_its_deadline_index()
    {
        await using var fixture = new AckFixture(useOuterRuntime: true);
        Task probe = null!;
        await fixture.Lane.RunAsync(() =>
        {
            using (ExecutionContext.SuppressFlow())
                probe = fixture.Connection.RequestLivenessProbeAsync(
                    47UL,
                    1UL,
                    new ZLinkServiceLiveness(TimeProvider.System.GetTimestamp()),
                    fixture.AdmissionStop.Token
                );
            fixture.Connection.RegisterRequestTask(probe);
        });
        using var received = Received.Create();
        Assert.True(fixture.Router.Recv(received));
        Assert.NotNull(received.ReplyToken);
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(received.Parts, out var probeId)
        );
        Assert.Equal(47UL, probeId);
        Assert.False(probe.IsCompleted);
        output.WriteLine("Native ROUTER received probe 47 with ReplyToken; reply withheld.");

        await fixture.OuterRuntime!.DisposeAsync();

        Assert.True(probe.IsCompletedSuccessfully);
        Assert.True(fixture.AdmissionStop.IsCancellationRequested);
        Assert.True(fixture.ReceiveStop.IsCancellationRequested);
        Assert.Equal(1, fixture.Dealer.Closes);
        Assert.Empty(fixture.Failures.Failures);
        output.WriteLine("Outer disposal and probe completed; DEALER close count: 1.");
    }

    [Fact]
    public async Task Pending_ack_admission_allows_the_next_receive_turn()
    {
        await using var fixture = new AckFixture();
        fixture.FillSendHwm();
        using (var probe = ZLinkClientServerControlProtocol.EncodeLivenessProbe(17))
            fixture.Router.Send(fixture.Rid).Message(probe).Submit();
        fixture.WaitForControlFrame();
        var checkpoint = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        using var waits = new BlockingWaitListener(checkpoint);
        var poller = new TurnPoller(fixture.ReceiveStop, checkpoint, waits);
        fixture.Connection._receivePoller = poller;
        var ended = fixture.Connection.RunControlLoop().AsTask();
        try
        {
            // The checkpoint is either the next receive turn or the actual synchronous
            // Task wait. Neither elapsed time nor absence of an event decides the result.
            Assert.True(await checkpoint.Task, "ACK admission blocked the receive thread");
            Assert.Equal(1L, fixture.Connection.ReceivedLivenessProbeCount);
            Assert.Equal(SubmitResult.Backpressured, fixture.Dealer.LastSubmission.Result);
            Assert.False(fixture.Dealer.LastSubmission.Admitted.IsCompleted);
            Assert.Single(await fixture.Lane.RunAsync(() => fixture.Requests.ToArray()));
        }
        finally
        {
            await fixture.AdmissionStop.CancelAsync();
            await fixture.ReceiveStop.CancelAsync();
            await ended;
        }
    }

    [Fact]
    public async Task Blocking_wait_listener_detects_a_synchronous_wait_on_the_receive_role()
    {
        await using var source = ZLinkProcessExecutionPool.CreateReceiveSource();
        var checkpoint = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var waits = new BlockingWaitListener(checkpoint);
        var waiting = WaitOnReceiveRoleAsync();
        try
        {
            Assert.False(await checkpoint.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        }
        finally
        {
            release.TrySetResult();
            await waiting.WaitAsync(TimeSpan.FromSeconds(5));
        }

        async Task WaitOnReceiveRoleAsync()
        {
            await source.ResumeAsync();
            waits.ReceiveThread = Environment.CurrentManagedThreadId;
            release.Task.GetAwaiter().GetResult();
        }
    }

    [Fact]
    public async Task Immediate_success_submits_inline_without_an_observer()
    {
        await using var fixture = new AckFixture();
        var ack = ZLinkClientServerControlProtocol.EncodeLivenessAck(23);
        using var scheduling = new BlockingWaitListener(new TaskCompletionSource<bool>());
        scheduling.SubmitThread = Environment.CurrentManagedThreadId;
        fixture.Connection.StartControlAck(ack);
        scheduling.SubmitThread = 0;
        Assert.Equal(0, scheduling.Scheduled);
        Assert.Equal(1, fixture.Dealer.Submissions);
        Assert.True(fixture.Dealer.SubmittedOnLane);
        Assert.Empty(await fixture.Lane.RunAsync(() => fixture.Requests.ToArray()));
        Assert.Null(fixture.Connection._disposeTask);
        Assert.Empty(fixture.Failures.Failures);
        using var received = Received.Create();
        Assert.True(fixture.Router.Recv(received));
        Assert.True(
            ZLinkClientServerControlProtocol.TryDecodeLivenessAck(received.Parts, out var id)
        );
        Assert.Equal(23UL, id);
        using var duplicate = Received.Create();
        Assert.False(fixture.Router.Recv(duplicate, RecvFlags.DontWait));
        Assert.Throws<ObjectDisposedException>(() => ack.Size);
    }

    [Fact]
    public async Task Immediate_typed_failure_is_reported_once_outside_the_lane_and_starts_disposal()
    {
        await using var fixture = new AckFixture();
        await fixture.MakeReadyAsync();
        var expected = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        fixture.Dealer.Failure = expected;
        var ack = ZLinkClientServerControlProtocol.EncodeLivenessAck(29);
        Task[] observers = [];
        await fixture.Lane.RunAsync(() =>
        {
            // TryPost queues this after the current turn. A sentinel in the same FIFO
            // captures the observer before disposal can take the lane and close it.
            fixture.Connection.StartControlAck(ack);
            fixture.Lane.TryPost(() => observers = fixture.Requests.ToArray());
        });
        await Task.WhenAll(observers);
        await fixture.Disposal;
        fixture.AssertControlFailure(expected);
        Assert.Equal(1, fixture.Dealer.Submissions);
        Assert.Throws<ObjectDisposedException>(() => ack.Size);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Late_typed_failure_survives_disposal_before_observation(
        bool disposeBeforeObserver
    )
    {
        await using var fixture = new AckFixture();
        await fixture.MakeReadyAsync();
        var expected = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        var failedSend = Task.FromException(expected);
        var releaseObserver = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        Task observer = null!;
        await fixture.Lane.RunAsync(() =>
        {
            // Fix the failure before disposal, but hold the observer's scheduler entry
            // until disposal is published. Only Framework task observation is controlled;
            // no binding SendSubmission or private native completion is fabricated.
            var token = fixture.AdmissionStop.Token;
            using (ExecutionContext.SuppressFlow())
                observer = releaseObserver
                    .Task.ContinueWith(
                        _ => fixture.Connection.ObserveControlAckAsync(failedSend, token),
                        CancellationToken.None,
                        TaskContinuationOptions.None,
                        TaskScheduler.Default
                    )
                    .Unwrap();
            fixture.Connection.RegisterRequestTask(observer);
        });
        Task? disposal = null;
        try
        {
            if (disposeBeforeObserver)
            {
                disposal = ((IAsyncDisposable)fixture.Connection).DisposeAsync().AsTask();
                Assert.False(disposal.IsCompleted);
                Assert.False(observer.IsCompleted);
            }
            releaseObserver.SetResult();
            await observer;
            await (disposal ?? fixture.Disposal);
            fixture.AssertControlFailure(expected);
        }
        finally
        {
            releaseObserver.TrySetResult();
            await observer;
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Dispose_before_registration_rejects_the_ack_and_releases_its_message(
        bool laneAlreadyClosed
    )
    {
        await using var fixture = new AckFixture();
        var ack = ZLinkClientServerControlProtocol.EncodeLivenessAck(31);
        Task disposal = Task.CompletedTask;
        if (laneAlreadyClosed)
        {
            await ((IAsyncDisposable)fixture.Connection).DisposeAsync();
            fixture.Connection.StartControlAck(ack);
        }
        else
        {
            await fixture.Lane.RunAsync(() =>
            {
                fixture.Connection.StartControlAck(ack);
                disposal = ((IAsyncDisposable)fixture.Connection).DisposeAsync().AsTask();
            });
            await disposal;
        }
        Assert.Equal(0, fixture.Dealer.Submissions);
        Assert.Empty(fixture.Requests);
        Assert.Empty(fixture.Failures.Failures);
        Assert.Throws<ObjectDisposedException>(() => ack.Size);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Dispose_during_registration_or_pending_admission_joins_the_whole_observer(
        bool duringSubmit
    )
    {
        await using var fixture = new AckFixture();
        fixture.FillSendHwm();
        Task? disposal = null;
        if (duringSubmit)
            fixture.Dealer.BeforeSubmit = () =>
                disposal = ((IAsyncDisposable)fixture.Connection).DisposeAsync().AsTask();
        fixture.Connection.StartControlAck(ZLinkClientServerControlProtocol.EncodeLivenessAck(37));
        Assert.Single(fixture.Requests);
        disposal ??= ((IAsyncDisposable)fixture.Connection).DisposeAsync().AsTask();
        await disposal;
        Assert.All(fixture.Requests, task => Assert.True(task.IsCompletedSuccessfully));
        Assert.True(fixture.AdmissionStop.IsCancellationRequested);
        Assert.True(fixture.ReceiveStop.IsCancellationRequested);
        Assert.Equal("configured", fixture.Connection._diagnostics);
        Assert.Empty(fixture.Failures.Failures);
    }

    [Fact]
    public async Task Disposal_joins_every_registered_callback_before_closing_the_lane()
    {
        await using var fixture = new AckFixture();
        var first = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var second = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task[] observers = [];
        await fixture.Lane.RunAsync(() =>
        {
            observers = new[] { first, second }
                .Select(source =>
                {
                    return fixture.RegisterObserver(source.Task);
                })
                .ToArray();
        });
        var disposal = ((IAsyncDisposable)fixture.Connection).DisposeAsync().AsTask();
        try
        {
            first.SetResult();
            await observers[0];
            Assert.False(disposal.IsCompleted);
            Assert.False(observers[1].IsCompleted);
            await fixture.Lane.RunAsync(() => Assert.Equal(2, fixture.Requests.Count));
        }
        finally
        {
            first.TrySetResult();
            second.TrySetResult();
            await Task.WhenAll(observers);
            await disposal;
        }
        Assert.Empty(fixture.Failures.Failures);
    }

    [Fact]
    public async Task Failure_reporting_remains_inside_the_registered_observer_lifetime()
    {
        using var release = new ManualResetEventSlim();
        var reporting = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        await using var fixture = new AckFixture(() =>
        {
            reporting.TrySetResult();
            release.Wait();
        });
        await fixture.MakeReadyAsync();
        var expected = new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected);
        fixture.Dealer.Failure = expected;
        fixture.Connection.StartControlAck(ZLinkClientServerControlProtocol.EncodeLivenessAck(41));
        Task[] registered = [];
        Task? disposal = null;
        try
        {
            await reporting.Task;
            registered = await fixture.Lane.RunAsync(() => fixture.Requests.ToArray());
            // The send has already faulted and the state transition has run, but the
            // registered task must still include reporting and shutdown publication.
            Assert.False(Assert.Single(registered).IsCompleted);
            disposal = ((IAsyncDisposable)fixture.Connection).DisposeAsync().AsTask();
            Assert.False(disposal.IsCompleted);
            Assert.Equal(0, fixture.Dealer.Closes);
        }
        finally
        {
            release.Set();
            await Task.WhenAll(registered);
            await (disposal ?? fixture.Disposal);
        }
        fixture.AssertControlFailure(expected);
    }

    [Fact]
    public async Task Probe_with_a_reply_token_keeps_the_raw_reply_terminal()
    {
        await using var fixture = new AckFixture();
        using var probe = ZLinkClientServerControlProtocol.EncodeLivenessProbe(43);
        // Core typed requests target ROUTER, so exercise the unchanged raw reply
        // helper with a public DEALER -> ROUTER request envelope.
        var request = fixture.Dealer.Request().Message(probe).Async();
        using var received = Received.Create();
        Assert.True(fixture.Router.Recv(received));
        Assert.NotNull(received.ReplyToken);
        ZLinkClientServerClientRuntime.Connection.ReplyOwned(
            received,
            ZLinkClientServerControlProtocol.EncodeLivenessAck(43)
        );
        while (!request.Reply.IsCompleted)
            fixture.DrainCompletion();
        var reply = await request.Reply;
        try
        {
            Assert.True(ZLinkClientServerControlProtocol.TryDecodeLivenessAck(reply, out var id));
            Assert.Equal(43UL, id);
            Assert.Equal(0, fixture.Dealer.Submissions);
            Assert.Empty(fixture.Requests);
            Assert.Empty(fixture.Failures.Failures);
        }
        finally
        {
            foreach (var part in reply)
                part.Dispose();
        }
    }

    private sealed class GatedReporter(AuditRuntimeFailureReporter inner, Action beforeReport)
        : Zlink.Framework.Runtime.Diagnostics.IZLinkRuntimeFailureReporter
    {
        public void ReportHandlerException(Exception error) => inner.ReportHandlerException(error);

        public void ReportUnhandledCallbackException(Exception error) =>
            inner.ReportUnhandledCallbackException(error);

        public void ReportRuntimeTaskException(string name, Exception error)
        {
            beforeReport();
            inner.ReportRuntimeTaskException(name, error);
        }
    }

    private sealed class TurnPoller(
        CancellationTokenSource stop,
        TaskCompletionSource<bool> checkpoint,
        BlockingWaitListener waits
    ) : IZLinkBackendSocketPoller
    {
        private int _turn;

        private readonly ZLinkProcessExecutionPool.ReceiveSource _source =
            ZLinkProcessExecutionPool.CreateReceiveSource();

        public async ValueTask<ZLinkBackendSocketReadiness> WaitAsync(
            TimeSpan timeout,
            CancellationToken cancellationToken
        )
        {
            await _source.ResumeAsync().ConfigureAwait(false);
            waits.ReceiveThread = Environment.CurrentManagedThreadId;
            if (++_turn == 2)
            {
                stop.Cancel();
                checkpoint.TrySetResult(true);
                return ZLinkBackendSocketReadiness.None;
            }
            return ZLinkBackendSocketReadiness.Readable;
        }

        public ValueTask ResumeAsync() => _source.ResumeAsync();

        public ValueTask DisposeAsync() => _source.DisposeAsync();
    }

    private sealed class BlockingWaitListener : EventListener
    {
        private readonly TaskCompletionSource<bool> _checkpoint;
        internal int ReceiveThread;
        internal int SubmitThread;
        internal int Scheduled;

        internal BlockingWaitListener(TaskCompletionSource<bool> checkpoint) =>
            _checkpoint = checkpoint;

        protected override void OnEventSourceCreated(EventSource source)
        {
            if (source.Name == "System.Threading.Tasks.TplEventSource")
                EnableEvents(source, EventLevel.Verbose, EventKeywords.All);
        }

        protected override void OnEventWritten(EventWrittenEventArgs data)
        {
            if (
                data.EventName == "TaskScheduled"
                && Environment.CurrentManagedThreadId == SubmitThread
            )
                Scheduled++;
            if (
                data.EventName == "TaskWaitBegin"
                && Environment.CurrentManagedThreadId == ReceiveThread
                && data.Payload is { Count: > 3 }
                && Convert.ToInt32(data.Payload[3]) == 1
            )
                _checkpoint.TrySetResult(false);
        }
    }

    private sealed class AckFixture : IAsyncDisposable
    {
        private readonly ZLinkStateLane _deadlineOwner = new();
        private readonly Zlink.Framework.Runtime.Messaging.ZLinkRequestDeadlineIndex _deadlines;
        private readonly IContext _context = Systems.Zlink.Zlink.CreateContext();
        private readonly IPoller _completions = Systems.Zlink.Zlink.CreatePoller();
        private readonly IDealerSocket _controlSocket;
        internal readonly IRouterSocket Router;
        internal readonly AckDealer Dealer;
        internal readonly RoutingId Rid = RoutingId.From("ack-client");
        internal readonly ZLinkClientServerClientRuntime.Connection Connection;
        internal readonly CancellationTokenSource AdmissionStop = new();
        internal readonly CancellationTokenSource ReceiveStop;
        internal readonly AuditRuntimeFailureReporter Failures = new();
        internal readonly ZLinkStateLane Lane;
        internal readonly List<Task> Requests;
        internal readonly ZLinkClientServerClientRuntime? OuterRuntime;

        internal AckFixture(Action? beforeReport = null, bool useOuterRuntime = false)
        {
            _context.Options.AutoHwmEnabled = false;
            Router = _context.CreateRouterSocket();
            var dealer = _context.CreateDealerSocket();
            _controlSocket = dealer;
            dealer.Options.SendHighWaterMark = 1;
            Router.Options.ReceiveHighWaterMark = 1;
            dealer.Options.Linger = TimeSpan.Zero;
            Router.Options.Linger = TimeSpan.Zero;
            dealer.SetRoutingId(Rid);
            var endpoint = $"inproc://client-ack-{Guid.NewGuid():N}";
            Router.Bind(endpoint);
            dealer.Connect(endpoint);
            Dealer = new AckDealer(dealer);
            _completions.Add(dealer, PollEventFlags.PollCompletion, 1);
            using (var ready = Message.From("ready"))
                dealer.Send().Message(ready).Submit();
            using (var received = Received.Create())
                Assert.True(Router.Recv(received));
            if (useOuterRuntime)
                OuterRuntime = new(
                    "work",
                    null!,
                    null!,
                    null!,
                    CancellationToken.None,
                    Failures,
                    new ZLinkRuntimeTaskRunner(Failures, CancellationToken.None)
                );
            _deadlines = OuterRuntime is null
                ? new(_deadlineOwner)
                : OuterRuntime._requestDeadlines;
            Connection = new(
                "work",
                endpoint,
                null,
                Dealer,
                CancellationToken.None,
                (_, _) => { },
                _ => { },
                TimeProvider.System,
                beforeReport is null ? Failures : new GatedReporter(Failures, beforeReport),
                new ZLinkRuntimeTaskRunner(Failures, CancellationToken.None),
                _deadlines
            );
            Connection._admissionStop = AdmissionStop;
            Lane = Connection._lane;
            Requests = Connection._requestTasks;
            ReceiveStop = Connection._receiveStop;
            if (OuterRuntime is not null)
                OuterRuntime._connections.Add(endpoint, Connection);
        }

        internal void WaitForControlFrame()
        {
            _completions.Modify(
                _controlSocket,
                PollEventFlags.PollIn | PollEventFlags.PollCompletion
            );
            try
            {
                var ready = new PollEvent[1];
                Assert.Equal(1, _completions.Wait(ready, Timeout.InfiniteTimeSpan));
                Assert.NotEqual(PollEventFlags.None, ready[0].Revents & PollEventFlags.PollIn);
            }
            finally
            {
                _completions.Modify(_controlSocket, PollEventFlags.PollCompletion);
            }
        }

        internal void DrainCompletion() =>
            _completions.Wait(new PollEvent[1], Timeout.InfiniteTimeSpan);

        internal Task RegisterObserver(Task send)
        {
            Task observer;
            using (ExecutionContext.SuppressFlow())
                observer = Connection.ObserveControlAckAsync(send, AdmissionStop.Token);
            Connection.RegisterRequestTask(observer);
            return observer;
        }

        internal Task Disposal => Connection._disposeTask!;

        internal async Task MakeReadyAsync()
        {
            await Lane.RunAsync(() =>
            {
                Connection._admissionCompleted = true;
                Connection._admittedDescriptor = new ZLinkClientServerControlProtocol.Admission(
                    "work",
                    RoutingId.From("server"),
                    1,
                    1,
                    100,
                    ZLinkFrameworkRuntimeState.Serving,
                    "plaintext",
                    4096,
                    "inproc://fixture"
                );
                Connection._ready = true;
                Connection._weight = 1;
                Connection.PublishReadyTargetUnderLock();
            });
            Assert.NotNull(Connection.ReadyTarget);
        }

        internal void AssertControlFailure(Exception expected)
        {
            Assert.Same(expected, Assert.Single(Failures.Failures));
            Assert.Equal(false, Connection._ready);
            Assert.Null(Connection.ReadyTarget);
            Assert.Equal(
                $"control:{expected.GetType().Name}:{expected.Message}",
                Connection._diagnostics
            );
            Assert.True(Disposal.IsCompletedSuccessfully);
            Assert.Equal(1, Dealer.Closes);
        }

        internal void FillSendHwm()
        {
            for (var index = 0; index < 32; index++)
            {
                using var filler = Message.From("filler");
                var result = Dealer.Send().Message(filler).Async(AdmissionStop.Token);
                if (result.Result == SubmitResult.Backpressured)
                {
                    Assert.False(result.Admitted.IsCompleted);
                    return;
                }
            }
            Assert.Fail("The native send HWM did not produce pending admission");
        }

        public async ValueTask DisposeAsync()
        {
            if (OuterRuntime is not null)
            {
                // The failing regression must still release the public native owners.
                try
                {
                    await OuterRuntime.DisposeAsync();
                }
                finally
                {
                    if (Dealer.Closes == 0)
                        await Dealer.DisposeAsync();
                    await OuterRuntime._lane.DisposeAsync();
                    await _deadlineOwner.DisposeAsync();
                    _completions.Dispose();
                    Router.Dispose();
                    _context.Dispose();
                }
                return;
            }
            var close = ((IAsyncDisposable)Connection).DisposeAsync();
            await _deadlines.DisposeAsync();
            await close;
            await _deadlineOwner.DisposeAsync();
            _completions.Dispose();
            Router.Dispose();
            _context.Dispose();
        }
    }

    private sealed class AckDealer(IDealerSocket inner)
        : PressureSocket<IDealerSocket>(inner),
            IDealerSocket
    {
        public new DealerSocketOptions Options => inner.Options;

        public void SetRoutingId(RoutingId rid) => inner.SetRoutingId(rid);

        public RoutingId GetRoutingId() => inner.GetRoutingId();

        public RequestOperation Request() => inner.Request();

        public SendOperation Send() => new CapturedSend(inner.Send(), this);

        internal SendSubmission LastSubmission;
        internal int Submissions;
        internal bool SubmittedOnLane;
        internal Exception? Failure;
        internal Action? BeforeSubmit;
        internal int Closes;

        public new ValueTask DisposeAsync()
        {
            Closes++;
            return inner.DisposeAsync();
        }

        private sealed class CapturedSend(SendOperation inner, AckDealer owner)
            : SendOperation,
                SendSubmitOperation
        {
            private SendSubmitOperation _submit = null!;

            public SendSubmitOperation Message(Message message)
            {
                _submit = _submit is null ? inner.Message(message) : _submit.Message(message);
                return this;
            }

            public void Submit() => throw new NotSupportedException();

            public SendSubmission Async(CancellationToken cancellationToken = default)
            {
                owner.Submissions++;
                owner.SubmittedOnLane = ZLinkStateLane.Current is not null;
                owner.BeforeSubmit?.Invoke();
                if (owner.Failure is { } failure)
                    throw failure;
                return owner.LastSubmission = _submit.Async(cancellationToken);
            }
        }
    }
}
