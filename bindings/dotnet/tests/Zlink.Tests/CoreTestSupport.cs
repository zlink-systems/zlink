using System;
using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

namespace Systems.Zlink.Tests;

internal static class CoreTestSupport
{
    private static readonly Regex VersionRegex = new(
        @"^#define\s+ZLINK_VERSION_(MAJOR|MINOR|PATCH)\s+(\d+)\s*$",
        RegexOptions.Compiled);

    internal static bool IsNativeAvailable()
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

    internal static (int major, int minor, int patch) ReadCoreHeaderVersion()
    {
        string? header = FindCoreHeader();
        if (header == null)
            throw new FileNotFoundException("core/include/zlink.h not found.");

        int major = -1;
        int minor = -1;
        int patch = -1;

        foreach (string line in File.ReadLines(header))
        {
            Match m = VersionRegex.Match(line);
            if (!m.Success)
                continue;

            int value = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
            switch (m.Groups[1].Value)
            {
                case "MAJOR":
                    major = value;
                    break;
                case "MINOR":
                    minor = value;
                    break;
                case "PATCH":
                    patch = value;
                    break;
            }
        }

        if (major < 0 || minor < 0 || patch < 0)
            throw new InvalidOperationException("Failed to parse zlink version macros.");

        return (major, minor, patch);
    }

    internal static RoutingId RoutingIdUtf8(string value)
    {
        return RoutingId.From(Encoding.UTF8.GetBytes(value));
    }

    internal static byte[] BuildStreamPacket(ReadOnlySpan<byte> body,
        ReadOnlySpan<byte> header = default)
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
        return frame;
    }

    internal static void SendStreamPacket(NetworkStream stream,
        ReadOnlySpan<byte> body, ReadOnlySpan<byte> header = default)
    {
        byte[] frame = BuildStreamPacket(body, header);
        stream.Write(frame);
        stream.Flush();
    }

    internal static string NewEndpoint(string transport, string prefix)
    {
        if (transport == "inproc")
            return $"inproc://{prefix}-{Guid.NewGuid():N}";
        if (transport == "ipc")
        {
            string file = $"{prefix}-{Guid.NewGuid():N}.sock";
            string path = Path.Combine(Path.GetTempPath(), file);
            return $"ipc://{path}";
        }

        int port = ReservePort();
        return $"{transport}://127.0.0.1:{port}";
    }

    internal static bool IsTransportSupported(string transport)
    {
        if (transport == "tcp" || transport == "inproc")
            return true;
        if (transport == "ipc")
            return Zlink.Has("ipc");
        if (transport == "ws")
            return Zlink.Has("ws");
        if (transport == "wss")
            return Zlink.Has("wss");
        if (transport == "tls")
            return Zlink.Has("tls");
        return false;
    }

    internal static bool WaitUntil(Func<bool> predicate, int timeoutMs,
        int sleepMs = 10)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            if (predicate())
                return true;
            Thread.Sleep(sleepMs);
        }
        return false;
    }

    internal static void SendWithRetry(IMessageSocket socket,
        ReadOnlySpan<byte> payload, int timeoutMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            using Message message = Message.From(payload);
            try
            {
                socket.Send().Message(message).Submit(SendFlags.None);
                return;
            }
            catch (ZlinkSubmitException)
            {
            }
            Thread.Sleep(10);
        }
        throw new TimeoutException("send timeout");
    }

    internal static void SendAsyncWithTimeout(IDealerSocket socket,
        ReadOnlySpan<byte> payload, int timeoutMs)
    {
        // Core refuses a submit that has no connected peer yet, and the
        // binding never retries on the caller's behalf. Retrying here is the
        // application-owned policy the contract puts on this side.
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (true)
        {
            using Message message = Message.From(payload);
            try
            {
                socket.Send().Message(message).Async()
                    .WaitAsync(TimeSpan.FromMilliseconds(timeoutMs))
                    .GetAwaiter().GetResult();
                return;
            }
            catch (ZlinkSubmitException) when (DateTime.UtcNow < deadline)
            {
                Thread.Sleep(10);
            }
        }
    }

    internal static void PublishWithRetry(IPublisherSocket socket, string topic,
        ReadOnlySpan<byte> payload, int timeoutMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            Message message = Message.From(payload);
            try
            {
                if (socket.TryPublish(topic).Message(message).Submit()) return;
            }
            catch (ZlinkSubmitException)
            {
            }
            Thread.Sleep(10);
        }
        throw new TimeoutException("publish timeout");
    }

    internal static Received ReceiveMessageWithTimeout(
        IReceivingMessageSocket socket,
        int timeoutMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        var received = Received.Create();
        while (DateTime.UtcNow < deadline)
        {
            if (socket.Recv(received, RecvFlags.DontWait))
                return received;
            Thread.Sleep(10);
        }
        throw new TimeoutException("receive timeout");
    }

    internal static byte[] ReceiveBytesWithTimeout(
        IReceivingMessageSocket socket,
        int maxSize, int timeoutMs)
    {
        _ = maxSize;
        Received received = ReceiveMessageWithTimeout(socket, timeoutMs);
        try
        {
            return received.Parts.Count == 0
                ? Array.Empty<byte>()
                : received.Parts[received.Parts.Count - 1].AsReadOnlySpan().ToArray();
        }
        finally
        {
            DisposeAll(received.Parts);
        }
    }

    internal static string ReceiveUtf8WithTimeout(
        IReceivingMessageSocket socket,
        int timeoutMs)
    {
        Received received = ReceiveMessageWithTimeout(socket, timeoutMs);
        try
        {
            return received.Parts.Count == 0
                ? string.Empty
                : Encoding.UTF8.GetString(
                    received.Parts[received.Parts.Count - 1].AsReadOnlySpan()).Trim('\0');
        }
        finally
        {
            DisposeAll(received.Parts);
        }
    }

    internal static string SubscribeUtf8WithTimeout(ISubscriberSocket socket,
        out string topic, int timeoutMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        using var subscribed = new Subscribed();
        while (DateTime.UtcNow < deadline)
        {
            if (socket.Subscribe(subscribed, RecvFlags.DontWait))
            {
                topic = subscribed.Topic;
                return subscribed.Parts.Count == 0
                    ? string.Empty
                    : Encoding.UTF8.GetString(
                        subscribed.Parts[subscribed.Parts.Count - 1]
                            .AsReadOnlySpan()).Trim('\0');
            }
            Thread.Sleep(10);
        }

        throw new TimeoutException("subscribe timeout");
    }

    internal static byte[] ReceiveSubscriptionEventWithTimeout(IXPubSocket socket,
        out bool subscribed, int timeoutMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        var ev = new SubscriptionEvent();
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                if (!socket.ReceiveSubscriptionEvent(ev, RecvFlags.DontWait))
                {
                    Thread.Sleep(10);
                    continue;
                }
                subscribed = ev.Subscribed;
                return Encoding.UTF8.GetBytes(ev.Topic);
            }
            catch (ZlinkRecvException)
            {
            }
            Thread.Sleep(10);
        }

        throw new TimeoutException("subscription event timeout");
    }

    internal static bool ExpectNoSubscriptionEvent(IXPubSocket socket, int probeMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(probeMs);
        var ev = new SubscriptionEvent();
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                if (socket.ReceiveSubscriptionEvent(ev, RecvFlags.DontWait))
                    return false;
            }
            catch (ZlinkRecvException)
            {
            }
            Thread.Sleep(5);
        }
        return true;
    }

    internal static string Utf8(Message message)
    {
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        return Encoding.UTF8.GetString(message.AsReadOnlySpan()).Trim('\0');
    }

    internal static string Utf8(ReadOnlySpan<byte> payload)
    {
        return Encoding.UTF8.GetString(payload).Trim('\0');
    }

    internal static bool TryReceiveMultipartLastPart(
        IReceivingMessageSocket socket,
        int maxSize, out byte[] lastPart)
    {
        _ = maxSize;
        try
        {
            var received = Received.Create();
            if (!socket.Recv(received, RecvFlags.DontWait))
            {
                lastPart = Array.Empty<byte>();
                return false;
            }
            IReadOnlyList<Message> parts = received.Parts;
            if (parts.Count == 0)
            {
                lastPart = Array.Empty<byte>();
                return true;
            }

            try
            {
                lastPart = parts[parts.Count - 1].AsReadOnlySpan().ToArray();
                return true;
            }
            finally
            {
                DisposeAll(parts);
            }
        }
        catch (ZlinkRecvException ex)
            when (ex.Result == ZlinkRecvException.ErrorCode.NoData)
        {
            lastPart = Array.Empty<byte>();
            return false;
        }
    }

    internal static bool ExpectNoMessage(IReceivingMessageSocket socket,
        int probeMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(probeMs);
        var received = Received.Create();
        while (DateTime.UtcNow < deadline)
        {
            if (socket.Recv(received, RecvFlags.DontWait))
            {
                DisposeAll(received.Parts);
                return false;
            }
            Thread.Sleep(5);
        }
        return true;
    }

    internal static bool ExpectNoSubscribedMessage(ISubscriberSocket socket,
        int probeMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(probeMs);
        using var subscribed = new Subscribed();
        while (DateTime.UtcNow < deadline)
        {
            if (socket.Subscribe(subscribed, RecvFlags.DontWait))
            {
                return false;
            }
            Thread.Sleep(5);
        }
        return true;
    }

    internal static (string routingId, string payload) ReceiveRoutedUtf8WithTimeout(
        IReceivingMessageSocket socket, int timeoutMs)
    {
        TimeSpan? oldTimeout = socket.Options.ReceiveTimeout;
        socket.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(timeoutMs);
        try
        {
            var received = Received.Create();
            socket.Recv(received);
            try
            {
                string payload = received.Parts.Count == 0
                    ? string.Empty
                    : Encoding.UTF8.GetString(
                        received.Parts[received.Parts.Count - 1].AsReadOnlySpan())
                        .Trim('\0');
                string routingId = received.RoutingId.HasValue
                    ? Encoding.UTF8.GetString(received.RoutingId.Value.ToBytes())
                    : string.Empty;
                return (routingId, payload);
            }
            finally
            {
                DisposeAll(received.Parts);
            }
        }
        catch (ZlinkRecvException)
        {
            throw new TimeoutException("receive timeout");
        }
        finally
        {
            socket.Options.ReceiveTimeout = oldTimeout;
        }
    }

    internal static int ExtractPort(string endpoint)
    {
        int idx = endpoint.LastIndexOf(':');
        if (idx <= 0 || idx == endpoint.Length - 1)
            throw new ArgumentException($"invalid endpoint: {endpoint}",
                nameof(endpoint));
        return int.Parse(endpoint.AsSpan(idx + 1), CultureInfo.InvariantCulture);
    }

    private static int ReservePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    private static string? FindCoreHeader()
    {
        DirectoryInfo? current = new DirectoryInfo(Directory.GetCurrentDirectory());
        for (int i = 0; i < 10 && current != null; i++)
        {
            string candidate = Path.Combine(current.FullName, "core", "include",
                "zlink.h");
            if (File.Exists(candidate))
                return candidate;
            current = current.Parent;
        }
        return null;
    }

    private static void DisposeAll(IReadOnlyList<Message> parts)
    {
        for (int i = 0; i < parts.Count; i++)
            parts[i].Dispose();
    }
}

internal sealed class CallbackEventQueue<T> : IDisposable
{
    private readonly ConcurrentQueue<T> _queue = new();
    private readonly ManualResetEventSlim _signal = new(false);

    internal void Enqueue(T value)
    {
        _queue.Enqueue(value);
        _signal.Set();
    }

    internal bool TryDequeue(int timeoutMs, out T value)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            if (_queue.TryDequeue(out T? queued))
            {
                value = queued!;
                if (_queue.IsEmpty)
                    _signal.Reset();
                return true;
            }

            TimeSpan remaining = deadline - DateTime.UtcNow;
            if (remaining <= TimeSpan.Zero)
                break;

            _signal.Wait(remaining);
        }

        value = default!;
        return false;
    }

    public void Dispose()
    {
        _signal.Dispose();
    }
}
