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
        string endpoint = MultiEndpointFor(options.Transport, "multi-stream",
            options);

        using var ctx = Zlink.CreateContext();
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreateStreamSocket();
        ApplyMultiSocketOptions(server, options);
        RecalculateAutoHwm(ctx);
        PrintAutoHwmSnapshot(server, "server", options.Transport,
            options.Size);
        ConfigureTlsServerIfNeeded(server, options.Transport);
        server.Options.SendTimeout = TimeSpan.FromMilliseconds(ioTimeoutMs);
        server.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(ioTimeoutMs);
        server.Options.TcpNoDelay = true;
        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;
        RecalculateAutoHwm(ctx);

        var dispatchQueue = new StreamDispatchQueue();
        var control = new ControlState(dispatchQueue.StopAccepting);
        StartControlWatcher(control);
        var sends = new SendTracker();
        using var drainCancellation = new CancellationTokenSource();
        Task dispatcher = DispatchSendsAsync(server, dispatchQueue, sends,
            control, drainCancellation.Token);
        long handlersInFlight = 0;

        server.OnPacket((routingId, header, payload) =>
        {
            Interlocked.Increment(ref handlersInFlight);
            Message? packet = null;
            try
            {
                using (header)
                using (payload)
                {
                    if (Volatile.Read(ref control.StopRequested) != 0)
                        return;
                    ReadOnlySpan<byte> body = payload.AsReadOnlySpan();
                    if (IsStopTokenPayload(body))
                    {
                        control.RequestStop();
                        return;
                    }

                    packet = BuildStreamPacket(header, payload);
                }

                if (dispatchQueue.TryEnqueue(routingId, packet))
                    packet = null;
                else
                {
                    packet.Dispose();
                    packet = null;
                }
            }
            catch (Exception ex)
            {
                packet?.Dispose();
                DebugFailure("stream packet callback", ex);
                control.Fail();
            }
            finally
            {
                Interlocked.Decrement(ref handlersInFlight);
            }
        });

        WriteStdoutLine($"READY,{endpoint}");
        await control.StopTask.ConfigureAwait(false);

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

        long handlerDeadline = Stopwatch.GetTimestamp()
            + (long)drainTimeoutMs * Stopwatch.Frequency / 1000L;
        while (Volatile.Read(ref handlersInFlight) != 0
               && Stopwatch.GetTimestamp() < handlerDeadline)
            await Task.Delay(1).ConfigureAwait(false);
        if (sends.Count != 0 || Volatile.Read(ref handlersInFlight) != 0)
            control.Fail();

        return control.ResultCode;
    }

    private static void StartControlWatcher(ControlState control)
    {
        string controlFile = PerfEnv.ReadString("PERF_MULTI_CONTROL_FILE",
            string.Empty);
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
                                    if (line.Trim().Equals("STOP",
                                            StringComparison.OrdinalIgnoreCase)
                                        || line.Trim().Equals("QUIT",
                                            StringComparison.OrdinalIgnoreCase))
                                    {
                                        control.RequestStop();
                                        return;
                                    }
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
                    if (line == "STOP" || line == "QUIT")
                    {
                        control.RequestStop();
                        return;
                    }
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
        private readonly TaskCompletionSource _stopped = new(
            TaskCreationOptions.RunContinuationsAsynchronously);
        private readonly Action _stopAccepting;

        internal ControlState(Action stopAccepting)
        {
            _stopAccepting = stopAccepting;
        }

        internal int StopRequested;
        internal int ResultCode;
        internal Task StopTask => _stopped.Task;

        internal void RequestStop()
        {
            if (Interlocked.Exchange(ref StopRequested, 1) == 0)
            {
                _stopAccepting();
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
