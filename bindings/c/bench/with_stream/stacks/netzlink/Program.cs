using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Zlink;

namespace NetZlinkStreamServer;

internal sealed class ServerOptions
{
    public string Host { get; set; } = "0.0.0.0";
    public int Port { get; set; } = 38007;
    public int Size { get; set; } = 1024;
    public int SndBuf { get; set; } = 1024 * 1024;
    public int RcvBuf { get; set; } = 1024 * 1024;
    public int Backlog { get; set; } = 32768;
    public int TcpNoDelay { get; set; } = 1;
    public int IoThreads { get; set; } = 4;

    public const int MinPayloadSize = 16;
    public const int MaxPayloadSize = 4 * 1024 * 1024;

    public static bool TryParse(string[] args, out ServerOptions options)
    {
        options = new ServerOptions();
        for (int i = 0; i < args.Length; i++)
        {
            string key = args[i];
            if (!key.StartsWith("--", StringComparison.Ordinal))
                continue;
            if ((i + 1) >= args.Length)
                break;

            string value = args[++i];
            switch (key)
            {
                case "--host":
                    options.Host = value;
                    break;
                case "--port":
                    options.Port = ParseInt(value, options.Port, 1);
                    break;
                case "--size":
                    options.Size = ParseInt(value, options.Size, MinPayloadSize);
                    break;
                case "--sndbuf":
                    options.SndBuf = ParseInt(value, options.SndBuf, 1);
                    break;
                case "--rcvbuf":
                    options.RcvBuf = ParseInt(value, options.RcvBuf, 1);
                    break;
                case "--backlog":
                    options.Backlog = ParseInt(value, options.Backlog, 1);
                    break;
                case "--tcp-nodelay":
                    options.TcpNoDelay = ParseInt(value, options.TcpNoDelay, 0);
                    break;
                case "--io-threads":
                    options.IoThreads = ParseInt(value, options.IoThreads, 1);
                    break;
            }
        }

        if (options.Size > MaxPayloadSize)
        {
            Console.Error.WriteLine($"netzlink stream: size too large {options.Size}");
            return false;
        }

        return true;
    }

    private static int ParseInt(string text, int fallback, int min)
    {
        if (!int.TryParse(text, out int parsed))
            return fallback;
        if (parsed < min)
            return min;
        return parsed;
    }
}

internal sealed class Metrics
{
    private long _recvMsgs;
    private long _parseError;
    private long _protocolError;
    private long _sendError;

    public long RecvMsgs => Interlocked.Read(ref _recvMsgs);
    public long ParseError => Interlocked.Read(ref _parseError);
    public long ProtocolError => Interlocked.Read(ref _protocolError);
    public long SendError => Interlocked.Read(ref _sendError);

    public void AddRecvMsg() => Interlocked.Increment(ref _recvMsgs);
    public void AddParseError() => Interlocked.Increment(ref _parseError);
    public void AddProtocolError() => Interlocked.Increment(ref _protocolError);
    public void AddSendError() => Interlocked.Increment(ref _sendError);
}

internal sealed class StreamEchoServer : IDisposable
{
    private const int PrefixSize = 6;
    private static readonly byte[] MessageName = Encoding.ASCII.GetBytes("stream.echo");
    private readonly StreamSocket _socket;
    private readonly Metrics _metrics;
    private bool _attached;

    internal StreamEchoServer(StreamSocket socket, Metrics metrics)
    {
        _socket = socket ?? throw new ArgumentNullException(nameof(socket));
        _metrics = metrics ?? throw new ArgumentNullException(nameof(metrics));
    }

    internal void Attach()
    {
        if (_attached)
            return;
        _socket.OnFramedPacket(OnFramedPacket);
        _attached = true;
    }

    private void OnFramedPacket(uint routingId, Message header, Message body)
    {
        try
        {
            ReadOnlySpan<byte> headerBytes = header.AsReadOnlySpan();
            if (!headerBytes.SequenceEqual(MessageName))
            {
                _metrics.AddParseError();
                _metrics.AddProtocolError();
                return;
            }

            int bodySize = body.Size;
            if (bodySize < ServerOptions.MinPayloadSize
                || bodySize > ServerOptions.MaxPayloadSize)
            {
                _metrics.AddParseError();
                _metrics.AddProtocolError();
                return;
            }

            _metrics.AddRecvMsg();
            int totalSize = PrefixSize + headerBytes.Length + bodySize;
            byte[] replyBytes = new byte[totalSize];
            replyBytes[0] = (byte)(headerBytes.Length >> 8);
            replyBytes[1] = (byte)headerBytes.Length;
            replyBytes[2] = (byte)(bodySize >> 24);
            replyBytes[3] = (byte)(bodySize >> 16);
            replyBytes[4] = (byte)(bodySize >> 8);
            replyBytes[5] = (byte)bodySize;
            headerBytes.CopyTo(replyBytes.AsSpan(PrefixSize, headerBytes.Length));
            body.AsReadOnlySpan().CopyTo(
                replyBytes.AsSpan(PrefixSize + headerBytes.Length, bodySize));

            _socket.Send(routingId, replyBytes, SendFlags.None);
        }
        catch
        {
            _metrics.AddSendError();
        }
        finally
        {
            header.Dispose();
            body.Dispose();
        }
    }

    public void Dispose()
    {
        if (!_attached)
            return;
        try
        {
            _socket.DetachStream();
        }
        finally
        {
            _attached = false;
        }
    }
}

internal static class Program
{

    private static string Endpoint(string host, int port) => $"tcp://{host}:{port}";

    private static void ApplySocketTuning(StreamSocket socket,
                                                 ServerOptions options)
    {
        socket.Options.SendBufferSize = options.SndBuf;
        socket.Options.ReceiveBufferSize = options.RcvBuf;
        socket.Options.TcpNoDelay = options.TcpNoDelay != 0;
        socket.Options.SendHighWaterMark = 100;
        socket.Options.ReceiveHighWaterMark = 100;
    }

    public static int Main(string[] args)
    {
        if (args.Length == 0)
        {
            Console.WriteLine("test_scenario_stream_netzlink: no args -> skip");
            return 0;
        }

        if (!ServerOptions.TryParse(args, out ServerOptions options))
            return 2;

        int rc = 0;
        Metrics metrics = new();
        using CancellationTokenSource cts = new();
        PosixSignalRegistration? sigIntReg = null;
        PosixSignalRegistration? sigTermReg = null;

        void RequestCancel()
        {
            if (!cts.IsCancellationRequested)
                cts.Cancel();
        }

        ConsoleCancelEventHandler cancelHandler = (_, e) =>
        {
            e.Cancel = true;
            RequestCancel();
        };
        EventHandler processExitHandler = (_, _) => RequestCancel();
        Console.CancelKeyPress += cancelHandler;
        AppDomain.CurrentDomain.ProcessExit += processExitHandler;

        try
        {
            if (OperatingSystem.IsLinux() || OperatingSystem.IsMacOS())
            {
                sigIntReg = PosixSignalRegistration.Create(PosixSignal.SIGINT, context =>
                {
                    context.Cancel = true;
                    RequestCancel();
                });
                sigTermReg = PosixSignalRegistration.Create(PosixSignal.SIGTERM, context =>
                {
                    context.Cancel = true;
                    RequestCancel();
                });
            }
        }
        catch
        {
        }

        try
        {
            using Context ctx = new();
            ctx.Options.IoThreads = options.IoThreads;
            using StreamSocket server = new(ctx);
            ApplySocketTuning(server, options);
            server.Bind(Endpoint(options.Host, options.Port));
            using StreamEchoServer echo = new(server, metrics);
            echo.Attach();

            while (!cts.IsCancellationRequested)
                Thread.Sleep(200);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"netzlink stream: {ex.Message}");
            rc = 2;
        }
        finally
        {
            Console.WriteLine(
                $"METRIC stack=netzlink mode=echo size={options.Size} recv_msgs={metrics.RecvMsgs} " +
                $"parse_error={metrics.ParseError} protocol_error={metrics.ProtocolError} " +
                $"send_error={metrics.SendError} connections=0");

            Console.CancelKeyPress -= cancelHandler;
            AppDomain.CurrentDomain.ProcessExit -= processExitHandler;
            sigIntReg?.Dispose();
            sigTermReg?.Dispose();
        }

        return rc;
    }
}
