using System.Net;
using System.Net.Sockets;

internal static class Program
{
private const byte ZmpMagic = 0x5A;
private const byte ZmpVersion = 0x01;
private const int ZmpHeaderSize = 8;
private const int ZmpRequestSequenceSize = 8;
private const byte ZmpFlagMore = 0x01;
private const byte SessionRelocationRoute = 44;

public static int Main(string[] args)
{
    try
    {
        var options = ParseOptions(args);
        var listenAddress = IPAddress.Parse(options.ListenHost);
        var targetAddress = IPAddress.Parse(options.TargetHost);
        var listener = new TcpListener(listenAddress, options.ListenPort);
        listener.Server.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
        listener.Start();
        Console.WriteLine($"proxy-ready listen={options.ListenHost}:{options.ListenPort} target={options.TargetHost}:{options.TargetPort}");

        while (true)
        {
            var client = listener.AcceptTcpClient();
            _ = Task.Run(() => Serve(client, targetAddress, options.TargetPort, options.ArmFile));
        }
    }
    catch (Exception error)
    {
        Console.Error.WriteLine($"session_route_block_proxy: {error.Message}");
        return 1;
    }
}

static void Serve(TcpClient client, IPAddress targetAddress, int targetPort, string armFile)
{
    using (client)
    using (var upstream = new TcpClient())
    {
        try
        {
            upstream.Connect(targetAddress, targetPort);
        }
        catch (SocketException)
        {
            return;
        }

        var downstream = new Thread(() => Pump(upstream, client, "gateway-to-peer", armFile));
        downstream.Start();
        Pump(client, upstream, "peer-to-gateway", armFile);
        downstream.Join();
    }
}

static void Pump(TcpClient source, TcpClient sink, string direction, string armFile)
{
    var parser = new FrameParser();
    var message = new MemoryStream();
    var messageFrames = 0;
    Command44? command44 = null;
    var chunk = new byte[65536];
    try
    {
        var input = source.GetStream();
        var output = sink.GetStream();
        int received;
        while ((received = input.Read(chunk, 0, chunk.Length)) > 0)
        {
            foreach (var frame in parser.Feed(chunk.AsSpan(0, received)))
            {
                message.Write(frame.Raw);
                messageFrames++;
                command44 ??= Command44Identity(frame.Body);
                if ((frame.Flags & ZmpFlagMore) != 0)
                    continue;

                var blocked = messageFrames == 1
                    && File.Exists(armFile)
                    && command44 is { Action: 1 };
                if (blocked)
                {
                    File.WriteAllText(armFile + ".blocked", Environment.NewLine);
                    Console.WriteLine(
                        $"blocked-command-44 direction={direction} actor={command44!.Actor} action=commit " +
                        $"previous-authority={command44.PreviousAuthority} target-authority={command44.TargetAuthority}");
                }
                else
                {
                    message.Position = 0;
                    message.CopyTo(output);
                    output.Flush();
                }

                message.SetLength(0);
                messageFrames = 0;
                command44 = null;
            }
        }
    }
    catch (Exception error)
    {
        Console.WriteLine($"proxy-pump-ended direction={direction} error={error.Message}");
    }

    try
    {
        sink.Client.Shutdown(SocketShutdown.Send);
    }
    catch (SocketException)
    {
        // The opposite pump may already have closed the socket.
    }
}

static Command44? Command44Identity(byte[] body)
{
    if (body.Length < 5 || body[0] != 90 || body[1] != 77 || body[3] != SessionRelocationRoute)
        return null;

    try
    {
        var reader = new BodyReader(body);
        reader.Skip(5 + 16);
        reader.Text8();
        reader.Skip(8);
        reader.Text8();
        reader.Skip(8);
        reader.SkipText16();
        reader.Skip(1);
        var actor = reader.Text8();
        reader.Skip(8);
        reader.Text8();
        reader.Skip(8);
        reader.Text8();
        reader.Skip(8);
        reader.Text8();
        reader.Skip(8);
        var action = reader.U8();
        var routeSize = reader.U16();
        ulong previous = 0;
        ulong target = 0;
        if (action == 1 && routeSize >= 16)
        {
            previous = reader.U64();
            target = reader.U64();
        }

        return new Command44(actor, action, previous, target);
    }
    catch (ArgumentOutOfRangeException)
    {
        return null;
    }
}

static Options ParseOptions(string[] arguments)
{
    var values = new Dictionary<string, string>(StringComparer.Ordinal);
    for (var index = 0; index + 1 < arguments.Length; index += 2)
    {
        var name = arguments[index];
        if (name is not ("--listen-host" or "--listen-port" or "--target-host" or "--target-port" or "--arm-file"))
            throw new ArgumentException($"unknown option: {name}");
        values[name] = arguments[index + 1];
    }

    if (arguments.Length % 2 != 0
        || !values.TryGetValue("--listen-host", out var listenHost)
        || !values.TryGetValue("--listen-port", out var listenPortText)
        || !values.TryGetValue("--target-host", out var targetHost)
        || !values.TryGetValue("--target-port", out var targetPortText)
        || !values.TryGetValue("--arm-file", out var armFile)
        || !int.TryParse(listenPortText, out var listenPort)
        || !int.TryParse(targetPortText, out var targetPort)
        || listenPort is < 1 or > 65535
        || targetPort is < 1 or > 65535)
    {
        throw new ArgumentException(
            "usage: --listen-host H --listen-port P --target-host H --target-port P --arm-file F");
    }

    return new Options(listenHost, listenPort, targetHost, targetPort, armFile);
}

sealed record Options(string ListenHost, int ListenPort, string TargetHost, int TargetPort, string ArmFile);
sealed record Command44(string Actor, byte Action, ulong PreviousAuthority, ulong TargetAuthority);
sealed record Frame(byte[] Raw, byte Flags, byte[] Body);

sealed class FrameParser
{
    private readonly List<byte> buffer = [];

    public IEnumerable<Frame> Feed(ReadOnlySpan<byte> data)
    {
        buffer.AddRange(data.ToArray());
        var frames = new List<Frame>();
        var offset = 0;
        while (buffer.Count - offset >= ZmpHeaderSize)
        {
            if (buffer[offset] != ZmpMagic || buffer[offset + 1] != ZmpVersion)
                throw new InvalidOperationException("unexpected ZMP frame header");
            var flags = buffer[offset + 2];
            var kind = buffer[offset + 3];
            var bodySize = ((uint)buffer[offset + 4] << 24)
                | ((uint)buffer[offset + 5] << 16)
                | ((uint)buffer[offset + 6] << 8)
                | buffer[offset + 7];
            var headerSize = ZmpHeaderSize + (kind is 0x01 or 0x02 or 0x03 ? ZmpRequestSequenceSize : 0);
            var total = checked(headerSize + (int)bodySize);
            if (buffer.Count - offset < total)
                break;

            var raw = buffer.GetRange(offset, total).ToArray();
            var body = raw[headerSize..];
            frames.Add(new Frame(raw, flags, body));
            offset += total;
        }

        if (offset != 0)
            buffer.RemoveRange(0, offset);
        return frames;
    }
}

sealed class BodyReader(byte[] body)
{
    private int offset;

    public byte U8()
    {
        Need(1);
        return body[offset++];
    }

    public ushort U16()
    {
        Need(2);
        var value = (ushort)((body[offset] << 8) | body[offset + 1]);
        offset += 2;
        return value;
    }

    public ulong U64()
    {
        Need(8);
        ulong value = 0;
        for (var index = 0; index < 8; index++)
            value = (value << 8) | body[offset + index];
        offset += 8;
        return value;
    }

    public string Text8()
    {
        var size = U8();
        Need(size);
        var value = System.Text.Encoding.UTF8.GetString(body, offset, size);
        offset += size;
        return value;
    }

    public void SkipText16() => Skip(U16());

    public void Skip(int count)
    {
        Need(count);
        offset += count;
    }

    private void Need(int count)
    {
        if (offset + count > body.Length)
            throw new ArgumentOutOfRangeException(nameof(count), "command 44 body is shorter than its layout");
    }
}
}
