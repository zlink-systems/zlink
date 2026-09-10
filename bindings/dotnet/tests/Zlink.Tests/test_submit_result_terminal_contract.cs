using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_submit_result_terminal_contract
{
    private const ulong RecordHwm = 65_536UL + 64UL;
    private const int MaxFillRecords = 32;
    private static readonly string FillerPayload =
        "submit-result-" + new string('z', 65_536);

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    [InlineData(5)]
    public async Task immediate_admission_returns_ok_before_request_reply(
        int repetition)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var server = context.CreateRouterSocket();
        using var client = context.CreateDealerSocket();
        ConnectReady(server, client, $"submit-result-ok-{repetition}");

        using (Message payload = Message.From("send-ok"))
        {
            SendSubmission send = client.Send().Message(payload).Async();
            Assert.Equal(SubmitResult.Ok, send.Result);
            Assert.True(send.Admitted.IsCompletedSuccessfully);
            await send.Admitted;
        }
        AssertReceived(server, "send-ok");

        RequestSubmission request;
        using (Message payload = Message.From("request-ok"))
        {
            request = client.Request().Message(payload)
                .Timeout(TimeSpan.FromSeconds(30)).Async();
        }
        Assert.Equal(SubmitResult.Ok, request.Result);
        Assert.True(request.Admitted.IsCompletedSuccessfully);
        Assert.False(request.Reply.IsCompleted);

        ReplyOnce(server, "request-ok", "reply-ok");
        IReadOnlyList<Message> reply = await request.Reply.WaitAsync(
            TimeSpan.FromSeconds(5));
        try
        {
            Assert.Equal("reply-ok", Assert.Single(reply).GetString());
        }
        finally
        {
            Zlink.MultipartClose(reply);
        }
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(3)]
    [InlineData(4)]
    [InlineData(5)]
    public async Task hwm_returns_backpressured_then_admits_before_reply(
        int repetition)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        await VerifyBackpressuredSend(repetition);
        await VerifyBackpressuredRequest(repetition);
    }

    private static async Task VerifyBackpressuredSend(int repetition)
    {
        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var server = context.CreateRouterSocket();
        using var client = context.CreateDealerSocket();
        using var poller = Zlink.CreatePoller();
        ConfigureSmallHwm(server, client);
        ConnectReady(server, client, $"submit-result-send-hwm-{repetition}");
        poller.Add(client,
            PollEventFlags.PollOut | PollEventFlags.PollCompletion, 1);

        SendSubmission waiting = default;
        var acceptedCount = 0;
        for (var sequence = 0; sequence < MaxFillRecords; sequence++)
        {
            using Message payload = Message.From(Payload(sequence));
            SendSubmission submission = client.Send().Message(payload).Async();
            if (submission.Result == SubmitResult.Backpressured)
            {
                waiting = submission;
                break;
            }
            Assert.Equal(SubmitResult.Ok, submission.Result);
            Assert.True(submission.Admitted.IsCompletedSuccessfully);
            acceptedCount++;
        }

        Assert.True(acceptedCount > 0);
        Assert.Equal(SubmitResult.Backpressured, waiting.Result);
        Assert.False(waiting.Admitted.IsCompleted);
        Assert.Equal(0, poller.Wait(new PollEvent[1], TimeSpan.Zero));

        for (var sequence = 0; sequence < acceptedCount; sequence++)
            AssertReceived(server, Payload(sequence));

        AwaitPollUntil(poller, waiting.Admitted);
        await waiting.Admitted.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(SubmitResult.Backpressured, waiting.Result);
        AssertReceived(server, Payload(acceptedCount));
    }

    private static async Task VerifyBackpressuredRequest(int repetition)
    {
        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var server = context.CreateRouterSocket();
        using var client = context.CreateDealerSocket();
        using var poller = Zlink.CreatePoller();
        ConfigureSmallHwm(server, client);
        ConnectReady(server, client,
            $"submit-result-request-hwm-{repetition}");
        poller.Add(client,
            PollEventFlags.PollOut | PollEventFlags.PollCompletion, 1);

        var admitted = new List<RequestSubmission>();
        RequestSubmission waiting = default;
        for (var sequence = 0; sequence < MaxFillRecords; sequence++)
        {
            using Message payload = Message.From(Payload(sequence));
            RequestSubmission submission = client.Request().Message(payload)
                .Timeout(TimeSpan.FromSeconds(30)).Async();
            if (submission.Result == SubmitResult.Backpressured)
            {
                waiting = submission;
                break;
            }
            Assert.Equal(SubmitResult.Ok, submission.Result);
            Assert.True(submission.Admitted.IsCompletedSuccessfully);
            admitted.Add(submission);
        }

        Assert.NotEmpty(admitted);
        Assert.Equal(SubmitResult.Backpressured, waiting.Result);
        Assert.False(waiting.Admitted.IsCompleted);
        Assert.False(waiting.Reply.IsCompleted);
        Assert.Equal(0, poller.Wait(new PollEvent[1], TimeSpan.Zero));

        for (var sequence = 0; sequence < admitted.Count; sequence++)
            ReplyOnce(server, Payload(sequence), $"reply-{sequence}");

        AwaitPollUntil(poller, waiting.Admitted);
        await waiting.Admitted.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(SubmitResult.Backpressured, waiting.Result);
        Assert.False(waiting.Reply.IsCompleted);

        ReplyOnce(server, Payload(admitted.Count), "reply-waiting");
        AwaitPollUntil(poller, waiting.Reply);
        for (var sequence = 0; sequence < admitted.Count; sequence++)
        {
            IReadOnlyList<Message> reply = await admitted[sequence].Reply
                .WaitAsync(TimeSpan.FromSeconds(5));
            try
            {
                Assert.Equal($"reply-{sequence}",
                    Assert.Single(reply).GetString());
            }
            finally
            {
                Zlink.MultipartClose(reply);
            }
        }

        IReadOnlyList<Message> waitingReply = await waiting.Reply.WaitAsync(
            TimeSpan.FromSeconds(5));
        try
        {
            Assert.Equal("reply-waiting",
                Assert.Single(waitingReply).GetString());
        }
        finally
        {
            Zlink.MultipartClose(waitingReply);
        }
    }

    private static void ConnectReady(IRouterSocket server,
        IDealerSocket client, string label)
    {
        string endpoint = CoreTestSupport.NewEndpoint("inproc", label);
        server.Bind(endpoint);
        client.Connect(endpoint);
        using Message probe = Message.From("ready");
        client.Send().Message(probe).Submit();
        AssertReceived(server, "ready");
    }

    private static void ConfigureSmallHwm(IRouterSocket server,
        IDealerSocket client)
    {
        server.Options.Linger = TimeSpan.Zero;
        client.Options.Linger = TimeSpan.Zero;
        server.Options.ReceiveHighWaterMark = RecordHwm;
        client.Options.SendHighWaterMark = RecordHwm;
    }

    private static void AwaitPollUntil(IPoller poller, Task completion)
    {
        var events = new PollEvent[1];
        for (var attempt = 0; attempt < 32 && !completion.IsCompleted;
             attempt++)
        {
            Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(5)));
            Assert.Equal((nuint)1, events[0].Slot);
        }
        Assert.True(completion.IsCompleted);
    }

    private static void ReplyOnce(IRouterSocket server, string expected,
        string reply)
    {
        using Received received = Received.Create();
        Assert.True(server.Recv(received));
        Assert.Equal(expected,
            received.SinglePartOrThrow().GetString());
        using Message payload = Message.From(reply);
        received.Reply().Message(payload).Submit();
    }

    private static void AssertReceived(IRouterSocket server, string expected)
    {
        using Received received = Received.Create();
        Assert.True(server.Recv(received));
        Assert.Equal(expected,
            received.SinglePartOrThrow().GetString());
    }

    private static string Payload(int sequence) =>
        FillerPayload + $"-{sequence:D2}";
}
