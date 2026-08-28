using System.Collections.Generic;
using System.Threading;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_pair_tcp
{
    [Theory]
    [InlineData("tcp")]
    [InlineData("ipc")]
    [InlineData("inproc")]
    public void pair_roundtrip(string transport)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported(transport))
            return;

        using var ctx = Zlink.CreateContext();
        using var sb = ctx.CreatePairSocket();
        using var sc = ctx.CreatePairSocket();

        string endpoint = CoreTestSupport.NewEndpoint(transport, "pair");
        sb.Bind(endpoint);
        sc.Connect(endpoint);
        Thread.Sleep(50);

        CoreTestSupport.SendWithRetry(sc, "ping"u8, 2000);
        Assert.Equal("ping", CoreTestSupport.ReceiveUtf8WithTimeout(sb, 2000));

        CoreTestSupport.SendWithRetry(sb, "pong"u8, 2000);
        Assert.Equal("pong", CoreTestSupport.ReceiveUtf8WithTimeout(sc, 2000));
    }

    [Theory]
    [InlineData("tcp")]
    [InlineData("ipc")]
    [InlineData("inproc")]
    public void pair_multipart_roundtrip(string transport)
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported(transport))
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        using var client = ctx.CreatePairSocket();

        string endpoint = CoreTestSupport.NewEndpoint(transport,
            "pair-multipart");
        server.Bind(endpoint);
        client.Connect(endpoint);
        Thread.Sleep(50);

        using Message part1 = Message.From("hello");
        using Message part2 = Message.From("world");
        Assert.True(client.Send().Message(part1).Message(part2)
            .TrySubmit(SendFlags.DontWait));

        var received = Received.Create();
        server.Recv(received);
        try
        {
            Assert.Equal(2, received.Parts.Count);
            Assert.Equal("hello", received.Parts[0].GetString());
            Assert.Equal("world", received.Parts[1].GetString());
        }
        finally
        {
            foreach (Message part in received.Parts)
                part.Dispose();
        }
    }

    [Fact]
    public void pair_tcp_connect_by_name_localhost()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sb = ctx.CreatePairSocket();
        using var sc = ctx.CreatePairSocket();

        int port = CoreTestSupport.ExtractPort(CoreTestSupport.NewEndpoint("tcp",
            "pair-name"));
        string bindEndpoint = $"tcp://127.0.0.1:{port}";
        string connectEndpoint = $"tcp://localhost:{port}";

        sb.Bind(bindEndpoint);
        sc.Connect(connectEndpoint);
        Thread.Sleep(50);

        CoreTestSupport.SendWithRetry(sc, "hello"u8, 2000);
        Assert.Equal("hello", CoreTestSupport.ReceiveUtf8WithTimeout(sb, 2000));
    }

    [Fact]
    public void poller_wait_span_api_reports_ready_count()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "pair-poller-span");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        using var poller = Zlink.CreatePoller();
        poller.Add(receiver, PollEventFlags.PollIn, 7);

        CoreTestSupport.SendWithRetry(sender, "x"u8, 2000);

        var events = new PollEvent[4];
        int written = poller.Wait(events, TimeSpan.FromMilliseconds(2000));
        Assert.True(written > 0);
        Assert.Equal(PollSourceKind.Socket, events[0].SourceKind);
        Assert.Equal((nuint)7, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None, events[0].Revents & PollEventFlags.PollIn);
    }

    [Fact]
    public void poller_clear_removes_registered_items()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "pair-poller-clear");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        poller.Add(receiver, PollEventFlags.PollIn, 1);
        Assert.Equal(1, poller.Size);

        poller.Clear();

        Assert.Equal(0, poller.Size);
        var clearEvents = new PollEvent[1];
        Assert.Equal(0, poller.Wait(clearEvents, TimeSpan.Zero));
    }

    [Fact]
    public void poller_modify_switches_socket_event_mask()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-poller-modify");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        poller.Add(receiver, PollEventFlags.PollIn, 11);
        Assert.Equal(1, poller.Size);

        CoreTestSupport.SendWithRetry(sender, "ping"u8, 2000);

        var events = new PollEvent[1];
        int written = poller.Wait(events, TimeSpan.FromMilliseconds(2000));
        Assert.Equal(1, written);
        Assert.Equal((nuint)11, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollIn);

        Assert.Equal("ping", CoreTestSupport.ReceiveUtf8WithTimeout(receiver,
            2000));

        poller.Modify(receiver, PollEventFlags.PollOut);

        CoreTestSupport.SendWithRetry(sender, "pong"u8, 2000);

        // POLLOUT is send-recovery readiness, not generic transport writability.
        // After switching away from POLLIN, queued receive data must not surface.
        Array.Clear(events);
        written = poller.Wait(events, TimeSpan.FromMilliseconds(100));
        Assert.Equal(0, written);

        poller.Modify(receiver, PollEventFlags.PollIn);
        written = poller.Wait(events, TimeSpan.FromMilliseconds(2000));
        Assert.Equal(1, written);
        Assert.Equal((nuint)11, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollIn);
        Assert.Equal(PollEventFlags.None,
            events[0].Revents & PollEventFlags.PollOut);
        Assert.Equal("pong", CoreTestSupport.ReceiveUtf8WithTimeout(receiver,
            2000));
    }

    [Fact]
    public void poller_wait_reports_multiple_ready_sources_and_respects_capacity()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender1 = ctx.CreatePairSocket();
        using var receiver1 = ctx.CreatePairSocket();
        using var sender2 = ctx.CreatePairSocket();
        using var receiver2 = ctx.CreatePairSocket();
        using var poller = Zlink.CreatePoller();

        string endpoint1 = CoreTestSupport.NewEndpoint("inproc",
            "pair-poller-multi-a");
        string endpoint2 = CoreTestSupport.NewEndpoint("inproc",
            "pair-poller-multi-b");
        sender1.Bind(endpoint1);
        receiver1.Connect(endpoint1);
        sender2.Bind(endpoint2);
        receiver2.Connect(endpoint2);
        Thread.Sleep(50);

        poller.Add(receiver1, PollEventFlags.PollIn, 101);
        poller.Add(receiver2, PollEventFlags.PollIn, 102);
        CoreTestSupport.SendWithRetry(sender1, "a"u8, 2000);
        CoreTestSupport.SendWithRetry(sender2, "b"u8, 2000);

        Span<PollEvent> one = stackalloc PollEvent[1];
        int written = poller.Wait(one, TimeSpan.FromMilliseconds(2000));
        Assert.Equal(1, written);
        Assert.True(one[0].Slot is 101 or 102);
        Assert.NotEqual(PollEventFlags.None,
            one[0].Revents & PollEventFlags.PollIn);

        if (one[0].Slot == 101)
            Assert.Equal("a", CoreTestSupport.ReceiveUtf8WithTimeout(receiver1,
                2000));
        else
            Assert.Equal("b", CoreTestSupport.ReceiveUtf8WithTimeout(receiver2,
                2000));

        Span<PollEvent> remaining = stackalloc PollEvent[2];
        written = poller.Wait(remaining, TimeSpan.FromMilliseconds(2000));
        Assert.Equal(1, written);
        Assert.NotEqual(one[0].Slot, remaining[0].Slot);

        if (remaining[0].Slot == 101)
            Assert.Equal("a", CoreTestSupport.ReceiveUtf8WithTimeout(receiver1,
                2000));
        else
            Assert.Equal("b", CoreTestSupport.ReceiveUtf8WithTimeout(receiver2,
                2000));

        CoreTestSupport.SendWithRetry(sender1, "c"u8, 2000);
        CoreTestSupport.SendWithRetry(sender2, "d"u8, 2000);

        Span<PollEvent> both = stackalloc PollEvent[2];
        written = poller.Wait(both, TimeSpan.FromMilliseconds(2000));
        Assert.Equal(2, written);
        Assert.Contains((nuint)101, new[] { both[0].Slot, both[1].Slot });
        Assert.Contains((nuint)102, new[] { both[0].Slot, both[1].Slot });
    }

    [Fact]
    public void poller_remove_suppresses_ready_events()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-poller-remove");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        poller.Add(receiver, PollEventFlags.PollIn, 31);
        Assert.True(poller.Remove(receiver));
        CoreTestSupport.SendWithRetry(sender, "removed"u8, 2000);

        Span<PollEvent> events = stackalloc PollEvent[1];
        Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));
    }

    [Fact]
    public void poller_wait_distinguishes_timer_and_socket_in_same_buffer()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        using var timer = Zlink.CreateTimer();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-poller-timer-socket");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        poller.Add(receiver, PollEventFlags.PollIn, 41);
        poller.Add(timer, 42);
        CoreTestSupport.SendWithRetry(sender, "socket"u8, 2000);
        timer.Start(TimeSpan.FromMilliseconds(5), 1);

        Span<PollEvent> events = stackalloc PollEvent[2];
        bool sawSocket = false;
        bool sawTimer = false;
        DateTime deadline = DateTime.UtcNow.AddSeconds(2);
        while ((!sawSocket || !sawTimer) && DateTime.UtcNow < deadline)
        {
            int written = poller.Wait(events, TimeSpan.FromMilliseconds(200));
            for (int i = 0; i < written; i++)
            {
                if (events[i].SourceKind == PollSourceKind.Socket)
                {
                    Assert.Equal((nuint)41, events[i].Slot);
                    Assert.Equal("socket",
                        CoreTestSupport.ReceiveUtf8WithTimeout(receiver, 2000));
                    sawSocket = true;
                }
                else if (events[i].SourceKind == PollSourceKind.Timer)
                {
                    Assert.Equal((nuint)42, events[i].Slot);
                    Assert.Equal(1UL, timer.Recv().GetValueOrDefault());
                    sawTimer = true;
                }
            }
        }

        Assert.True(sawSocket);
        Assert.True(sawTimer);
    }

    [Fact]
    public void poller_wait_span_does_not_allocate_after_buffer_warmup()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        using var poller = Zlink.CreatePoller();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-poller-no-alloc");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        poller.Add(receiver, PollEventFlags.PollIn, 51);
        Span<PollEvent> events = stackalloc PollEvent[1];
        Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));

        CoreTestSupport.SendWithRetry(sender, "ready"u8, 2000);
        long before = GC.GetAllocatedBytesForCurrentThread();
        int written = poller.Wait(events, TimeSpan.FromMilliseconds(2000));
        long after = GC.GetAllocatedBytesForCurrentThread();

        Assert.Equal(1, written);
        Assert.Equal(before, after);
        Assert.Equal((nuint)51, events[0].Slot);
    }

    [Fact]
    public void receive_dontwait_returns_null_on_empty_queue()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-try-recv-code");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        var probe = Received.Create();
        Assert.False(receiver.Recv(probe, RecvFlags.DontWait));
    }

    [Fact]
    public void pair_direct_receive_path_receives_messages()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-recv-handler");
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        CoreTestSupport.SendWithRetry(sender, "pair-direct"u8, 2000);
        Assert.Equal("pair-direct", CoreTestSupport.ReceiveUtf8WithTimeout(
            receiver, 2000));
    }

    [Fact]
    public void socket_monitor_attach_handler_snapshot_and_close_work()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreatePairSocket();
        using var client = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp",
            "pair-monitor-shape");
        server.Bind(endpoint);

        using ISocketMonitor monitor = server.MonitorOpen(SocketEvent.ConnectionReady
            | SocketEvent.Disconnected);
        int callbackCount = 0;
        monitor.OnEvent(_ => Interlocked.Increment(ref callbackCount));

        client.Connect(endpoint);
        Thread.Sleep(50);

        MonitorStatus snapshot = monitor.Status();
        Assert.Equal<MonitorSourceKind>(MonitorSourceKind.Socket, snapshot.SourceKind);
        Assert.True(snapshot.SndPendingMsgs >= 0);

        Assert.True(CoreTestSupport.WaitUntil(() =>
            Volatile.Read(ref callbackCount) >= 1, 15000, 10));

        monitor.Close();
        Assert.Throws<ObjectDisposedException>(() => monitor.Status());
    }

    [Fact]
    public void send_nonblocking_reports_backpressured_when_pair_hwm_is_exhausted()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-try-send-backpressured");

        sender.Options.SendHighWaterMark = 1;
        receiver.Options.ReceiveHighWaterMark = 1;
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        bool sent = true;
        byte[] payloadBytes = new byte[64 * 1024];
        for (int i = 0; i < 16 * 1024; i++)
        {
            using Message payload = Message.From(payloadBytes);
            sent = sender.Send().Message(payload)
                .TrySubmit(SendFlags.DontWait);
            if (!sent)
                break;
        }

        Assert.False(sent);
    }

    [Fact]
    public void recv_part_reuses_message_storage_and_reports_multipart_boundary()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-recv-part-reuse");

        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        using Message first = Message.From("first"u8.ToArray());
        using Message second = Message.From("second"u8.ToArray());
        sender.Send().Message(first).Message(second).Submit(SendFlags.None);

        using var received = Received.Create();
        Assert.True(CoreTestSupport.WaitUntil(
            () => receiver.Recv(received, RecvFlags.DontWait),
            5000,
            10));
        Assert.Equal(2, received.Parts.Count);
        Assert.Equal("first",
            System.Text.Encoding.UTF8.GetString(
                received.Parts[0].AsReadOnlySpan()));
        Assert.Equal("second",
            System.Text.Encoding.UTF8.GetString(
                received.Parts[1].AsReadOnlySpan()));

        Assert.False(receiver.Recv(received, RecvFlags.DontWait));
    }

    [Fact]
    public void send_operation_rejects_second_submit_after_state_transition()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-send-once");

        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        using Message payload = Message.From("once");
        RoutedSendSubmitOperation operation = sender.Send().Message(payload);
        operation.Submit(SendFlags.None);
        Assert.Throws<ZlinkConfigException>(() =>
            operation.Submit(SendFlags.None));
    }

    [Fact]
    public void from_bytes_nonblocking_send_completes_borrowed_lifetime()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-from-bytes-borrowed-lifetime");

        sender.Options.SendHighWaterMark = 4096;
        receiver.Options.ReceiveHighWaterMark = 4096;
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        byte[] payloadBytes = new byte[64];
        using var received = Received.Create();
        for (int i = 0; i < 2048; i++)
        {
            payloadBytes[0] = unchecked((byte)i);
            bool sent = false;
            while (!sent)
            {
                using Message payload = Message.From(payloadBytes);
                sent = sender.Send()
                    .Message(payload)
                    .TrySubmit(SendFlags.DontWait);
                while (receiver.Recv(received, RecvFlags.DontWait))
                {
                }
            }
        }
    }

    [Fact]
    public void send_nonblocking_returns_false_only_for_backpressured_pair_queue()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var sender = ctx.CreatePairSocket();
        using var receiver = ctx.CreatePairSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc",
            "pair-public-try-send-backpressured");

        sender.Options.SendHighWaterMark = 1;
        receiver.Options.ReceiveHighWaterMark = 1;
        sender.Bind(endpoint);
        receiver.Connect(endpoint);
        Thread.Sleep(50);

        bool sent = true;
        byte[] payloadBytes = new byte[64 * 1024];
        for (int i = 0; i < 16 * 1024; i++)
        {
            using Message payload = Message.From(payloadBytes);
            sent = sender.Send().Message(payload)
                .TrySubmit(SendFlags.DontWait);
            if (!sent)
                break;
        }

        Assert.False(sent);
    }

    [Fact]
    public async Task routed_async_reports_not_connected_for_unknown_router_peer()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var router = ctx.CreateRouterSocket();
        router.Options.Mandatory = true;

        using Message message = Message.From("no-route");
        var ex = await Assert.ThrowsAsync<ZlinkSubmitException>(() =>
            router.Send(RoutingId.From("UNKNOWN"u8)).Message(message).Async());
        Assert.Equal(ZlinkSubmitException.ErrorCode.NotConnected, ex.Result);
    }
}
