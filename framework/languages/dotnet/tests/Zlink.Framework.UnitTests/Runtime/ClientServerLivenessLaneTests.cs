using System.Reflection;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed class ClientServerLivenessLaneTests
{
    [Fact]
    public async Task Server_probe_admission_does_not_hold_the_receive_turn()
    {
        const int maximumFillerRecords = 32;
        using var context = Systems.Zlink.Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        router.Options.SendHighWaterMark = 1;
        dealer.Options.ReceiveHighWaterMark = 1;
        router.Options.Linger = TimeSpan.Zero;
        dealer.Options.Linger = TimeSpan.Zero;
        dealer.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
        var rid = RoutingId.From("blocked-probe-client");
        dealer.SetRoutingId(rid);
        var endpoint = $"inproc://blocked-server-probe-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using (var ready = Message.From("ready"))
            dealer.Send().Message(ready).Submit();
        using (var received = Received.Create())
            Assert.True(router.Recv(received));
        using var completions = new TestCompletionPollerDriver(router);
        using var cancellation = new CancellationTokenSource();
        var accepted = 0;
        SendSubmission pending = default;
        for (; accepted < maximumFillerRecords; accepted++)
        {
            using var filler = Message.From(new byte[] { 1 });
            var submitted = router.Send(rid).Message(filler).Async(cancellation.Token);
            if (submitted.Result == SubmitResult.Backpressured)
            {
                pending = submitted;
                break;
            }
            Assert.Equal(SubmitResult.Ok, submitted.Result);
        }
        Assert.Equal(SubmitResult.Backpressured, pending.Result);
        Assert.False(pending.Admitted.IsCompleted);
        var failures = new AuditRuntimeFailureReporter();
        var identity = new ZLinkClientServerServerIdentity(
            "work",
            RoutingId.From("probe-server"),
            1,
            "plaintext",
            1,
            4096,
            endpoint,
            failures
        );
        await identity.AdmitPeerAsync(rid, 4096, 1);
        var tick = identity.TickLivenessAsync(router, cancellation.Token);
        try
        {
            // No peer read can release HWM until the receive-turn assertion finishes.
            Assert.True(tick.IsCompletedSuccessfully);
            Assert.Equal(0, identity.LivenessProbeCount);
        }
        finally
        {
            for (var index = 0; index < accepted + 2; index++)
            {
                using var received = Received.Create();
                Assert.True(dealer.Recv(received));
                if (index == accepted + 1)
                    Assert.True(
                        ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(
                            received.Parts,
                            out _
                        )
                    );
            }
            await tick;
            await pending.Admitted;
            using var completionGuard = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            while (identity.LivenessProbeCount == 0)
            {
                completionGuard.Token.ThrowIfCancellationRequested();
                await Task.Yield();
            }
            await cancellation.CancelAsync();
        }
        Assert.Equal(1, identity.LivenessProbeCount);
        Assert.Empty(failures.Failures);
    }

    [Fact]
    public async Task Server_probe_failure_reaches_the_existing_runtime_failure_reporter()
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        var endpoint = $"inproc://failed-server-probe-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        var failures = new AuditRuntimeFailureReporter();
        var identity = new ZLinkClientServerServerIdentity(
            "work",
            RoutingId.From("probe-server"),
            1,
            "plaintext",
            1,
            4096,
            endpoint,
            failures
        );
        await identity.AdmitPeerAsync(RoutingId.From("missing-probe-client"), 4096, 1);

        await identity.TickLivenessAsync(router, CancellationToken.None);

        Assert.IsType<ZlinkSubmitException>(Assert.Single(failures.Failures));
        Assert.Equal(0, identity.LivenessProbeCount);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void Server_ack_without_a_reply_token_preserves_delivery_and_failure_reporting(
        bool disconnectBeforeAck
    )
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        dealer.Options.ReceiveTimeout = TimeSpan.FromSeconds(5);
        var rid = RoutingId.From("ack-client");
        dealer.SetRoutingId(rid);
        var endpoint = $"inproc://server-ack-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        const ulong probeId = 17;
        using (var probe = ZLinkClientServerControlProtocol.EncodeLivenessProbe(probeId))
            dealer.Send().Message(probe).Submit();
        using var received = Received.Create();
        Assert.True(router.Recv(received));
        Assert.Null(received.ReplyToken);
        if (disconnectBeforeAck)
            router.DisconnectRid(rid);
        var failures = new AuditRuntimeFailureReporter();
        var identity = new ZLinkClientServerServerIdentity(
            "work",
            RoutingId.From("ack-server"),
            1,
            "plaintext",
            1,
            4096,
            endpoint,
            failures
        );

        typeof(ZLinkChannelReceiveLoop)
            .GetMethod("ReplyClientServerControl", BindingFlags.Static | BindingFlags.NonPublic)!
            .Invoke(null, [router, received, identity, failures, CancellationToken.None]);

        Assert.Equal(1, identity.ReceivedLivenessProbeCount);
        if (disconnectBeforeAck)
            Assert.IsType<ZlinkSubmitException>(Assert.Single(failures.Failures));
        else
        {
            using var ack = Received.Create();
            Assert.True(dealer.Recv(ack));
            Assert.True(
                ZLinkClientServerControlProtocol.TryDecodeLivenessAck(ack.Parts, out var actual)
            );
            Assert.Equal(probeId, actual);
            Assert.Empty(failures.Failures);
        }
    }

    // State ownership §6 type ②: native completion before the first await is a valid
    // schedule. Exercise the Connection's actual admission and timer probe entry points.
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Completed_native_probe_reply_does_not_reenter_connection_lane(
        bool afterAdmission
    )
    {
        using var context = Systems.Zlink.Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        using var poller = Systems.Zlink.Zlink.CreatePoller();
        poller.Add(router, PollEventFlags.PollIn, 1);
        poller.Add(dealer, PollEventFlags.PollCompletion, 2);
        var endpoint = $"inproc://completed-liveness-{Guid.NewGuid():N}";
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        var barrier = new CompletedReplyDealer(dealer, router, poller);
        var time = new ControllableTimeProvider();
        var failures = new AuditRuntimeFailureReporter();
        using var stop = new CancellationTokenSource();
        var connectionType = typeof(ZLinkClientServerClientRuntime).GetNestedType(
            "Connection",
            BindingFlags.NonPublic
        )!;
        // Only Framework state is inspected. Binding calls, request/reply and completion
        // draining all use public APIs; no production hook or binding reflection is needed.
        var connection = Activator.CreateInstance(
            connectionType,
            BindingFlags.Instance | BindingFlags.NonPublic,
            binder: null,
            args:
            [
                "work",
                endpoint,
                null,
                barrier,
                stop.Token,
                (Action<object, string>)((_, _) => { }),
                (Action<bool>)(_ => { }),
                time,
                failures,
                new ZLinkRuntimeTaskRunner(failures, stop.Token),
            ],
            culture: null
        )!;
        FieldInfo Field(string name) =>
            connectionType.GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!;
        MethodInfo Method(string name) =>
            connectionType.GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!;
        var lane = (ZLinkStateLane)Field("_lane").GetValue(connection)!;
        var requests = (List<Task>)Field("_requestTasks").GetValue(connection)!;
        using var admissionStop = CancellationTokenSource.CreateLinkedTokenSource(stop.Token);
        Field("_admissionStop").SetValue(connection, admissionStop);
        const uint maximumMessageBytes = 4096;
        Field("_normalizedEffectiveMaxMessageBytes").SetValue(connection, maximumMessageBytes);
        var admission = new ZLinkClientServerControlProtocol.Admission(
            "work",
            RoutingId.From("probe-server"),
            1,
            1,
            100,
            ZLinkFrameworkRuntimeState.Serving,
            "plaintext",
            maximumMessageBytes,
            endpoint
        );
        try
        {
            using var reply = ZLinkClientServerControlProtocol.EncodeAdmission(admission);
            Assert.True(
                (bool)Method("ApplyAdmission").Invoke(connection, [new[] { reply }, 1UL, 0UL])!
            );
            var first = await lane.RunAsync(() => requests.ToArray());
            await Task.WhenAll(first);
            Assert.Equal(1L, Field("_livenessAckCount").GetValue(connection));
            if (afterAdmission)
            {
                // Advance only the monotonic clock, then run the timer's state transition.
                // No elapsed-time wait or scheduling race is needed to reach the next probe.
                time.AdvanceMonotonic(ZLinkServiceLiveness.ProbeInterval);
                await lane.RunAsync(() =>
                    Method("ProgressLivenessUnderState")
                        .Invoke(connection, [admissionStop.Token, TimeSpan.Zero])
                );
                var next = await lane.RunAsync(() => requests.ToArray());
                await Task.WhenAll(next);
                Assert.Equal(2L, Field("_livenessAckCount").GetValue(connection));
            }
            Assert.Equal(afterAdmission ? 2 : 1, barrier.CompletedReplies);
            Assert.Empty(failures.Failures);
        }
        finally
        {
            // Join even the faulted pre-fix request and cancel/join the liveness loop before
            // releasing native resources. Nothing closes a socket while a child still uses it.
            await admissionStop.CancelAsync();
            foreach (var request in await lane.RunAsync(() => requests.ToArray()))
            {
                try
                {
                    await request;
                }
                catch (Exception) when (request.IsFaulted || request.IsCanceled) { }
            }
            if (Field("_livenessTask").GetValue(connection) is Task liveness)
            {
                try
                {
                    await liveness;
                }
                catch (OperationCanceledException) when (admissionStop.IsCancellationRequested) { }
            }
            await lane.DisposeAsync();
        }
    }

    private sealed class CompletedReplyDealer(
        IDealerSocket inner,
        IRouterSocket router,
        IPoller poller
    ) : PressureSocket<IDealerSocket>(inner), IDealerSocket
    {
        internal int CompletedReplies { get; private set; }
        public new DealerSocketOptions Options => inner.Options;

        public void SetRoutingId(RoutingId rid) => inner.SetRoutingId(rid);

        public RoutingId GetRoutingId() => inner.GetRoutingId();

        public SendOperation Send() => inner.Send();

        public RequestOperation Request() => new CompletedReplyRequest(inner.Request(), this);

        private void CompleteReply(RequestSubmission result)
        {
            var events = new PollEvent[2];
            while (!result.Reply.IsCompleted)
            {
                Assert.True(poller.Wait(events, TimeSpan.FromSeconds(5)) > 0);
                using var received = Received.Create();
                if (!router.Recv(received, RecvFlags.DontWait))
                    continue;
                Assert.True(
                    ZLinkClientServerControlProtocol.TryDecodeLivenessProbe(
                        received.Parts,
                        out var probeId
                    )
                );
                using var ack = ZLinkClientServerControlProtocol.EncodeLivenessAck(probeId);
                received.Reply().Message(ack).Submit();
            }
            // The original c1878 native-reply barrier, held entirely in the fixture.
            _ = result.Reply.GetAwaiter().GetResult();
            CompletedReplies++;
        }

        private sealed class CompletedReplyRequest(
            RequestOperation request,
            CompletedReplyDealer owner
        ) : RequestOperation, RequestSubmitOperation
        {
            private RequestSubmitOperation _submission = null!;

            public RequestSubmitOperation Message(Message message)
            {
                _submission = _submission is null
                    ? request.Message(message)
                    : _submission.Message(message);
                return this;
            }

            public RequestSubmitOperation Timeout(TimeSpan timeout)
            {
                _submission = _submission.Timeout(timeout);
                return this;
            }

            public IReadOnlyList<Message> Submit() => throw new NotSupportedException();

            public RequestSubmission Async(CancellationToken cancellationToken = default)
            {
                var result = _submission.Async(cancellationToken);
                owner.CompleteReply(result);
                return result;
            }
        }
    }
}
