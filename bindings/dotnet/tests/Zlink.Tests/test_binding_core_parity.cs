using Xunit;

namespace Systems.Zlink.Tests;

/// <summary>
///     The binding only wraps Core: an omitted request timeout is passed to
///     Core as zero, and an abandoned async send is never resubmitted.
/// </summary>
public sealed class test_binding_core_parity
{
    private const ulong RecordHwm = 65_536UL + 64UL;
    private static readonly string FillerPayload =
        "filler" + new string('p', 65_536);

    [Fact]
    public async Task request_without_timeout_uses_the_socket_request_timeout_option()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.RequestTimeout = TimeSpan.FromMilliseconds(300);
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "parity-request-timeout");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using var completions = new CompletionPollerDriver(dealer);

        using Message part = Message.From("never-answered");
        var watch = System.Diagnostics.Stopwatch.StartNew();
        Task<IReadOnlyList<Message>> reply = dealer.Request().Message(part)
            .Async().Reply;
        ZlinkRequestException error = await Assert.ThrowsAsync<
            ZlinkRequestException>(() => reply.WaitAsync(
                TimeSpan.FromSeconds(3)));
        watch.Stop();
        Assert.Equal(ZlinkRequestException.ErrorCode.TimedOut, error.Result);
        // The binding has no 5 s default of its own; Core applied the option.
        Assert.True(watch.Elapsed < TimeSpan.FromSeconds(3));
    }

    [Fact]
    public async Task abandoned_async_send_is_not_resubmitted()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "parity-abandoned-send");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        using (Message handshake = Message.From("handshake"))
            dealer.Send().Message(handshake).Submit();
        using (Received first = Received.Create())
            Assert.True(router.Recv(first));
        using var completions = new CompletionPollerDriver(dealer);

        using var cancellation = new CancellationTokenSource();
        Task? abandoned = null;
        var accepted = 0;
        for (var attempt = 0; attempt < 16 && abandoned is null; attempt++)
        {
            using Message candidate = Message.From(FillerPayload + attempt);
            SendSubmission submission = dealer.Send().Message(candidate)
                .Async(cancellation.Token);
            if (submission.Result == SubmitResult.Ok)
            {
                accepted++;
                continue;
            }
            abandoned = submission.Admitted;
        }
        Assert.NotNull(abandoned);

        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => abandoned!.WaitAsync(TimeSpan.FromSeconds(3)));

        // Freeing the router queue makes Core publish WRITABLE for the token.
        // The binding ignores it and must not send the abandoned message.
        for (var i = 0; i < accepted; i++)
        {
            using Received received = Received.Create();
            Assert.True(router.Recv(received));
        }
        using var poller = Zlink.CreatePoller();
        poller.Add(router, PollEventFlags.PollIn, 1);
        Assert.Equal(0, poller.Wait(new PollEvent[1],
            TimeSpan.FromMilliseconds(500)));
    }

    [Fact]
    public void abandoned_send_waiting_to_retry_releases_the_message_without_resubmitting()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "parity-abandoned-retry");
        router.Bind(endpoint);
        dealer.Connect(endpoint);

        Type ownerType = CompletionOwnerTestAccess.RuntimeType(
            "Systems.Zlink.CompletionOwner");
        object owner = CompletionOwnerTestAccess.Owner(dealer);
        CompletionOwnerTestAccess.Invoke(owner, "TransferToPublic", new object());
        using var cancellation = new CancellationTokenSource();
        Type entryType = ownerType.GetNestedType("SendCompletionEntry",
            System.Reflection.BindingFlags.NonPublic)!;
        object entry = CompletionOwnerTestAccess.Create(entryType, owner,
            null, cancellation.Token);
        CompletionOwnerTestAccess.Invoke(owner, "Register", entry, IntPtr.Zero);
        Message abandoned = Message.From("abandoned");
        CompletionOwnerTestAccess.Invoke(entry, "Arm", 73UL,
            new[] { abandoned });
        Type stateType = entryType.Assembly.GetType(
            "Systems.Zlink.CompletionOwner+SendEntryState")!;
        CompletionOwnerTestAccess.SetField(entry, "_state",
            Enum.Parse(stateType, "Retrying"));

        cancellation.Cancel();
        CompletionOwnerTestAccess.Invoke(entry, "Retry");

        using var poller = Zlink.CreatePoller();
        poller.Add(router, PollEventFlags.PollIn, 1);
        Assert.Equal(0, poller.Wait(new PollEvent[1],
            TimeSpan.FromMilliseconds(300)));
        Assert.Empty(CompletionOwnerTestAccess.Entries(owner));
    }
}
