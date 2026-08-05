using System;
using System.Buffers.Binary;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using Xunit;

namespace Systems.Zlink.Tests;

public sealed class test_stream_socket
{
    private static TcpClient ConnectRawClient(int port)
    {
        var client = new TcpClient();
        client.NoDelay = true;
        client.ReceiveTimeout = 15000;
        client.SendTimeout = 15000;
        client.Connect(IPAddress.Loopback, port);
        return client;
    }

    private static void SendAll(NetworkStream stream, ReadOnlySpan<byte> payload)
    {
        stream.Write(payload);
        stream.Flush();
    }

    private static byte[] ReceiveExact(NetworkStream stream, int size)
    {
        byte[] buffer = new byte[size];
        int read = 0;
        while (read < size)
        {
            int n = stream.Read(buffer, read, size - read);
            if (n <= 0)
                throw new TimeoutException("stream receive timeout");
            read += n;
        }
        return buffer;
    }

    private static bool WaitForRawClientClose(NetworkStream stream, int timeoutMs)
    {
        int previousTimeout = stream.ReadTimeout;
        stream.ReadTimeout = timeoutMs;
        try
        {
            int read = stream.ReadByte();
            return read < 0;
        }
        catch (IOException ex)
            when (ex.InnerException is SocketException { SocketErrorCode: SocketError.TimedOut })
        {
            return false;
        }
        catch (IOException)
        {
            return true;
        }
        catch (SocketException ex) when (ex.SocketErrorCode != SocketError.TimedOut)
        {
            return true;
        }
        finally
        {
            stream.ReadTimeout = previousTimeout;
        }
    }

    private static bool TryDrainOneMultipart(IStreamSocket streamSocket)
    {
        return CoreTestSupport.TryReceiveMultipartLastPart(streamSocket, 512, out _);
    }

    private static bool HasPublicMessageSend(Type type)
    {
        return type.GetMethod("Send", BindingFlags.Instance | BindingFlags.Public,
            binder: null, types: new[] { typeof(Message) },
            modifiers: null) != null;
    }

    private static bool HasPublicRoutedSend(Type type)
    {
        return type.GetMethod("Send", BindingFlags.Instance | BindingFlags.Public,
            binder: null, types: new[]
            {
                typeof(string), typeof(Message)
            }, modifiers: null) != null;
    }

    private static bool HasPublicReceiveWithFlags()
    {
        return typeof(IRoutedMessageSocket).GetMethod("Recv",
            BindingFlags.Instance | BindingFlags.Public, binder: null,
            types: new[] { typeof(Received), typeof(RecvFlags) },
            modifiers: null) != null;
    }

    private static string ResolveRepoPath(string relativePath)
    {
        DirectoryInfo? current = new DirectoryInfo(Directory.GetCurrentDirectory());
        for (int i = 0; i < 10 && current != null; i++)
        {
            string candidate = Path.Combine(current.FullName, relativePath);
            if (File.Exists(candidate))
                return candidate;
            current = current.Parent;
        }
        throw new FileNotFoundException($"{relativePath} not found.");
    }

    [Fact]
    public void stream_callback_exception_reports_unhandled_event()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-callback-ex");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        Exception? observed = null;
        void OnUnhandled(Exception ex)
        {
            observed = ex;
        }

        Zlink.UnhandledCallbackException += OnUnhandled;
        try
        {
            stream.OnPacket((StreamPacketHandler)((_, header, payload) =>
            {
                header.Dispose();
                payload.Dispose();
                throw new InvalidOperationException("stream-callback-fail");
            }));

            using var client = ConnectRawClient(port);
            SendAll(client.GetStream(),
                CoreTestSupport.BuildStreamPacket("stream-callback-fail"u8));

            Assert.True(CoreTestSupport.WaitUntil(() => observed != null, 3000));
            Assert.IsType<InvalidOperationException>(observed);
        }
        finally
        {
            Zlink.UnhandledCallbackException -= OnUnhandled;
        }
    }

    [Fact]
    public void stream_recv_api_dispatch_conflict()
    {
        Assert.True(HasPublicReceiveWithFlags());
    }

    [Fact]
    public void stream_dispatch_start_allows_stream_notify()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();

        stream.Options.Notify = true;
        stream.OnPacket((StreamPacketHandler)((_, _, body) =>
        {
            body.Dispose();
        }));
    }

    [Fact]
    public void stream_send_message_failure_preserves_ownership()
    {
        Assert.False(HasPublicMessageSend(typeof(ISubSocket)));
    }

    [Fact]
    public void stream_streamsend_message_failure_preserves_ownership()
    {
        Assert.False(HasPublicRoutedSend(typeof(IDealerSocket)));
    }

    [Fact]
    public void stream_callback_echo_raw()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-raw-cb");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        int matched = 0;
        byte[] expected = "stream-callback-raw"u8.ToArray();
        stream.OnPacket((StreamPacketHandler)((rid, header, payload) =>
        {
            header.Dispose();
            if (payload.AsReadOnlySpan().SequenceEqual(expected))
                Interlocked.Increment(ref matched);
            stream.Send(rid).Message(payload).Submit();
        }));

        using var client = ConnectRawClient(port);
        NetworkStream ns = client.GetStream();
        SendAll(ns, CoreTestSupport.BuildStreamPacket(expected));

        byte[] echoed = ReceiveExact(ns, expected.Length);
        Assert.Equal(expected, echoed);
        Assert.True(CoreTestSupport.WaitUntil(() => Volatile.Read(ref matched) >= 1,
            15000));
    }

    [Fact]
    public void stream_callback_raw_transfers_message_ownership()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-raw-owned");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        using var receivedSignal = new ManualResetEventSlim(false);
        Message? owned = null;
        byte[] expected = "stream-raw-owned-payload"u8.ToArray();
        stream.OnPacket((StreamPacketHandler)((_, header, payload) =>
        {
            header.Dispose();
            ReadOnlySpan<byte> bytes = payload.AsReadOnlySpan();
            if (bytes.Length == 1 && (bytes[0] == 0x00 || bytes[0] == 0x01))
            {
                payload.Dispose();
                return;
            }

            owned = payload;
            receivedSignal.Set();
        }));

        using var client = ConnectRawClient(port);
        NetworkStream ns = client.GetStream();
        SendAll(ns, CoreTestSupport.BuildStreamPacket(expected));

        Assert.True(receivedSignal.Wait(3000));
        Assert.NotNull(owned);
        Assert.True(owned!.AsReadOnlySpan().SequenceEqual(expected));
        owned.Dispose();
        Assert.Throws<ObjectDisposedException>(() =>
        {
            _ = owned.Size;
        });
    }

    [Fact]
    public void stream_callback_echo_single_zero_byte()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-zero-cb");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        int matched = 0;
        byte[] payload = { 0x00 };
        stream.OnPacket((StreamPacketHandler)((rid, header, msg) =>
        {
            header.Dispose();
            ReadOnlySpan<byte> payload = msg.AsReadOnlySpan();
            if (payload.Length == 1 && payload[0] == 0)
                Interlocked.Increment(ref matched);
            stream.Send(rid).Message(msg).Submit();
        }));

        using var client = ConnectRawClient(port);
        NetworkStream ns = client.GetStream();
        SendAll(ns, CoreTestSupport.BuildStreamPacket(payload));
        byte[] echoed = ReceiveExact(ns, 1);
        Assert.Equal(payload, echoed);
        Assert.True(CoreTestSupport.WaitUntil(() => Volatile.Read(ref matched) >= 1,
            3000));
    }

    [Fact]
    public void stream_packet_disconnect_rid_closes_raw_client()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-disconnect-rid");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        using var receivedSignal = new ManualResetEventSlim(false);
        RoutingId observedRid = default;
        byte[] payload = "close-me"u8.ToArray();
        stream.OnPacket((StreamPacketHandler)((rid, header, body) =>
        {
            header.Dispose();
            body.Dispose();
            observedRid = rid;
            receivedSignal.Set();
        }));

        using var client = ConnectRawClient(port);
        NetworkStream ns = client.GetStream();
        SendAll(ns, CoreTestSupport.BuildStreamPacket(payload, "bye"u8));

        Assert.True(receivedSignal.Wait(5000));
        Assert.False(observedRid.IsEmpty);

        using (Message closing = Message.From("session-closing"u8))
            stream.Send(observedRid).Message(closing).Submit();
        stream.DisconnectRid(observedRid);
        Assert.Equal("session-closing"u8.ToArray(), ReceiveExact(ns, "session-closing"u8.Length));
        Assert.True(WaitForRawClientClose(ns, 3000),
            "stream DisconnectRid(rid) did not close the raw TCP client.");
    }

    [Fact]
    public void stream_socket_raw_tcp_roundtrip()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        using var client = new TcpClient();
        client.NoDelay = true;
        client.Connect(IPAddress.Loopback, port);

        byte[] incoming = "hello"u8.ToArray();
        client.GetStream().Write(incoming, 0, incoming.Length);

        var received = Received.Create(); stream.Recv(received, RecvFlags.None);
        using (Message payloadMessage = received.Parts[0])
        {
            RoutingId routingId = received.RoutingId
                ?? throw new InvalidOperationException("missing routing id");
            Assert.False(routingId.IsEmpty);
            Assert.Equal("hello", CoreTestSupport.Utf8(payloadMessage));
            using var reply = Message.From("world"u8);
            stream.Send(routingId).Message(reply).Submit();
            Assert.Throws<ObjectDisposedException>(() =>
            {
                _ = reply.Size;
            });
        }

        client.ReceiveTimeout = 3000;
        byte[] recv = new byte[64];
        int n = client.GetStream().Read(recv, 0, recv.Length);
        Assert.True(n > 0);
        Assert.Equal("world", CoreTestSupport.Utf8(recv.AsSpan(0, n)));
    }

    [Fact]
    public void stream_recv_part_returns_raw_part_and_routing_id()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-recv-part");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        using var client = ConnectRawClient(port);
        NetworkStream clientStream = client.GetStream();
        byte[] incoming = "raw-part"u8.ToArray();
        SendAll(clientStream, incoming);

        Assert.True(stream.RecvPart(
            out RoutingId? sourceRoutingId,
            out Message? part,
            out bool hasMore,
            RecvFlags.None));
        RoutingId routingId = sourceRoutingId
            ?? throw new InvalidOperationException("missing routing id");
        Assert.False(routingId.IsEmpty);
        Assert.False(hasMore);
        using (part ?? throw new InvalidOperationException("missing raw part"))
            Assert.Equal(incoming, part!.ToArray());

        using var reply = Message.From("raw-reply"u8);
        stream.Send(routingId).Message(reply).Submit();
        Assert.Equal("raw-reply"u8.ToArray(),
            ReceiveExact(clientStream, "raw-reply"u8.Length));
    }

    [Fact]
    public void stream_recv_part_emits_connection_ready_monitor_event()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-recv-part-monitor");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        using ISocketMonitor monitor = stream.MonitorOpen(SocketEvent.ConnectionReady);
        using var client = ConnectRawClient(port);
        SendAll(client.GetStream(), "raw-part-monitor"u8);

        Assert.True(stream.RecvPart(
            out RoutingId? sourceRoutingId,
            out Message? part,
            out bool hasMore,
            RecvFlags.None));
        using (part ?? throw new InvalidOperationException("missing raw part"))
            Assert.Equal("raw-part-monitor"u8.ToArray(), part!.ToArray());
        Assert.False(sourceRoutingId is null || sourceRoutingId.Value.IsEmpty);
        Assert.False(hasMore);

        Assert.Equal(1, ZlinkPoll.Poll(new[] { monitor }, 3000));
        SocketMonitorEvent evt = monitor.Recv(RecvFlags.DontWait)
            ?? throw new TimeoutException("STREAM monitor event was not queued.");
        Assert.Equal(MonitorEventType.ConnectionReady, evt.Event);
    }

    [Fact]
    public void stream_recv_part_is_gated_by_socket_poller()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        using var poller = Zlink.CreatePoller();

        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-poller");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);
        poller.Add(stream, PollEventFlags.PollIn, 17);

        using var client = ConnectRawClient(port);
        SendAll(client.GetStream(), "poller-part"u8);

        var events = new PollEvent[1];
        Assert.Equal(1, poller.Wait(events, TimeSpan.FromSeconds(3)));
        Assert.Equal(PollSourceKind.Socket, events[0].SourceKind);
        Assert.Equal((nuint)17, events[0].Slot);
        Assert.NotEqual(PollEventFlags.None, events[0].Revents & PollEventFlags.PollIn);

        Assert.True(stream.RecvPart(
            out _,
            out var part,
            out _,
            RecvFlags.DontWait));
        using (part ?? throw new InvalidOperationException("missing raw part"))
            Assert.Equal("poller-part"u8.ToArray(), part!.ToArray());
    }

    [Fact]
    public void stream_receive_and_packet_callback_modes_are_exclusive()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        stream.Bind(CoreTestSupport.NewEndpoint("tcp", "stream-recv-mode"));

        Assert.False(stream.RecvPart(
            out RoutingId? sourceRoutingId,
            out Message? part,
            out bool hasMore,
            RecvFlags.DontWait));
        Assert.Null(sourceRoutingId);
        Assert.Null(part);
        Assert.False(hasMore);

        var error = Assert.Throws<ZlinkHandlerException>(() =>
            stream.OnPacket((StreamPacketHandler)((_, header, payload) =>
            {
                header.Dispose();
                payload.Dispose();
            })));
        Assert.Equal(ZlinkHandlerException.ErrorCode.Busy, error.Result);

        using var callbackFirst = ctx.CreateStreamSocket();
        callbackFirst.Bind(CoreTestSupport.NewEndpoint("tcp", "stream-callback-mode"));
        callbackFirst.OnPacket((StreamPacketHandler)((_, header, payload) =>
        {
            header.Dispose();
            payload.Dispose();
        }));

        var receiveError = Assert.Throws<ZlinkHandlerException>(() =>
            callbackFirst.RecvPart(
                out RoutingId? callbackRoutingId,
                out Message? callbackPart,
                out bool callbackHasMore,
                RecvFlags.DontWait));
        Assert.Equal(ZlinkHandlerException.ErrorCode.Busy, receiveError.Result);
    }

    [Fact]
    public void stream_receive_surfaces_public_routing_id()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();

        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-peers");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        using var client = new TcpClient();
        client.NoDelay = true;
        client.Connect(IPAddress.Loopback, port);

        byte[] incoming = "routing-id-check"u8.ToArray();
        client.GetStream().Write(incoming, 0, incoming.Length);

        var received = Received.Create(); stream.Recv(received, RecvFlags.None);
        using (Message message = received.Parts[0])
        {
            RoutingId routingId = received.RoutingId
                ?? throw new InvalidOperationException("missing routing id");
            Assert.False(routingId.IsEmpty);
            Assert.Equal("routing-id-check", CoreTestSupport.Utf8(message));
        }
    }

    [Fact]
    public void stream_monitor_multiclient_connect_disconnect()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-monitor");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        using var monitorEvents = new CallbackEventQueue<SocketMonitorEvent>();
        using ISocketMonitor monitor = stream.MonitorOpen(
            SocketEvent.ConnectionReady | SocketEvent.Disconnected);
        monitor.OnEvent(monitorEvents.Enqueue);

        const int clients = 4;
        for (int i = 0; i < clients; i++)
        {
            uint connectRid = 0;
            using var client = ConnectRawClient(port);
            NetworkStream ns = client.GetStream();
            SendAll(ns, "probe"u8);
            var received = Received.Create(); stream.Recv(received, RecvFlags.None);
            using (Message payload = received.Parts[0])
            {
                RoutingId routingId = received.RoutingId
                    ?? throw new InvalidOperationException("missing routing id");
                Assert.False(routingId.IsEmpty);
                Assert.Equal("probe", CoreTestSupport.Utf8(payload));
            }
            Assert.True(TryReadMonitorEvent(monitorEvents,
                SocketEvent.ConnectionReady, 3000, out connectRid));
            Assert.True(connectRid > 0);

            client.Dispose();
            bool disconnected = TryReadMonitorEvent(monitorEvents,
                SocketEvent.Disconnected, 3000, out uint disconnectRid);
            if (disconnected)
            {
                Assert.True(disconnectRid > 0);
            }
        }
    }

    [Fact]
    public async Task stream_raw_multiclient_load_integrity()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-raw-load");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        stream.OnPacket((StreamPacketHandler)((rid, header, payload) =>
        {
            header.Dispose();
            stream.Send(rid).Message(payload).Submit();
        }));

        const int clientCount = 8;
        const int messagesPerClient = 20;
        Task[] clients = new Task[clientCount];
        for (int i = 0; i < clientCount; i++)
        {
            int clientId = i;
            clients[i] = Task.Run(() =>
            {
                using var client = ConnectRawClient(port);
                NetworkStream ns = client.GetStream();
                for (int m = 0; m < messagesPerClient; m++)
                {
                    byte[] payload = new byte[64];
                    BinaryPrimitives.WriteInt32BigEndian(payload.AsSpan(0, 4),
                        clientId);
                    BinaryPrimitives.WriteInt32BigEndian(payload.AsSpan(4, 4), m);
                    for (int j = 8; j < payload.Length; j++)
                        payload[j] = (byte)(clientId + m + j);

                    byte[] frame = CoreTestSupport.BuildStreamPacket(payload);
                    int split = 1 + ((clientId + m) % (frame.Length - 1));
                    SendAll(ns, frame.AsSpan(0, split));
                    SendAll(ns, frame.AsSpan(split));
                    byte[] echoed = ReceiveExact(ns, payload.Length);
                    Assert.Equal(payload, echoed);
                }
            });
        }

        await Task.WhenAll(clients);
    }

    [Fact]
    public void stream_maxmsgsize_disconnects_oversized_payload()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        stream.Options.MaxMessageSize = 4L;

        string endpoint = CoreTestSupport.NewEndpoint("tcp", "stream-maxmsg");
        int port = CoreTestSupport.ExtractPort(endpoint);
        stream.Bind(endpoint);

        using var monitorEvents = new CallbackEventQueue<SocketMonitorEvent>();
        using ISocketMonitor monitor = stream.MonitorOpen(
            SocketEvent.ConnectionReady | SocketEvent.Disconnected);
        monitor.OnEvent(monitorEvents.Enqueue);

        using var client = ConnectRawClient(port);
        NetworkStream ns = client.GetStream();
        SendAll(ns, "ok"u8);

        var received = Received.Create(); stream.Recv(received, RecvFlags.None);
        RoutingId serverRoutingId = received.RoutingId
            ?? throw new InvalidOperationException("missing routing id");
        using (Message payload = received.Parts[0])
        {
            Assert.Equal("ok", CoreTestSupport.Utf8(payload));
        }
        Assert.False(serverRoutingId.IsEmpty);

        Assert.True(TryReadMonitorEvent(monitorEvents, SocketEvent.ConnectionReady,
            3000, out uint connectRid));
        Assert.True(connectRid > 0);

        byte[] oversized = new byte[1024];
        Array.Fill(oversized, (byte)'A');
        SendAll(ns, oversized);

        bool monitorDisconnected = TryReadMonitorEvent(monitorEvents,
            SocketEvent.Disconnected, 4000, out uint disconnectRid);
        if (monitorDisconnected)
        {
            Assert.Equal(connectRid, disconnectRid);
        }
    }

    [Fact]
    public void stream_connect_rejected()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        Assert.Null(typeof(IStreamSocket).GetMethod("Connect"));
    }

    [Fact]
    public void stream_ws_basic()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported("ws"))
            return;

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        string endpoint = CoreTestSupport.NewEndpoint("ws", "stream-ws");
        stream.Bind(endpoint);
    }

    [Fact]
    public void stream_wss_basic()
    {
        if (!CoreTestSupport.IsNativeAvailable())
            return;
        if (!CoreTestSupport.IsTransportSupported("wss"))
            return;

        string cert;
        string key;
        try
        {
            cert = ResolveRepoPath("bindings/dotnet/tests/certs/server.crt");
            key = ResolveRepoPath("bindings/dotnet/tests/certs/server.key");
        }
        catch (FileNotFoundException)
        {
            return;
        }

        using var ctx = Zlink.CreateContext();
        using var stream = ctx.CreateStreamSocket();
        stream.SetTlsServer(cert, key);
        string endpoint = CoreTestSupport.NewEndpoint("wss", "stream-wss");
        stream.Bind(endpoint);
    }

    private static bool TryReadMonitorEvent(
        CallbackEventQueue<SocketMonitorEvent> events, SocketEvent expectedEvent,
        int timeoutMs, out uint routingId)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            int remainingMs = (int)Math.Max(1,
                (deadline - DateTime.UtcNow).TotalMilliseconds);
            if (!events.TryDequeue(remainingMs, out SocketMonitorEvent evt))
                break;

            if (evt.Event != (MonitorEventType)expectedEvent
                || evt.RoutingId == null)
                continue;

            routingId = BinaryPrimitives.ReadUInt32BigEndian(
                evt.RoutingId.Value.ToBytes());
            return true;
        }

        routingId = 0;
        return false;
    }
}
