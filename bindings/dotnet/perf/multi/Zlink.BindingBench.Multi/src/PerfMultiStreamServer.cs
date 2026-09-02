using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiStreamServer
{
    private const string Pattern = "STREAM";

    internal static async Task<int> Run(PerfOptions options)
    {
        if (!IsCoreStreamServerTransport(options.Transport))
        {
            return PerfRunner.PrintUnsupported(Pattern, options.Transport,
                options.Size,
                "transport_not_supported");
        }

        int ioTimeoutMs = Math.Max(ResolveMultiSndTimeoutMs(options),
            ResolveMultiRcvTimeoutMs(options));
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        int clientCount = ResolveMultiClients(options);
        ulong monitorHwmBytes = ResolveMultiMonitorHwmBytes();
        string endpoint = MultiEndpointFor(options.Transport, "multi-stream",
            options);

        using var ctx = Zlink.CreateContext();
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreateStreamSocket();
        ApplyMultiSocketOptions(server, options);
        server.Options.ReceiveMode = StreamReceiveMode.Packet;
        RecalculateAutoHwm(ctx);
        ConfigureTlsServerIfNeeded(server, options.Transport);
        using var monitor = server.MonitorOpen(SocketEvent.ConnectionReady,
            monitorHwmBytes);
        server.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(ioTimeoutMs);
        server.Options.TcpNoDelay = true;
        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;

        var dispatchQueue = new StreamDispatchQueue();
        var control = new ControlState(dispatchQueue.StopAccepting);
        StartControlWatcher(control, options.Size);
        var sends = new SendTracker();
        using var drainCancellation = new CancellationTokenSource();
        Task dispatcher = DispatchSendsAsync(server, dispatchQueue, sends,
            control, drainCancellation.Token);
        Task receiver = Task.Run(() =>
        {
            using var result = StreamPacket.Create();
            while (Volatile.Read(ref control.StopRequested) == 0)
            {
                try
                {
                    if (!server.RecvPacket(result))
                        continue;
                    Message? header = result.Header;
                    Message? bodyMessage = result.Body;
                    RoutingId? routingId = result.RoutingId;
                    if (header == null || bodyMessage == null || routingId == null)
                        continue;
                    ReadOnlySpan<byte> body = bodyMessage.AsReadOnlySpan();
                    if (IsStopTokenPayload(body))
                    {
                        control.RequestStop();
                        break;
                    }

                    Message packet = BuildStreamPacket(header, bodyMessage);
                    if (!dispatchQueue.TryEnqueue(routingId.Value, packet))
                        packet.Dispose();
                }
                catch (ZlinkException)
                {
                    if (Volatile.Read(ref control.StopRequested) == 0)
                        continue;
                }
                catch (Exception ex)
                {
                    DebugFailure("stream packet receive", ex);
                    control.Fail();
                    break;
                }
            }
        });

        WriteStdoutLine($"READY,{endpoint}");
        if (!await control.WaitForStartAsync(readyTimeoutMs)
                .ConfigureAwait(false))
        {
            if (Volatile.Read(ref control.StopRequested) == 0)
                Console.Error.WriteLine("multi_server_error:no_start");
            control.Fail();
        }
        else
        {
            // Pair the raw peer's CLIENT_READY with independent server-side
            // route readiness before freezing the connected Auto-HWM state.
            if (!WaitConnectReadyCount(monitor, clientCount, readyTimeoutMs))
            {
                Console.Error.WriteLine(
                    "multi_server_error:connect_ready_timeout");
                control.Fail();
            }
            else
            {
                try
                {
                    // Do not use the best-effort benchmark helper here: a
                    // failed barrier recalculation must suppress the ACK.
                    ctx.RecalculateAutoHwm();
                    PrintAutoHwmSnapshot(server, "server-connected",
                        options.Transport, options.Size);
                    WriteStdoutLine($"SERVER_START_READY,{options.Size}");
                }
                catch (Exception ex)
                {
                    DebugFailure("stream connected Auto-HWM recalc", ex);
                    control.Fail();
                }
            }
        }
        await control.StopTask.ConfigureAwait(false);

        await receiver.ConfigureAwait(false);

        int drainTimeoutMs = Math.Max(1000, ioTimeoutMs * 4);
        drainCancellation.CancelAfter(drainTimeoutMs);
        try
        {
            await dispatcher.ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            control.Fail();
        }

        if (sends.Count != 0)
            control.Fail();

        return control.ResultCode;
    }

    private static void StartControlWatcher(ControlState control, int size)
    {
        string controlFile = PerfEnv.ReadString("PERF_MULTI_CONTROL_FILE",
            string.Empty);
        string expectedStart = $"START,{size}";
        Thread watcher = new(() =>
        {
            if (!string.IsNullOrWhiteSpace(controlFile))
            {
                string previous = string.Empty;
                while (Volatile.Read(ref control.StopRequested) == 0)
                {
                    try
                    {
                        if (File.Exists(controlFile))
                        {
                            string contents = File.ReadAllText(controlFile);
                            if (!string.Equals(contents, previous,
                                    StringComparison.Ordinal))
                            {
                                previous = contents;
                                foreach (string line in contents.Split(
                                    new[] { '\r', '\n' },
                                    StringSplitOptions.RemoveEmptyEntries))
                                {
                                    if (!HandleControlLine(control, line,
                                            expectedStart))
                                        return;
                                }
                            }
                        }
                    }
                    catch (IOException)
                    {
                    }
                    catch (UnauthorizedAccessException)
                    {
                    }

                    Thread.Sleep(10);
                }
                return;
            }

            try
            {
                string? line;
                while ((line = Console.In.ReadLine()) != null)
                {
                    if (!HandleControlLine(control, line, expectedStart))
                        return;
                }
            }
            finally
            {
                control.RequestStop();
            }
        });
        watcher.IsBackground = true;
        watcher.Start();
    }

    private static bool HandleControlLine(ControlState control, string rawLine,
        string expectedStart)
    {
        string line = rawLine.Trim();
        if (string.Equals(line, expectedStart, StringComparison.Ordinal))
        {
            control.RequestStart();
            return true;
        }
        if (line.Equals("STOP", StringComparison.OrdinalIgnoreCase)
            || line.Equals("QUIT", StringComparison.OrdinalIgnoreCase))
        {
            control.RequestStop();
            return false;
        }
        return true;
    }

    private static Message BuildStreamPacket(Message header, Message payload)
    {
        ReadOnlySpan<byte> headerBytes = header.AsReadOnlySpan();
        ReadOnlySpan<byte> payloadBytes = payload.AsReadOnlySpan();
        if (headerBytes.Length > ushort.MaxValue
            || payloadBytes.Length > MaxStreamFrameBytes)
        {
            throw new InvalidOperationException("stream packet is too large");
        }

        Message packet = Message.Allocate(
            6 + headerBytes.Length + payloadBytes.Length);
        Span<byte> packetBytes = packet.AsSpan();
        packetBytes[0] = (byte)((headerBytes.Length >> 8) & 0xFF);
        packetBytes[1] = (byte)(headerBytes.Length & 0xFF);
        packetBytes[2] = (byte)((payloadBytes.Length >> 24) & 0xFF);
        packetBytes[3] = (byte)((payloadBytes.Length >> 16) & 0xFF);
        packetBytes[4] = (byte)((payloadBytes.Length >> 8) & 0xFF);
        packetBytes[5] = (byte)(payloadBytes.Length & 0xFF);
        headerBytes.CopyTo(packetBytes.Slice(6));
        payloadBytes.CopyTo(packetBytes.Slice(6 + headerBytes.Length));
        return packet;
    }

    private static bool IsStopTokenPayload(ReadOnlySpan<byte> payload)
    {
        return payload.SequenceEqual(MultiStopToken);
    }

    private static async Task DispatchSendsAsync(IStreamSocket server,
        StreamDispatchQueue dispatchQueue, SendTracker sends,
        ControlState control, CancellationToken cancellationToken)
    {
        try
        {
            await foreach (PendingStreamPacket packet in dispatchQueue.Reader
                               .ReadAllAsync(cancellationToken)
                               .ConfigureAwait(false))
            {
                Task send = SendMessageAsync(server, packet.RoutingId,
                    packet.Payload, cancellationToken);
                sends.Track(send, control);
            }

            await sends.DrainTask.WaitAsync(cancellationToken)
                .ConfigureAwait(false);
        }
        finally
        {
            dispatchQueue.DisposeRemaining();
        }
    }

    private static async Task SendMessageAsync(IStreamSocket server,
        RoutingId routingId, Message payload,
        CancellationToken cancellationToken)
    {
        try
        {
            await server.Send(routingId).Message(payload)
                .Async(cancellationToken)
                .ConfigureAwait(false);
        }
        catch (ZlinkSubmitException ex) when (IsStaleRoute(ex))
        {
        }
        finally
        {
            payload.Dispose();
        }
    }

    private static void DebugFailure(string operation, Exception error)
    {
        if (Environment.GetEnvironmentVariable("PERF_DEBUG") == null)
            return;
        if (error is ZlinkException zlinkError)
            Console.Error.WriteLine($"{Pattern} {operation} failed "
                + $"errno={zlinkError.NativeErrno}: {error}");
        else
            Console.Error.WriteLine($"{Pattern} {operation} failed: {error}");
    }

    private static bool IsStaleRoute(ZlinkSubmitException error)
    {
        return error.Result == ZlinkSubmitException.ErrorCode.NotConnected
            || error.Result == ZlinkSubmitException.ErrorCode.NotFound;
    }

    private sealed class PendingStreamPacket : IDisposable
    {
        internal PendingStreamPacket(RoutingId routingId, Message payload)
        {
            RoutingId = routingId;
            Payload = payload;
        }

        internal RoutingId RoutingId { get; }
        internal Message Payload { get; }

        public void Dispose()
        {
            Payload.Dispose();
        }
    }

    private sealed class StreamDispatchQueue
    {
        private readonly Channel<PendingStreamPacket> _channel =
            Channel.CreateUnbounded<PendingStreamPacket>(
                new UnboundedChannelOptions
                {
                    SingleReader = true,
                    SingleWriter = false,
                    AllowSynchronousContinuations = false,
                });
        private int _accepting = 1;

        internal ChannelReader<PendingStreamPacket> Reader => _channel.Reader;

        internal bool TryEnqueue(RoutingId routingId, Message payload)
        {
            if (Volatile.Read(ref _accepting) == 0)
                return false;
            return _channel.Writer.TryWrite(
                new PendingStreamPacket(routingId, payload));
        }

        internal void StopAccepting()
        {
            if (Interlocked.Exchange(ref _accepting, 0) != 0)
                _channel.Writer.TryComplete();
        }

        internal void DisposeRemaining()
        {
            while (_channel.Reader.TryRead(out PendingStreamPacket? packet))
                packet.Dispose();
        }
    }

    private sealed class SendTracker
    {
        private readonly object _sync = new();
        private readonly Dictionary<long, Task> _sends = new();
        private TaskCompletionSource _drained = CompletedSource();
        private long _nextId;

        internal int Count
        {
            get
            {
                lock (_sync)
                    return _sends.Count;
            }
        }

        internal Task DrainTask
        {
            get
            {
                lock (_sync)
                    return _drained.Task;
            }
        }

        internal void Track(Task send, ControlState control)
        {
            long id;
            lock (_sync)
            {
                if (_sends.Count == 0)
                    _drained = new TaskCompletionSource(
                        TaskCreationOptions.RunContinuationsAsynchronously);
                id = ++_nextId;
                _sends.Add(id, send);
            }
            _ = ObserveAsync(id, send, control);
        }

        private async Task ObserveAsync(long id, Task send,
            ControlState control)
        {
            try
            {
                await send.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                if (Volatile.Read(ref control.StopRequested) == 0)
                    control.Fail();
            }
            catch (Exception ex)
            {
                DebugFailure("async echo send", ex);
                control.Fail();
            }
            finally
            {
                TaskCompletionSource? drained = null;
                lock (_sync)
                {
                    _sends.Remove(id);
                    if (_sends.Count == 0)
                        drained = _drained;
                }
                drained?.TrySetResult();
            }
        }

        private static TaskCompletionSource CompletedSource()
        {
            var source = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            source.SetResult();
            return source;
        }
    }

    private sealed class ControlState
    {
        private readonly TaskCompletionSource _started = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly TaskCompletionSource _stopped = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly Action _stopAccepting;

        internal ControlState(Action stopAccepting)
        {
            _stopAccepting = stopAccepting;
        }

        internal int StartRequested;
        internal int StopRequested;
        internal int ResultCode;
        internal Task StopTask => _stopped.Task;

        internal void RequestStart()
        {
            if (Interlocked.Exchange(ref StartRequested, 1) == 0)
                _started.TrySetResult();
        }

        internal async Task<bool> WaitForStartAsync(int timeoutMs)
        {
            Task completed = await Task.WhenAny(_started.Task,
                Task.Delay(Math.Max(1, timeoutMs))).ConfigureAwait(false);
            return ReferenceEquals(completed, _started.Task)
                && Volatile.Read(ref StartRequested) != 0
                && Volatile.Read(ref StopRequested) == 0;
        }

        internal void RequestStop()
        {
            if (Interlocked.Exchange(ref StopRequested, 1) == 0)
            {
                _stopAccepting();
                _started.TrySetResult();
                _stopped.TrySetResult();
            }
        }

        internal void Fail()
        {
            Interlocked.Exchange(ref ResultCode, 1);
            RequestStop();
        }
    }
}
