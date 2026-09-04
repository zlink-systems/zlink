using System.Diagnostics;
using Xunit;

namespace Systems.Zlink.Tests;

/// <summary>
///     An admitted async send completes immediately without a native completion.
///     A backpressured send waits for its exact WRITABLE token and then retries
///     the same managed record. Requests retain their native completion flow.
/// </summary>
public sealed class test_routed_async_admission
{
    private const ulong RecordHwm = 65_536UL + 64UL;
    private static readonly string FillerPayload =
        "filler" + new string('d', 65_536);

    [Fact]
    public void synchronous_terminal_admits_a_routed_send()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-sync-admission");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message payload = Message.From("sync-payload");
        dealer.Send().Message(payload).Submit();

        using Received received = RecvWithRetry(router);
        Assert.Equal("sync-payload", received.SinglePartOrThrow().GetString());
    }

    [Fact]
    public async Task async_terminal_waits_for_writable_without_blocking_for_credit()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-sync-dontwait");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        _ = FillDealerTarget(dealer, out List<Task> filler);

        using var cancellation = new CancellationTokenSource();
        using Message payload = Message.From("pending");
        var started = Stopwatch.StartNew();
        Task pending = dealer.Send().Message(payload).Async(cancellation.Token);
        started.Stop();

        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
        await Task.Delay(25);
        Assert.False(pending.IsCompleted);
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            async () => await pending);
        SwallowAll(filler);
    }

    [Fact]
    public async Task request_cancellation_only_cancels_the_caller_wait()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-request-wait-cancel");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        using var cancellation = new CancellationTokenSource();
        using Message payload = Message.From("cancel-wait");
        Task<IReadOnlyList<Message>> pending = dealer.Request().Message(payload)
            .Timeout(TimeSpan.FromSeconds(2)).Async(cancellation.Token);
        using Received received = RecvWithRetry(router);

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            async () => await pending);

        using Message reply = Message.From("late-reply");
        router.Reply(received.RoutingId!.Value, received.ReplyToken!)
            .Message(reply).Submit();
        await Task.Delay(25);
    }

    [Fact]
    public async Task admitted_send_completes_without_occupying_the_caller()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-inline-admission");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message payload = Message.From("inline-payload");
        var started = Stopwatch.StartNew();
        Task admitted = dealer.Send().Message(payload).Async();
        started.Stop();

        // A successful SEND has completion id zero and no native completion.
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
        Assert.True(admitted.IsCompletedSuccessfully);
        await admitted.WaitAsync(TimeSpan.FromSeconds(3));

        using Received received = RecvWithRetry(router);
        Assert.Collection(received.Parts,
            part => Assert.Equal("inline-payload", part.GetString()));
    }

    [Theory]
    [InlineData(2)]
    [InlineData(9)]
    public async Task multipart_record_admission(int partCount)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-multipart-admission");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        string[] expected = Enumerable.Range(0, partCount)
            .Select(index => $"mp-part-{index}")
            .ToArray();
        Message[] parts = expected.Select(Message.From).ToArray();
        try
        {
            await dealer.Send().Messages(parts).Async()
                .WaitAsync(TimeSpan.FromSeconds(3));

            using Received received = RecvWithRetry(router);
            Assert.Equal(expected,
                received.Parts.Select(part => part.GetString()));
        }
        finally
        {
            foreach (Message part in parts)
                part.Dispose();
        }
    }

    [Fact]
    public async Task two_part_inline_admission_consumes_both_parts_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-two-part-inline-admission");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message payload = Message.From("two-part-inline");
        using Message tail = Message.Allocate(0);
        Task admission = dealer.Send().Message(payload).Message(tail).Async();

        Assert.Throws<ObjectDisposedException>(() => _ = payload.Size);
        Assert.Throws<ObjectDisposedException>(() => _ = tail.Size);
        await admission.WaitAsync(TimeSpan.FromSeconds(3));
        Assert.True(admission.IsCompletedSuccessfully);

        using Received received = RecvWithRetry(router);
        Assert.Collection(received.Parts,
            part => Assert.Equal("two-part-inline", part.GetString()),
            part => Assert.Equal(0, part.Size));
    }

    [Theory]
    [InlineData(2)]
    [InlineData(9)]
    public void multipart_materialization_failure_restores_moved_parts(
        int partCount)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var sender = context.CreatePairSocket();
        string[] expected = Enumerable.Range(0, partCount)
            .Select(index => $"restore-part-{index}")
            .ToArray();
        Message[] parts = expected.Select(Message.From).ToArray();
        parts[^1].Dispose();
        try
        {
            SendSubmitOperation operation = sender.Send().Messages(parts);
            Action submit = () => _ = operation.Async();
            Assert.Throws<ObjectDisposedException>(submit);

            for (var index = 0; index < parts.Length - 1; index++)
                Assert.Equal(expected[index], parts[index].GetString());
        }
        finally
        {
            foreach (Message part in parts)
                part.Dispose();
        }
    }

    [Fact]
    public async Task public_poller_drains_writable_and_retries_the_same_packet()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-writable-retry");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        // A blocking handshake establishes the target without a timing delay.
        using (Message handshake = Message.From("handshake"))
            dealer.Send().Message(handshake).Submit();
        using (Received handshake = Received.Create())
        {
            Assert.True(router.Recv(handshake));
            Assert.Equal("handshake", handshake.SinglePartOrThrow().GetString());
        }

        using var poller = Zlink.CreatePoller();
        poller.Add(dealer,
            PollEventFlags.PollOut | PollEventFlags.PollCompletion, 41);

        Task pending = FillDealerUntilAsyncBackpressured(dealer,
            out List<string> acceptedPayloads, out string pendingPayload);
        Assert.NotEmpty(acceptedPayloads);
        Assert.False(pending.IsCompleted);

        var events = new PollEvent[1];
        Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));

        foreach (string expected in acceptedPayloads)
        {
            using Received filler = Received.Create();
            Assert.True(router.Recv(filler));
            Assert.Equal(expected, filler.SinglePartOrThrow().GetString());
        }

        Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(5)));
        Assert.Equal((nuint)41, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollOut);
        Assert.Equal(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollCompletion);

        // Poller.Wait pulls the WRITABLE completion, matches its token,
        // context, and target, and retries this exact packet.
        await pending.WaitAsync(TimeSpan.FromSeconds(2));

        using Received retried = Received.Create();
        Assert.True(router.Recv(retried));
        Assert.Equal(pendingPayload,
            retried.SinglePartOrThrow().GetString());

        using Received duplicate = Received.Create();
        Assert.False(router.Recv(duplicate, RecvFlags.DontWait));
    }

    [Fact]
    public async Task filtered_writable_wake_keeps_waiting_for_request_completion()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-filtered-writable-request-wait");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        using (Message handshake = Message.From("handshake"))
            dealer.Send().Message(handshake).Submit();
        using (Received handshake = Received.Create())
            Assert.True(router.Recv(handshake));

        // POLLCOMPLETION owns the queue but does not expose a WRITABLE-only
        // wake. Wait must keep its original deadline after making that progress.
        using var poller = Zlink.CreatePoller();
        poller.Add(dealer, PollEventFlags.PollCompletion, 43);

        Task pendingSend = FillDealerUntilAsyncBackpressured(dealer,
            out List<string> acceptedPayloads, out string pendingPayload);
        Assert.NotEmpty(acceptedPayloads);
        Assert.False(pendingSend.IsCompleted);
        foreach (string expected in acceptedPayloads)
        {
            using Received filler = Received.Create();
            Assert.True(router.Recv(filler));
            Assert.Equal(expected, filler.SinglePartOrThrow().GetString());
        }

        using Message request = Message.From("request-after-credit");
        Task<IReadOnlyList<Message>> replyTask = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(5))
            .Async();
        RoutingId source;
        ReplyToken replyToken;
        using (Received receivedRequest = Received.Create())
        {
            Assert.True(router.Recv(receivedRequest));
            Assert.Equal(ReceivedMessageType.Request,
                receivedRequest.MessageType);
            source = receivedRequest.RoutingId!.Value;
            replyToken = receivedRequest.ReplyToken!;
        }

        Task responder = ReplyAfterSendRetry(pendingSend, router, source,
            replyToken);
        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(5)));
        Assert.Equal((nuint)43, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollCompletion);

        IReadOnlyList<Message> reply = await replyTask.WaitAsync(
            TimeSpan.FromSeconds(2));
        try
        {
            Assert.Single(reply);
            Assert.Equal("reply-after-retry", reply[0].GetString());
        }
        finally
        {
            foreach (Message part in reply)
                part.Dispose();
        }
        await responder;

        using Received retried = Received.Create();
        Assert.True(router.Recv(retried));
        Assert.Equal(pendingPayload,
            retried.SinglePartOrThrow().GetString());
    }

    [Fact]
    public void try_submit_reports_backpressure_and_preserves_the_packet()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-try-submit");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        using (Message handshake = Message.From("handshake"))
            dealer.Send().Message(handshake).Submit();
        using (Received handshake = Received.Create())
            Assert.True(router.Recv(handshake));

        using var poller = Zlink.CreatePoller();
        poller.Add(dealer,
            PollEventFlags.PollOut | PollEventFlags.PollCompletion, 42);

        Message blocked = FillDealerUntilTrySubmitBackpressured(dealer,
            out int acceptedCount);
        using (blocked)
        {
            Assert.True(acceptedCount > 0);
            Assert.Equal(FillerPayload, blocked.GetString());

            var events = new PollEvent[1];
            Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));

            for (var index = 0; index < acceptedCount; index++)
            {
                using Received filler = Received.Create();
                Assert.True(router.Recv(filler));
            }

            Assert.Equal(1,
                poller.Wait(events, TimeSpan.FromSeconds(5)));
            Assert.Equal((nuint)42, events[0].Slot);
            Assert.NotEqual(PollEventFlags.None,
                events[0].Revents & PollEventFlags.PollOut);
            Assert.Equal(PollEventFlags.None,
                events[0].Revents & PollEventFlags.PollCompletion);

            Assert.True(dealer.Send().Message(blocked).TrySubmit());
            using Received retried = Received.Create();
            Assert.True(router.Recv(retried));
            Assert.Equal(FillerPayload,
                retried.SinglePartOrThrow().GetString());
        }
    }

    [Fact]
    public async Task cancellation_completes_a_writable_waiter_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-cancel");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        _ = FillDealerTarget(dealer, out List<Task> filler);

        using var cancellation = new CancellationTokenSource();
        using Message payload = Message.From("pending-cancel");
        Task pending = dealer.Send()
            .Message(payload)
            .Async(cancellation.Token);
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            await pending.WaitAsync(TimeSpan.FromSeconds(3)));
        // Exactly-once: the terminal stays cancelled.
        Assert.True(pending.IsCanceled);
        SwallowAll(filler);
    }

    [Fact]
    public async Task close_completes_a_writable_waiter_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-close");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        _ = FillDealerTarget(dealer, out List<Task> filler);

        using Message payload = Message.From("pending-close");
        Task pending = dealer.Send().Message(payload).Async();
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        dealer.Dispose();
        ZlinkSubmitException error = await Assert.ThrowsAsync<
            ZlinkSubmitException>(async () =>
            await pending.WaitAsync(TimeSpan.FromSeconds(3)));
        Assert.Equal(ZlinkSubmitException.ErrorCode.Terminated, error.Result);
        SwallowAll(filler);
    }

    [Fact]
    public async Task close_completes_an_accepted_request_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-close-request");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message request = Message.From("accepted-before-close");
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromSeconds(2))
            .Async();
        using Received accepted = RecvWithRetry(router);
        Assert.Equal(ReceivedMessageType.Request, accepted.MessageType);

        dealer.Dispose();
        ZlinkRequestException error = await Assert.ThrowsAsync<
            ZlinkRequestException>(async () => await pending);
        Assert.Equal(ZlinkRequestException.ErrorCode.Terminated, error.Result);
    }

    [Fact]
    public async Task disconnect_terminates_the_selected_generation()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var router = context.CreateRouterSocket();
        using var dealer = context.CreateDealerSocket();
        RoutingId dealerRid = CoreTestSupport.RoutingIdUtf8(
            "async-disconnect-target");
        dealer.SetRoutingId(dealerRid);
        router.Options.SendHighWaterMark = RecordHwm;
        dealer.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-async-disconnect");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        _ = FillRouterTarget(router, dealerRid, out List<Task> filler);

        using Message payload = Message.From("pending-disconnect");
        Task pending = router.Send(dealerRid).Message(payload).Async();
        await Task.Delay(50);
        Assert.False(pending.IsCompleted);

        router.DisconnectRid(dealerRid);
        ZlinkSubmitException error = await Assert.ThrowsAsync<
            ZlinkSubmitException>(async () => await pending.WaitAsync(
            TimeSpan.FromSeconds(3)));
        Assert.Contains(error.Result, new[]
        {
            ZlinkSubmitException.ErrorCode.NotConnected,
            ZlinkSubmitException.ErrorCode.NotFound,
            ZlinkSubmitException.ErrorCode.Terminated
        });
        SwallowAll(filler);
    }

    [Fact]
    public async Task request_timeout_is_core_owned()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint(
            "inproc", "dotnet-routed-request-deadline");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        using Message request = Message.From("deadline-request");
        var started = Stopwatch.StartNew();
        Task<IReadOnlyList<Message>> pending = dealer.Request()
            .Message(request)
            .Timeout(TimeSpan.FromMilliseconds(100))
            .Async();
        started.Stop();
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));

        ZlinkRequestException error = await Assert.ThrowsAsync<
            ZlinkRequestException>(async () => await pending);
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, error.Result);
    }

    /// <summary>
    ///     Submits large records until one is backpressured, and returns its task.
    ///     Awaitable send never blocks the caller for HWM credit: a record that
    ///     cannot be admitted waits for WRITABLE and is then retried.
    /// </summary>
    private static Task FillDealerTarget(IDealerSocket dealer,
        out List<Task> submitted)
    {
        submitted = new List<Task>();
        for (var attempt = 0; attempt < 16; attempt++)
        {
            using Message filler = Message.From(FillerPayload);
            Task task = dealer.Send().Message(filler).Async();
            submitted.Add(task);
            if (!task.IsCompleted)
                return task;
        }

        throw new Xunit.Sdk.XunitException(
            "The DEALER target never backpressured a record.");
    }

    private static Task FillRouterTarget(IRouterSocket router,
        RoutingId routingId, out List<Task> submitted)
    {
        submitted = new List<Task>();
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        for (var attempt = 0; attempt < 64; attempt++)
        {
            using Message filler = Message.From(FillerPayload);
            Task task;
            try
            {
                task = router.Send(routingId).Message(filler).Async();
            }
            catch (ZlinkSubmitException) when (DateTimeOffset.UtcNow < deadline)
            {
                // The ROUTER has not admitted the route yet. Retrying is the
                // application's policy: the binding never retries.
                Thread.Sleep(10);
                continue;
            }

            submitted.Add(task);
            if (!task.IsCompleted)
                return task;
        }

        throw new Xunit.Sdk.XunitException(
            "The ROUTER target never backpressured a record.");
    }

    private static Task FillDealerUntilAsyncBackpressured(
        IDealerSocket dealer, out List<string> acceptedPayloads,
        out string pendingPayload)
    {
        acceptedPayloads = new List<string>();
        pendingPayload = string.Empty;
        for (var attempt = 0; attempt < 16; attempt++)
        {
            string payload = FillerPayload + $"-{attempt:D2}";
            using Message candidate = Message.From(payload);
            Task submitted = dealer.Send().Message(candidate).Async();
            if (!submitted.IsCompleted)
            {
                pendingPayload = payload;
                return submitted;
            }

            submitted.GetAwaiter().GetResult();
            acceptedPayloads.Add(payload);
        }

        throw new Xunit.Sdk.XunitException(
            "The DEALER target did not return a pending WRITABLE waiter.");
    }

    private static Message FillDealerUntilTrySubmitBackpressured(
        IDealerSocket dealer,
        out int acceptedCount)
    {
        acceptedCount = 0;
        for (var attempt = 0; attempt < 16; attempt++)
        {
            Message candidate = Message.From(FillerPayload);
            try
            {
                if (!dealer.Send().Message(candidate).TrySubmit())
                    return candidate;

                acceptedCount++;
                candidate.Dispose();
            }
            catch
            {
                candidate.Dispose();
                throw;
            }
        }

        throw new Xunit.Sdk.XunitException(
            "The DEALER target did not report backpressure through TrySubmit.");
    }

    private static async Task ReplyAfterSendRetry(Task pendingSend,
        IRouterSocket router, RoutingId source, ReplyToken replyToken)
    {
        await pendingSend.ConfigureAwait(false);
        using Message reply = Message.From("reply-after-retry");
        router.Reply(source, replyToken).Message(reply).Submit();
    }

    private static void SwallowAll(IEnumerable<Task> tasks)
    {
        foreach (Task task in tasks)
            _ = task.ContinueWith(static completed => _ = completed.Exception,
                TaskScheduler.Default);
    }

    private static Received RecvWithRetry(IRouterSocket socket)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(3);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var received = Received.Create();
            if (socket.Recv(received, RecvFlags.DontWait))
                return received;
            received.Dispose();
            Thread.Sleep(1);
        }

        throw new TimeoutException("Timed out waiting for routed record.");
    }

}
