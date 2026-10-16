using System.Reflection;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;
using Zlink.Framework.UnitTests.Runtime;

namespace Zlink.Framework.UnitTests;

public sealed class ClientServerLivenessLaneTests
{
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
