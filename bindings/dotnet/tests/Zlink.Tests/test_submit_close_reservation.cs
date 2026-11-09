using System.Collections.Concurrent;
using Xunit;
using Xunit.Abstractions;

namespace Systems.Zlink.Tests;

public sealed class test_submit_close_reservation
{
    private const int Repetitions = 1000;
    private readonly ITestOutputHelper _output;

    public test_submit_close_reservation(ITestOutputHelper output)
    {
        _output = output;
    }

    [Theory]
    [InlineData("send")]
    [InlineData("request")]
    [InlineData("reply")]
    public void close_between_validation_and_native_submit(string kind)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "close-window");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using (Message handshake = Message.From("ready"))
            dealer.Send().Message(handshake).Submit();
        using var received = Received.Create();
        Assert.True(router.Recv(received));
        using var poller = Zlink.CreatePoller();
        poller.Add(dealer, PollEventFlags.PollCompletion, 1);
        Task<IReadOnlyList<Message>>? pending = null;
        if (kind == "reply")
        {
            using Message request = Message.From("request");
            pending = dealer.Request().Message(request)
                .Timeout(TimeSpan.FromSeconds(30)).Async().Reply;
            Assert.True(router.Recv(received));
        }
        ISocket subject = kind == "reply" ? router : dealer;
        object owner = CompletionOwnerTestAccess.Owner(subject);
        bool acceptedClose = false;
        using Message payload = Message.From("preserved");
        var parts = new ClosingParts(payload, () =>
        {
            try
            {
                subject.Close();
                acceptedClose = true;
            }
            catch (ZlinkCloseException error) when (
                error.Result == ZlinkCloseException.ErrorCode.Busy)
            {
            }
        });
        Exception? failure = Record.Exception(() =>
        {
            switch (kind)
            {
                case "send":
                    CompletionOwnerTestAccess.Invoke(owner, "Send", null, parts);
                    break;
                case "request":
                    pending = ((RequestSubmission)CompletionOwnerTestAccess.Invoke(
                        owner, "RequestAsync", null, parts, 30_000u,
                        CancellationToken.None)!).Reply;
                    break;
                case "reply":
                    CompletionOwnerTestAccess.Invoke(owner, "Reply",
                        received.RoutingId!.Value, received.ReplyToken!, parts);
                    break;
            }
        });
        _output.WriteLine($"kind={kind} accepted_close={acceptedClose} failure={failure}");
        subject.Close();
        dealer.Close();
        if (pending is not null)
            Assert.Throws<ZlinkRequestException>(() => pending.GetAwaiter().GetResult());
        if (acceptedClose)
        {
            var error = Assert.IsType<ZlinkSubmitException>(failure);
            Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, error.Result);
            Assert.Equal("preserved", payload.GetString());
        }
        else
            Assert.Null(failure);
    }

    private sealed class ClosingParts : IReadOnlyList<Message>
    {
        private readonly Message _part;
        private Action? _close;

        internal ClosingParts(Message part, Action close)
        {
            _part = part;
            _close = close;
        }

        public int Count => 1;

        public Message this[int index]
        {
            get
            {
                Action? close = _close;
                _close = null;
                close?.Invoke();
                return index == 0 ? _part
                    : throw new ArgumentOutOfRangeException(nameof(index));
            }
        }

        public IEnumerator<Message> GetEnumerator()
        {
            yield return this[0];
        }

        System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() =>
            GetEnumerator();
    }

    [Theory]
    [InlineData("send")]
    [InlineData("request")]
    [InlineData("reply")]
    [InlineData("publish")]
    public void same_handle_submit_races_close(string kind)
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        var outcomes = new ConcurrentDictionary<string, int>();
        var closedOutcomes = new ConcurrentDictionary<string, int>();
        var closeBusy = 0;

        for (var repetition = 0; repetition < Repetitions; repetition++)
        {
            using var router = context.CreateRouterSocket();
            using var dealer = context.CreateDealerSocket();
            using var publisher = context.CreatePubSocket();
            router.Options.Linger = TimeSpan.Zero;
            dealer.Options.Linger = TimeSpan.Zero;
            publisher.Options.Linger = TimeSpan.Zero;
            string endpoint = CoreTestSupport.NewEndpoint("inproc",
                $"submit-close-{kind}-{repetition}");
            router.Bind(endpoint);
            dealer.Connect(endpoint);
            using (Message handshake = Message.From("ready"))
                dealer.Send().Message(handshake).Submit();
            using var received = Received.Create();
            Assert.True(router.Recv(received));
            using var poller = Zlink.CreatePoller();
            poller.Add(dealer, PollEventFlags.PollCompletion, 1);

            Task<IReadOnlyList<Message>>? pending = null;
            Task? admitted = null;
            if (kind == "reply")
            {
                using Message request = Message.From("request");
                pending = dealer.Request().Message(request)
                    .Timeout(TimeSpan.FromSeconds(30)).Async().Reply;
                Assert.True(router.Recv(received));
            }

            ISocket subject = kind == "reply" ? router
                : kind == "publish" ? publisher : dealer;
            using var start = new Barrier(2);
            Exception? failure = null;
            int delay = repetition % 3;
            var submitter = new Thread(() =>
            {
                start.SignalAndWait();
                if (delay == 0)
                    Thread.Yield();
                try
                {
                    Submit();
                }
                catch (Exception exception)
                {
                    failure = exception;
                }
            })
            { IsBackground = true };
            submitter.Start();
            start.SignalAndWait();
            if (delay == 1)
                Thread.Yield();
            else if (delay == 2)
                Thread.SpinWait(100);
            try
            {
                subject.Close();
            }
            catch (ZlinkCloseException exception) when (
                exception.Result == ZlinkCloseException.ErrorCode.Busy)
            {
                closeBusy++;
            }
            Assert.True(submitter.Join(TimeSpan.FromSeconds(10)));
            Count(outcomes, failure);
            subject.Close();

            failure = Record.Exception(Submit);
            Count(closedOutcomes, failure);
            dealer.Close();
            if (pending is not null)
            {
                ZlinkRequestException terminated = Assert.Throws<ZlinkRequestException>(
                    () => pending.GetAwaiter().GetResult());
                Assert.Equal(ZlinkRequestException.ErrorCode.Terminated,
                    terminated.Result);
                if (admitted is { IsCompletedSuccessfully: false })
                {
                    ZlinkRequestException admissionError = Assert.Throws<ZlinkRequestException>(
                        () => admitted.GetAwaiter().GetResult());
                    Assert.Same(terminated, admissionError);
                }
            }

            void Submit()
            {
                using Message part = Message.From("payload");
                switch (kind)
                {
                    case "send":
                        dealer.Send().Message(part).Submit();
                        break;
                    case "request":
                        RequestSubmission submission = dealer.Request().Message(part)
                            .Timeout(TimeSpan.FromSeconds(30)).Async();
                        pending = submission.Reply;
                        admitted = submission.Admitted;
                        Assert.Equal(SubmitResult.Ok, submission.Result);
                        break;
                    case "reply":
                        received.Reply().Message(part).Submit();
                        break;
                    case "publish":
                        publisher.Publish("topic").Message(part).Submit();
                        break;
                    default:
                        throw new ArgumentOutOfRangeException(nameof(kind));
                }
            }
        }

        _output.WriteLine($"kind={kind} repetitions={Repetitions} close_busy={closeBusy}");
        Print("race", outcomes);
        Print("closed", closedOutcomes);
        Assert.Equal(Repetitions, outcomes.Values.Sum());
        Assert.All(outcomes.Keys, outcome =>
            Assert.Contains(outcome, new[] { "success", "TERMINATED" }));
        Assert.Equal(Repetitions, closedOutcomes.GetValueOrDefault("TERMINATED"));
    }

    private void Print(string phase, ConcurrentDictionary<string, int> outcomes)
    {
        foreach (var pair in outcomes.OrderBy(pair => pair.Key))
            _output.WriteLine($"phase={phase} outcome={pair.Key} count={pair.Value}");
    }

    private static void Count(ConcurrentDictionary<string, int> outcomes,
        Exception? exception)
    {
        string outcome = exception switch
        {
            null => "success",
            ZlinkSubmitException submit when submit.Result ==
                ZlinkSubmitException.ErrorCode.Terminated => "TERMINATED",
            ZlinkException error when error.NativeErrno ==
                CoreTestSupport.Errno("EFAULT") => "EFAULT",
            ZlinkException error when error.NativeErrno ==
                CoreTestSupport.Errno("ENOENT") => "ENOENT",
            ZlinkSubmitException submit when submit.Result ==
                ZlinkSubmitException.ErrorCode.InvalidHandle => "INVALID_HANDLE",
            ZlinkSubmitException submit when submit.Result ==
                ZlinkSubmitException.ErrorCode.NotFound => "NOT_FOUND",
            _ => $"other:{exception.GetType().Name}:{exception.Message}"
        };
        outcomes.AddOrUpdate(outcome, 1, (_, count) => count + 1);
    }
}
