using Xunit;

namespace Systems.Zlink.Tests;

/// <summary>
///     Contract B (0.17.0) regressions: one DONTWAIT attempt, wait token plus
///     WRITABLE retry of the same packet, immediate NOT_CONNECTED for a
///     ROUTER RID with no route, and public poller wait errors that do not
///     settle live waiters.
/// </summary>
public sealed class test_contract_b_regressions
{
    private const ulong RecordHwm = 65_536UL + 64UL;
    private static readonly string FillerPayload =
        "filler" + new string('b', 65_536);

    [Fact]
    public async Task router_no_route_reports_not_connected_immediately()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        using var router = context.CreateRouterSocket();
        // Core 0.17.0 still drops a no-route RID silently when MANDATORY is
        // off; the immediate NOT_CONNECTED contract is observable with it on.
        router.Options.Mandatory = true;
        RoutingId unknown = RoutingId.From("no-such-peer"u8);

        using Message asyncMessage = Message.From("async-no-route");
        ZlinkSubmitException asyncError = await Assert.ThrowsAsync<
            ZlinkSubmitException>(() =>
            router.Send(unknown).Message(asyncMessage).Async());
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected,
            asyncError.Result);

        // TrySubmit reports only BACKPRESSURED as false; a missing route is
        // an immediate failure with no wait token.
        using Message tryMessage = Message.From("try-no-route");
        ZlinkSubmitException tryError = Assert.Throws<ZlinkSubmitException>(
            () => router.Send(unknown).Message(tryMessage).TrySubmit());
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected,
            tryError.Result);
        Assert.Equal("try-no-route", tryMessage.GetString());
    }

    [Fact]
    public async Task rebackpressured_retry_keeps_waiting_and_delivers_each_record_once()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "contract-b-rebackpressure");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Handshake(dealer, router);

        // Two records wait behind the same target. The first WRITABLE edge
        // admits one; the other retry is back-pressured again and must keep
        // waiting on its new token until the next edge.
        var accepted = new List<string>();
        var pending = new Dictionary<string, Task>();
        for (var attempt = 0; attempt < 32 && pending.Count < 2; attempt++)
        {
            string payload = FillerPayload + $"-{attempt:D2}";
            using Message candidate = Message.From(payload);
            Task submitted = dealer.Send().Message(candidate).Async();
            if (submitted.IsCompleted)
            {
                await submitted;
                accepted.Add(payload);
                continue;
            }

            pending.Add(payload, submitted);
        }

        Assert.Equal(2, pending.Count);

        foreach (string expected in accepted)
        {
            using Received filler = Received.Create();
            Assert.True(router.Recv(filler));
            Assert.Equal(expected, filler.SinglePartOrThrow().GetString());
        }

        var delivered = new List<string>();
        for (var index = 0; index < 2; index++)
        {
            using Received retried = Received.Create();
            Assert.True(router.Recv(retried));
            delivered.Add(retried.SinglePartOrThrow().GetString());
        }

        foreach (Task task in pending.Values)
            await task.WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(pending.Keys.OrderBy(key => key, StringComparer.Ordinal),
            delivered.OrderBy(key => key, StringComparer.Ordinal));

        using Received duplicate = Received.Create();
        Assert.False(router.Recv(duplicate, RecvFlags.DontWait));
    }

    [Fact]
    public async Task try_submit_wait_token_is_retired_by_socket_close()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "contract-b-try-submit-close");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Handshake(dealer, router);

        Message? blocked = null;
        var acceptedCount = 0;
        for (var attempt = 0; attempt < 16 && blocked is null; attempt++)
        {
            Message candidate = Message.From(FillerPayload);
            if (dealer.Send().Message(candidate).TrySubmit())
            {
                acceptedCount++;
                candidate.Dispose();
                continue;
            }

            blocked = candidate;
        }

        Assert.NotNull(blocked);
        using (blocked)
        {
            Assert.True(acceptedCount > 0);
            Assert.Equal(FillerPayload, blocked!.GetString());

            // The live wait token belongs to Core; closing the socket retires
            // it and must neither block nor throw.
            await Task.Run(dealer.Dispose).WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(FillerPayload, blocked.GetString());
        }

        for (var index = 0; index < acceptedCount; index++)
        {
            using Received filler = Received.Create();
            Assert.True(router.Recv(filler));
        }
    }

    [Fact]
    public async Task concurrent_wait_error_does_not_fail_pending_writable_waiter()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var context = Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        using var dealer = context.CreateDealerSocket();
        using var router = context.CreateRouterSocket();
        dealer.Options.SendHighWaterMark = RecordHwm;
        router.Options.ReceiveHighWaterMark = RecordHwm;

        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "contract-b-busy-wait");
        router.Bind(endpoint);
        dealer.Connect(endpoint);
        Handshake(dealer, router);

        using var poller = Zlink.CreatePoller();
        poller.Add(dealer,
            PollEventFlags.PollOut | PollEventFlags.PollCompletion, 7);

        var accepted = new List<string>();
        Task? pending = null;
        string pendingPayload = string.Empty;
        for (var attempt = 0; attempt < 16 && pending is null; attempt++)
        {
            string payload = FillerPayload + $"-{attempt:D2}";
            using Message candidate = Message.From(payload);
            Task submitted = dealer.Send().Message(candidate).Async();
            if (submitted.IsCompleted)
            {
                await submitted;
                accepted.Add(payload);
                continue;
            }

            pending = submitted;
            pendingPayload = payload;
        }

        Assert.NotNull(pending);

        // One thread owns a long Wait; a second Wait on the same poller is a
        // Core EBUSY error. That error must surface to its caller only and
        // leave the armed WRITABLE waiter intact.
        // Exactly one side of an overlapping pair gets EBUSY. The owner
        // retries until the probe has observed the error while the owner's
        // wait stayed active; the probe polls with a zero timeout only.
        var ownerEvents = new PollEvent[1];
        var busyObserved = 0;
        Task<int> owner = Task.Run(() =>
        {
            while (true)
            {
                try
                {
                    return poller.Wait(ownerEvents, TimeSpan.FromSeconds(10));
                }
                catch (ZlinkConfigException)
                    when (Volatile.Read(ref busyObserved) == 0)
                {
                }
            }
        });

        var busyEvents = new PollEvent[1];
        ZlinkConfigException? busy = null;
        var deadline = Environment.TickCount64 + 5_000;
        var lastWait = 0;
        while (busy is null && Environment.TickCount64 < deadline)
        {
            try
            {
                lastWait = poller.Wait(busyEvents, TimeSpan.Zero);
                if (lastWait != 0)
                    break;
            }
            catch (ZlinkConfigException error)
            {
                busy = error;
                Volatile.Write(ref busyObserved, 1);
            }
        }

        Assert.True(busy is not null,
            $"owner={owner.Status} lastWait={lastWait} pending={pending!.Status}");
        Assert.False(pending!.IsCompleted);

        foreach (string expected in accepted)
        {
            using Received filler = Received.Create();
            Assert.True(router.Recv(filler));
            Assert.Equal(expected, filler.SinglePartOrThrow().GetString());
        }

        Assert.Equal(1, await owner.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal((nuint)7, ownerEvents[0].Slot);
        await pending.WaitAsync(TimeSpan.FromSeconds(5));

        using Received retried = Received.Create();
        Assert.True(router.Recv(retried));
        Assert.Equal(pendingPayload, retried.SinglePartOrThrow().GetString());
    }

    private static void Handshake(IDealerSocket dealer, IRouterSocket router)
    {
        using Message handshake = Message.From("handshake");
        dealer.Send().Message(handshake).Submit();
        using Received received = Received.Create();
        Assert.True(router.Recv(received));
        Assert.Equal("handshake", received.SinglePartOrThrow().GetString());
    }
}
