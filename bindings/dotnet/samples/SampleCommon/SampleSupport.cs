using System;
using System.Buffers.Binary;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using Systems.Zlink;

namespace SampleCommon;

public static class SampleSupport
{
    public static bool IsNativeAvailable()
    {
        try
        {
            _ = global::Systems.Zlink.Zlink.Version();
            return true;
        }
        catch (DllNotFoundException)
        {
            return false;
        }
        catch (EntryPointNotFoundException)
        {
            return false;
        }
        catch (TypeInitializationException ex) when (ex.InnerException
                is DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }
    }

    public static string NewEndpoint(string transport, string prefix)
    {
        int port = ReservePort();
        return $"{transport}://127.0.0.1:{port}";
    }

    public static RoutingId RoutingIdUtf8(string value)
    {
        return RoutingId.From(Encoding.UTF8.GetBytes(value));
    }

    public static int ReservePort()
    {
        TcpListener listener = new(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    public static void WaitConnected(params ISocketMonitor[] monitors)
    {
        foreach (ISocketMonitor monitor in monitors)
            WaitMonitorEvent(monitor, 5000, SocketEvent.ConnectionReady);
    }

    public static MonitorEvent WaitMonitorEvent(ISocketMonitor monitor,
        int timeoutMs, params SocketEvent[] expectedEvents)
    {
        if (expectedEvents == null || expectedEvents.Length == 0)
        {
            throw new ArgumentException("Expected monitor events are required.",
                nameof(expectedEvents));
        }

        _ = timeoutMs;
        MonitorEvent evt = monitor.Recv()
            ?? throw new TimeoutException("Timed out waiting for monitor event.");
        for (int i = 0; i < expectedEvents.Length; i++)
        {
            if ((SocketEvent)evt.Event == expectedEvents[i])
                return evt;
        }

        throw new InvalidOperationException(
            $"Unexpected monitor event {evt.Event}.");
    }

    public static void WaitOrThrow(Func<bool> predicate, int timeoutMs,
        string message)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            if (predicate())
                return;
            Thread.Sleep(10);
        }

        throw new TimeoutException(message);
    }

    public static string ReceiveUtf8(IMessageSocket socket, int timeoutMs)
    {
        _ = timeoutMs;
        using var received = Received.Create();
        if (!socket.Recv(received))
            throw new InvalidOperationException("recv failed");
        if (received.Parts.Count == 0)
            throw new InvalidOperationException(
                "Expected at least one message part.");
        return Encoding.UTF8.GetString(received.Parts[0].AsReadOnlySpan());
    }

    public static string SubscribeUtf8(ISubscriberSocket socket, out string topic,
        int timeoutMs)
    {
        _ = timeoutMs;
        using var subscribed = new TopicMessage();
        socket.Subscribe(subscribed);
        topic = subscribed.Topic;
        if (subscribed.Parts.Count == 0)
            throw new InvalidOperationException(
                "Expected at least one subscribed message part.");
        return Encoding.UTF8.GetString(subscribed.Parts[0].AsReadOnlySpan());
    }

    public static TcpClient ConnectRawClient(int port)
    {
        var client = new TcpClient();
        client.NoDelay = true;
        client.Connect(IPAddress.Loopback, port);
        return client;
    }

    public static void SendAll(NetworkStream stream, ReadOnlySpan<byte> payload)
    {
        stream.Write(payload);
        stream.Flush();
    }

    public static void SendStreamPacket(NetworkStream stream,
        ReadOnlySpan<byte> body, ReadOnlySpan<byte> header = default)
    {
        if (header.Length > ushort.MaxValue)
        {
            throw new ArgumentOutOfRangeException(nameof(header),
                "Stream packet header is too large.");
        }

        byte[] frame = new byte[6 + header.Length + body.Length];
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(0, 2),
            (ushort)header.Length);
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(2, 4),
            (uint)body.Length);
        header.CopyTo(frame.AsSpan(6, header.Length));
        body.CopyTo(frame.AsSpan(6 + header.Length, body.Length));
        SendAll(stream, frame);
    }

    public static byte[] ReceiveExact(NetworkStream stream, int size)
    {
        byte[] buffer = new byte[size];
        int read = 0;
        while (read < size)
        {
            int n = stream.Read(buffer, read, size - read);
            if (n <= 0)
                throw new IOException("stream receive timeout");
            read += n;
        }

        return buffer;
    }

    public static int ExtractPort(string endpoint)
    {
        int idx = endpoint.LastIndexOf(':');
        return int.Parse(endpoint.AsSpan(idx + 1));
    }

    public static void EnsureEqual(string expected, string actual, string name)
    {
        if (!string.Equals(expected, actual, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"Expected {name} \"{expected}\" but received \"{actual}\".");
        }
    }

}
