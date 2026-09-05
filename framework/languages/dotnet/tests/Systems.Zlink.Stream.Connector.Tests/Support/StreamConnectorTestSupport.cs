using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using MessagePack;
using Systems.Zlink.Stream.Connector.Contracts;

public sealed partial class StreamConnectorTests
{
    private static async Task<(byte[] Header, byte[] Payload)> ReadPacketAsync(Stream stream)
    {
        var prefix = new byte[6];
        await stream.ReadExactlyAsync(prefix);
        var headerSize = BinaryPrimitives.ReadUInt16BigEndian(prefix.AsSpan(0, 2));
        var payloadSize = BinaryPrimitives.ReadUInt32BigEndian(prefix.AsSpan(2, 4));
        var header = new byte[headerSize];
        var payload = new byte[payloadSize];
        await stream.ReadExactlyAsync(header);
        await stream.ReadExactlyAsync(payload);
        return (header, payload);
    }

    private static async Task WritePacketAsync(Stream stream, byte[] header, byte[] payload)
    {
        var prefix = new byte[6];
        BinaryPrimitives.WriteUInt16BigEndian(prefix.AsSpan(0, 2), (ushort)header.Length);
        BinaryPrimitives.WriteUInt32BigEndian(prefix.AsSpan(2, 4), (uint)payload.Length);
        await stream.WriteAsync(prefix);
        await stream.WriteAsync(header);
        await stream.WriteAsync(payload);
    }

    private static async Task WritePrefixAsync(
        Stream stream,
        ushort headerLength,
        uint payloadLength)
    {
        var prefix = new byte[6];
        BinaryPrimitives.WriteUInt16BigEndian(prefix.AsSpan(0, 2), headerLength);
        BinaryPrimitives.WriteUInt32BigEndian(prefix.AsSpan(2, 4), payloadLength);
        await stream.WriteAsync(prefix);
    }

    private static byte[] EncodePacket(byte[] header, byte[] payload)
    {
        var frame = new byte[6 + header.Length + payload.Length];
        BinaryPrimitives.WriteUInt16BigEndian(frame.AsSpan(0, 2), (ushort)header.Length);
        BinaryPrimitives.WriteUInt32BigEndian(frame.AsSpan(2, 4), (uint)payload.Length);
        header.CopyTo(frame.AsSpan(6));
        payload.CopyTo(frame.AsSpan(6 + header.Length));
        return frame;
    }

    private static (byte[] Header, byte[] Payload) DecodePacket(byte[] frame)
    {
        var headerSize = BinaryPrimitives.ReadUInt16BigEndian(frame.AsSpan(0, 2));
        var payloadSize = BinaryPrimitives.ReadUInt32BigEndian(frame.AsSpan(2, 4));
        var header = frame.AsSpan(6, headerSize).ToArray();
        var payload = frame.AsSpan(6 + headerSize, (int)payloadSize).ToArray();
        return (header, payload);
    }

    private static async Task<byte[]> ReceiveWebSocketMessageAsync(WebSocket webSocket)
    {
        var buffer = new byte[4096];
        using var stream = new MemoryStream();
        WebSocketReceiveResult result;
        do
        {
            result = await webSocket.ReceiveAsync(buffer, CancellationToken.None);
            stream.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);

        return stream.ToArray();
    }

    private static int GetFreeTcpPort()
    {
        using var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return ((IPEndPoint)listener.LocalEndpoint).Port;
    }

    private static async Task DispatchUntilAsync(
        IZlinkStreamConnector connector,
        Func<bool> predicate,
        TimeSpan timeout)
    {
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (!predicate())
        {
            await connector.Dispatch.Async();
            if (Stopwatch.GetElapsedTime(deadlineStarted) >= timeout) throw new TimeoutException("Connector dispatch timeout.");

            await Task.Delay(TimeSpan.FromMilliseconds(10));
        }
    }

    private static async Task WaitUntilAsync(
        Func<bool> predicate,
        TimeSpan timeout)
    {
        var deadlineStarted = Stopwatch.GetTimestamp();
        while (!predicate())
        {
            if (Stopwatch.GetElapsedTime(deadlineStarted) >= timeout) throw new TimeoutException("Condition wait timeout.");

            await Task.Delay(TimeSpan.FromMilliseconds(10));
        }
    }

    private static ZlinkStreamHeartbeatOptions DisabledHeartbeat()
    {
        return new ZlinkStreamHeartbeatOptions { Enabled = false };
    }

    private static X509Certificate2 CreateSelfSignedCertificate()
    {
        using var rsa = RSA.Create(2048);
        var request = new CertificateRequest(
            "CN=localhost",
            rsa,
            HashAlgorithmName.SHA256,
            RSASignaturePadding.Pkcs1);
        request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
        request.CertificateExtensions.Add(new X509KeyUsageExtension(
            X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment,
            false));
        var certificate = request.CreateSelfSigned(
            DateTimeOffset.UtcNow.AddMinutes(-1),
            DateTimeOffset.UtcNow.AddDays(1));
        return new X509Certificate2(certificate.Export(X509ContentType.Pfx));
    }

    private sealed record Ping(string Text);

    private sealed record Pong(string Text);

    [MessagePackObject]
    public sealed class PackedPing
    {
        [Key(0)] public string Text { get; set; } = string.Empty;
    }

    [ZlinkStreamPacketName("custom.packet")]
    private sealed record NamedPacket(string Text);
}