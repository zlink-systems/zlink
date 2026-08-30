using System.Diagnostics;
using System.Reflection;
using Xunit;

namespace Systems.Zlink.Tests;

/// <summary>
///     The routed asynchronous send terminal is a thin wrapper over the Core
///     0.13.2 send-admission contract: one complete record goes to
///     <c>zlink_send_async</c>. Immediate admission returns operation id zero
///     and resolves locally; a non-zero pending operation receives exactly one
///     Core completion. The binding parks nothing, retries nothing and times
///     nothing.
/// </summary>
public sealed class test_routed_async_admission
{
    private const ulong RecordHwm = 65_536UL + 64UL;
    private static readonly string FillerPayload =
        "filler" + new string('d', 65_536);

    [Fact]
    public void pending_send_defers_task_allocation_until_it_is_observed()
    {
        object pending = CreatePendingSend();
        FieldInfo completionField = PendingSendCompletionField(pending);

        Assert.Null(completionField.GetValue(pending));

        Task task = PendingSendTask(pending);

        Assert.NotNull(completionField.GetValue(pending));
        Assert.False(task.IsCompleted);

        CompletePendingSendAsAdmitted(pending);

        Assert.True(task.IsCompletedSuccessfully);
    }

    [Fact]
    public void inline_completion_and_task_observation_share_one_task()
    {
        object pending = CreatePendingSend();
        FieldInfo completionField = PendingSendCompletionField(pending);

        CompletePendingSendAsAdmitted(pending);
        object? completionAfterCallback = completionField.GetValue(pending);
        Task first = PendingSendTask(pending);
        Task second = PendingSendTask(pending);

        Assert.NotNull(completionAfterCallback);
        Assert.Same(first, second);
        Assert.True(first.IsCompletedSuccessfully);
    }

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
        dealer.Send().Message(payload).Submit(SendFlags.None);

        using Received received = RecvWithRetry(router);
        Assert.Equal("sync-payload", received.SinglePartOrThrow().GetString());
    }

    [Fact]
    public void dontwait_terminal_reports_immediate_backpressure()
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

        using Message payload = Message.From("backpressured");
        var started = Stopwatch.StartNew();
        ZlinkSubmitException error = Assert.Throws<ZlinkSubmitException>(() =>
            dealer.Send().Message(payload).Submit(SendFlags.DontWait));
        started.Stop();

        Assert.Equal(ZlinkSubmitException.ErrorCode.Backpressured, error.Result);
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
        SwallowAll(filler);
    }

    [Fact]
    public void request_callback_terminal_reports_immediate_backpressure()
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
            "inproc", "dotnet-request-callback-dontwait");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);
        _ = FillDealerTarget(dealer, out List<Task> filler);

        using Message payload = Message.From("request-backpressured");
        bool callbackCalled = false;
        var started = Stopwatch.StartNew();
        ZlinkSubmitException error = Assert.Throws<ZlinkSubmitException>(() =>
            dealer.Request().Message(payload).Timeout(TimeSpan.FromSeconds(1))
                .Submit(SendFlags.DontWait, (_, _) => callbackCalled = true));
        started.Stop();

        Assert.Equal(ZlinkSubmitException.ErrorCode.Backpressured, error.Result);
        Assert.False(callbackCalled);
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
        SwallowAll(filler);
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

        // Immediate admission returns operation id zero and the binding
        // resolves locally without entering Core's callback dispatch scope.
        Assert.Same(Task.CompletedTask, admitted);
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
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

        Assert.Same(Task.CompletedTask, admission);
        Assert.Throws<ObjectDisposedException>(() => _ = payload.Size);
        Assert.Throws<ObjectDisposedException>(() => _ = tail.Size);
        await admission.WaitAsync(TimeSpan.FromSeconds(3));

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
            RoutedSendSubmitOperation operation = sender.Send().Messages(parts);
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
    public async Task backpressured_send_returns_immediately_and_core_completes_it()
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
            "inproc", "dotnet-routed-async-multipart");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Thread.Sleep(100);

        Task parked = FillDealerTarget(dealer, out List<Task> filler);

        using Message payload = Message.From("pending-payload");
        var started = Stopwatch.StartNew();
        Task pending = dealer.Send().Message(payload).Async();
        started.Stop();

        // The submit never waits for credit, whether or not it parks.
        Assert.True(started.Elapsed < TimeSpan.FromMilliseconds(250));
        Assert.False(parked.IsCompleted);

        // Draining the receiver returns credit and Core completes every
        // parked record in submit order.
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (!pending.IsCompleted && DateTimeOffset.UtcNow < deadline)
        {
            using var received = Received.Create();
            if (!router.Recv(received, RecvFlags.DontWait))
                await Task.Delay(1);
        }

        await pending.WaitAsync(TimeSpan.FromSeconds(2));
        await Task.WhenAll(filler).WaitAsync(TimeSpan.FromSeconds(5));
    }

    [Fact]
    public async Task cancellation_completes_a_parked_send_once()
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
    public async Task close_completes_a_pending_send_once()
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
    ///     Submits large records until one parks, and returns that parked task.
    ///     `zlink_send_async` never reports HWM back-pressure to the caller: a
    ///     record that cannot be admitted becomes a Core pending operation.
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
            "The DEALER target never parked a record.");
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
            "The ROUTER target never parked a record.");
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

    private static object CreatePendingSend()
    {
        Type pendingType = typeof(Message).Assembly.GetType(
            "Systems.Zlink.SendCompletionRegistry+PendingSend",
            throwOnError: true)!;
        ConstructorInfo constructor = pendingType.GetConstructor(
            BindingFlags.Instance | BindingFlags.NonPublic,
            binder: null, new[] { typeof(CancellationToken) },
            modifiers: null)!;
        return constructor.Invoke(new object[] { CancellationToken.None });
    }

    private static FieldInfo PendingSendCompletionField(object pending)
    {
        return pending.GetType().GetField("_completion",
            BindingFlags.Instance | BindingFlags.NonPublic)!;
    }

    private static Task PendingSendTask(object pending)
    {
        PropertyInfo property = pending.GetType().GetProperty("Task",
            BindingFlags.Instance | BindingFlags.NonPublic)!;
        return (Task)property.GetValue(pending)!;
    }

    private static void CompletePendingSendAsAdmitted(object pending)
    {
        Type pendingType = pending.GetType();
        MethodInfo method = pendingType.GetMethod("Complete",
            BindingFlags.Instance | BindingFlags.NonPublic)!;
        Type resultType = method.GetParameters()[0].ParameterType;
        object admitted = Enum.Parse(resultType, "Admitted");
        method.Invoke(pending, new[] { admitted, (object)0 });
    }
}
