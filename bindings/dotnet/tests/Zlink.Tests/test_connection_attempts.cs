using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;
using Xunit;
using Xunit.Abstractions;

namespace Systems.Zlink.Tests;

public sealed class test_connection_attempts
{
    private const int WaitMs = 3000;
    private readonly ITestOutputHelper _output;

    public test_connection_attempts(ITestOutputHelper output) => _output = output;

    [Fact]
    public void configured_core_must_export_snapshot_before_socket_creation()
    {
        if (Environment.GetEnvironmentVariable("ZLINK_TEST_EXPECT_MISSING_SNAPSHOT") == "1")
        {
            var initializer = Assert.Throws<TypeInitializationException>(
                () => { using var ctx = Zlink.CreateContext(); });
            var failure = Assert.IsType<DllNotFoundException>(initializer.InnerException);
            Assert.Contains("zlink_socket_connection_attempts_snapshot", failure.Message);
            _output.WriteLine(failure.Message);
            return;
        }

        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var context = Zlink.CreateContext();
    }

    [Fact]
    public void native_layout_and_closed_enum_values_match_core_abi()
    {
        Assert.Equal(16, Marshal.SizeOf<ZlinkConnectionFailure>());
        foreach (var (name, offset) in new[] { (nameof(ZlinkConnectionFailure.Kind), 0),
                     (nameof(ZlinkConnectionFailure.Stage), 4),
                     (nameof(ZlinkConnectionFailure.NormalizedErrno), 8),
                     (nameof(ZlinkConnectionFailure.DisconnectReason), 12) })
            Assert.Equal((IntPtr)offset, Marshal.OffsetOf<ZlinkConnectionFailure>(name));
        if (IntPtr.Size == 8)
        {
            Assert.Equal(64, Marshal.SizeOf<ZlinkConnectionAttempt>());
            foreach (var (name, offset) in new[] { (nameof(ZlinkConnectionAttempt.EndpointOffset), 0),
                         (nameof(ZlinkConnectionAttempt.EndpointLength), 8),
                         (nameof(ZlinkConnectionAttempt.IntentId), 16),
                         (nameof(ZlinkConnectionAttempt.Revision), 24),
                         (nameof(ZlinkConnectionAttempt.LastFailureId), 32),
                         (nameof(ZlinkConnectionAttempt.State), 40),
                         (nameof(ZlinkConnectionAttempt.LastFailure), 44) })
                Assert.Equal((IntPtr)offset, Marshal.OffsetOf<ZlinkConnectionAttempt>(name));
        }
        Assert.Equal(typeof(uint), Enum.GetUnderlyingType(typeof(ConnectionAttemptState)));
        Assert.Equal(typeof(uint), Enum.GetUnderlyingType(typeof(ConnectionFailureKind)));
        Assert.Equal(typeof(uint), Enum.GetUnderlyingType(typeof(ConnectionFailureStage)));
        Assert.Equal(new uint[] { 0, 1, 2 }, Enum.GetValues<ConnectionAttemptState>().Select(value => (uint)value));
        Assert.Equal(new uint[] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 },
            Enum.GetValues<ConnectionFailureKind>().Select(value => (uint)value));
        Assert.Equal(new uint[] { 0, 1, 2, 3, 4, 5, 6 },
            Enum.GetValues<ConnectionFailureStage>().Select(value => (uint)value));
        Assert.Equal(new[] { "Connecting", "Connected", "AttemptFailed" }, Enum.GetNames<ConnectionAttemptState>());
        Assert.Equal(new[] { "None", "Refused", "EndpointMissing", "TimedOut", "Unreachable",
            "NameResolution", "AuthFailed", "ProtocolFailed", "ConnectionLost", "Other" }, Enum.GetNames<ConnectionFailureKind>());
        Assert.Equal(new[] { "None", "Resolve", "Transport", "Tls", "WebSocket", "CoreHandshake", "Established" },
            Enum.GetNames<ConnectionFailureStage>());
        Assert.Equal(128, (int)PollEventFlags.PollConnect);
    }

    [Fact]
    public void empty_snapshot_and_same_endpoint_intents_preserve_utf8_and_resize_both_buffers()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        using var client = ctx.CreateDealerSocket();
        client.Options.Linger = TimeSpan.Zero;
        Assert.Empty(client.ConnectionAttemptsSnapshot());
        // Long, distinct UTF-8 endpoints exceed the initial arena and row capacities.
        string prefix = CoreTestSupport.NewEndpoint("inproc", "연결-" + new string('x', 100));
        var endpoints = Enumerable.Range(0, 40).Select(index => prefix + index).ToArray();
        foreach (string endpoint in endpoints)
            client.Connect(endpoint);
        client.Connect(endpoints[0]);
        var rows = client.ConnectionAttemptsSnapshot();
        Assert.Equal(endpoints.Length + 1, rows.Count);
        Assert.Equal(endpoints.OrderBy(value => value).Append(endpoints[0]).OrderBy(value => value),
            rows.Select(row => row.Endpoint).OrderBy(value => value));
        Assert.Equal(rows.Count, rows.Select(row => row.IntentId).Distinct().Count());
        Assert.All(rows, row =>
        {
            Assert.NotEqual(0UL, row.IntentId);
            Assert.NotEqual(0UL, row.Revision);
            Assert.Equal(0UL, row.LastFailureId);
            Assert.Equal(ConnectionAttemptState.Connecting, row.State);
            Assert.Equal(default, row.LastFailure);
        });
        client.Disconnect(endpoints[0]);
        Assert.DoesNotContain(client.ConnectionAttemptsSnapshot(), row => row.Endpoint == endpoints[0]);
    }

    [Fact]
    public void refused_failure_recovery_and_disconnect_keep_core_history_and_pollconnect_levels()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        using var server = ctx.CreateRouterSocket();
        using var client = ctx.CreateDealerSocket();
        server.Options.Linger = client.Options.Linger = TimeSpan.Zero;
        server.Bind("tcp://127.0.0.1:*");
        string endpoint = server.Options.LastEndpoint;
        server.Unbind(endpoint);
        using var poller = Zlink.CreatePoller();
        poller.Add(client, PollEventFlags.PollConnect, 17);
        Assert.Empty(client.ConnectionAttemptsSnapshot());
        client.Connect(endpoint);
        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromMilliseconds(WaitMs)));
        Assert.Equal((nuint)17, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None, events[0].Revents & PollEventFlags.PollConnect);
        ConnectionAttempt failed = WaitState(client, poller, ConnectionAttemptState.AttemptFailed);
        Assert.Equal(endpoint, failed.Endpoint);
        Assert.Equal(ConnectionFailureKind.Refused, failed.LastFailure.Kind);
        Assert.Equal(ConnectionFailureStage.Transport, failed.LastFailure.Stage);
        Assert.Equal(CoreTestSupport.Errno("ECONNREFUSED"), failed.LastFailure.NormalizedErrno);
        Assert.Equal(0U, failed.LastFailure.DisconnectReason);
        Assert.NotEqual(0UL, failed.LastFailureId);
        ulong firstFailureId = failed.LastFailureId;
        long retryDeadline = Environment.TickCount64 + WaitMs;
        do
        {
            long remaining = retryDeadline - Environment.TickCount64;
            Assert.True(remaining > 0, "No new connection failure before the deadline");
            Assert.Equal(1, poller.Wait(events, TimeSpan.FromMilliseconds(remaining)));
            Assert.NotEqual(PollEventFlags.None, events[0].Revents & PollEventFlags.PollConnect);
            failed = Assert.Single(client.ConnectionAttemptsSnapshot());
            Assert.Equal(ConnectionAttemptState.AttemptFailed, failed.State);
        } while (failed.LastFailureId == firstFailureId && Environment.TickCount64 < retryDeadline);
        Assert.NotEqual(firstFailureId, failed.LastFailureId);
        // Recovery must leave a nonzero failure id, including when retry failures
        // race the bind. A recovered row must still carry the latest failure.
        server.Bind(endpoint);
        ConnectionAttempt connected = WaitState(client, poller, ConnectionAttemptState.Connected);
        Assert.Equal(failed.IntentId, connected.IntentId);
        Assert.NotEqual(failed.Revision, connected.Revision);
        Assert.NotEqual(0UL, connected.LastFailureId);
        Assert.Equal(ConnectionFailureKind.Refused, connected.LastFailure.Kind);
        Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));
        server.Unbind(endpoint);
        ConnectionAttempt lost = WaitState(client, poller, ConnectionAttemptState.AttemptFailed);
        Assert.Equal(ConnectionFailureKind.ConnectionLost, lost.LastFailure.Kind);
        Assert.Equal(ConnectionFailureStage.Established, lost.LastFailure.Stage);
        Assert.NotEqual(connected.LastFailureId, lost.LastFailureId);
        client.Disconnect(endpoint);
        Assert.Equal(1, poller.Wait(events, TimeSpan.Zero));
        Assert.NotEqual(PollEventFlags.None, events[0].Revents & PollEventFlags.PollConnect);
        Assert.Empty(client.ConnectionAttemptsSnapshot());
        Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));
        client.Connect(endpoint);
        Assert.NotEqual(failed.IntentId, Assert.Single(client.ConnectionAttemptsSnapshot()).IntentId);
    }

    [Fact]
    public void ipc_missing_is_normal_snapshot_data_and_query_errors_remain_config_errors()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        using var client = ctx.CreateDealerSocket();
        client.Options.Linger = TimeSpan.Zero;
        if (Zlink.Has("ipc"))
        {
            using var poller = Zlink.CreatePoller();
            poller.Add(client, PollEventFlags.PollConnect, 1);
            string endpoint = "ipc:///tmp/zlink-1710-" + Guid.NewGuid().ToString("N") + "/연결";
            client.Connect(endpoint);
            ConnectionAttempt failed = WaitState(client, poller, ConnectionAttemptState.AttemptFailed);
            Assert.Equal(endpoint, failed.Endpoint);
            Assert.Equal(ConnectionFailureKind.EndpointMissing, failed.LastFailure.Kind);
            Assert.Equal(ConnectionFailureStage.Transport, failed.LastFailure.Stage);
            Assert.Equal(CoreTestSupport.Errno("ENOENT"), failed.LastFailure.NormalizedErrno);
        }
        // Context shutdown is a native query error, unlike an already disposed
        // managed handle (which the existing SocketHandle rejects before P/Invoke).
        ctx.Shutdown();
        var error = Assert.Throws<ZlinkConfigException>(() => client.ConnectionAttemptsSnapshot());
        Assert.Equal(ZlinkConfigException.ErrorCode.InternalError, error.Result);
        Assert.Equal(CoreTestSupport.Errno("ETERM"), error.NativeErrno);
    }

    [Fact]
    public void raw_poll_preserves_pollconnect_and_successful_snapshot_clears_it()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        using var client = ctx.CreateDealerSocket();
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "raw-connect-poll");
        client.Connect(endpoint);
        var revents = new PollEventFlags[1];
        Assert.Equal(1, ZlinkPoll.Poll([client], [PollEventFlags.PollConnect], revents, 0));
        Assert.Equal(PollEventFlags.PollConnect, revents[0]);
        Assert.Equal(endpoint, Assert.Single(client.ConnectionAttemptsSnapshot()).Endpoint);
        Assert.Equal(0, ZlinkPoll.Poll([client], [PollEventFlags.PollConnect], revents, 0));
        Assert.Equal(PollEventFlags.None, revents[0]);
    }

    [Fact]
    public void all_connectable_socket_facades_project_empty_snapshots()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        IConnectableSocket[] sockets = [ctx.CreatePairSocket(),
            ctx.CreateDealerSocket(),
            ctx.CreatePubSocket(),
            ctx.CreateSubSocket(),
            ctx.CreateXPubSocket(),
            ctx.CreateXSubSocket(),
            ctx.CreateRouterSocket()];
        try
        {
            Assert.All(sockets, socket => Assert.Empty(socket.ConnectionAttemptsSnapshot()));
        }
        finally
        {
            foreach (var socket in sockets)
                socket.Dispose();
        }
    }

    [Fact]
    public void pollconnect_registration_does_not_acquire_a_completion_owner()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        using var client = ctx.CreateDealerSocket();
        using var poller = Zlink.CreatePoller();
        poller.Add(client, PollEventFlags.PollConnect, 1);
        using Message part = Message.From("ownerless");
        var error = Assert.Throws<ZlinkSubmitException>(() =>
            client.Request().Message(part).Async());
        Assert.Equal(ZlinkSubmitException.ErrorCode.InvalidState, error.Result);
        Assert.Equal("ownerless", part.GetString());
    }

    [Fact]
    public async Task combined_pollconnect_completion_and_writable_preserve_request_progress()
    {
        Assert.True(CoreTestSupport.IsNativeAvailable());
        using var ctx = Zlink.CreateContext();
        using var dealer = ctx.CreateDealerSocket();
        using var router = ctx.CreateRouterSocket();
        dealer.Options.Linger = router.Options.Linger = TimeSpan.Zero;
        string endpoint = CoreTestSupport.NewEndpoint("inproc", "connect-completion");
        router.Bind(endpoint);
        using var poller = Zlink.CreatePoller();
        poller.Add(dealer, PollEventFlags.PollConnect | PollEventFlags.PollCompletion | PollEventFlags.PollOut, 9);
        dealer.Connect(endpoint);
        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromMilliseconds(WaitMs)));
        Assert.NotEqual(PollEventFlags.None, events[0].Revents & PollEventFlags.PollConnect);
        Assert.Single(dealer.ConnectionAttemptsSnapshot());
        using Message request = Message.From("request");
        var pending = dealer.Request().Message(request).Timeout(TimeSpan.FromMilliseconds(WaitMs)).Async();
        using Received received = CoreTestSupport.ReceiveMessageWithTimeout(router, WaitMs);
        using Message reply = Message.From("reply");
        router.Reply(received.RoutingId ?? throw new InvalidOperationException("Missing route"), received.ReplyToken!).Message(reply).Submit();
        // Add a second intent before draining the reply to test combined readiness.
        dealer.Connect(CoreTestSupport.NewEndpoint("inproc", "pending-intent"));
        poller.Modify(dealer, PollEventFlags.PollConnect | PollEventFlags.PollCompletion);
        bool completion = false, connection = false;
        long deadline = Environment.TickCount64 + WaitMs;
        while (!completion && Environment.TickCount64 < deadline)
        {
            long remaining = deadline - Environment.TickCount64;
            Assert.Equal(1, poller.Wait(events, TimeSpan.FromMilliseconds(Math.Max(0, remaining))));
            connection |= (events[0].Revents & PollEventFlags.PollConnect) != 0;
            completion |= (events[0].Revents & PollEventFlags.PollCompletion) != 0;
            if ((events[0].Revents & PollEventFlags.PollConnect) != 0)
                Assert.Equal(2, dealer.ConnectionAttemptsSnapshot().Count);
        }
        Assert.True(connection);
        Assert.True(completion);
        await pending.Admitted;
        IReadOnlyList<Message> result = await pending.Reply.WaitAsync(TimeSpan.FromMilliseconds(WaitMs));
        Assert.Equal("reply", Assert.Single(result).GetString());
        Zlink.MultipartClose(result);
        Assert.Equal(0, poller.Wait(events, TimeSpan.Zero));
    }

    private static ConnectionAttempt WaitState(IConnectableSocket socket, IPoller poller,
        ConnectionAttemptState expected)
    {
        var events = new PollEvent[1];
        long deadline = Environment.TickCount64 + WaitMs;
        var row = Assert.Single(socket.ConnectionAttemptsSnapshot());
        while (row.State != expected)
        {
            long remaining = deadline - Environment.TickCount64;
            Assert.True(remaining > 0, $"Connection did not reach {expected}");
            Assert.Equal(1, poller.Wait(events, TimeSpan.FromMilliseconds(remaining)));
            Assert.NotEqual(PollEventFlags.None, events[0].Revents & PollEventFlags.PollConnect);
            row = Assert.Single(socket.ConnectionAttemptsSnapshot());
        }
        return row;
    }
}
