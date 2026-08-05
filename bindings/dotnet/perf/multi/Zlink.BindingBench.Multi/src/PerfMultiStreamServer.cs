using System;
using System.Collections.Generic;
using System.Threading;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiStreamServer
{
    private const string Pattern = "STREAM";

    private enum SendStatus
    {
        Done = 0,
        Blocked = 1,
        Fatal = 2,
    }

    internal static int Run(PerfOptions options)
    {
        if (!IsCoreStreamServerTransport(options.Transport))
        {
            return PerfRunner.PrintUnsupported(Pattern, options.Transport,
                options.Size,
                "transport_not_supported");
        }

        int ioTimeoutMs = Math.Max(ResolveMultiSndTimeoutMs(options),
            ResolveMultiRcvTimeoutMs(options));
        int pendingCapacity = ResolvePendingCapacity(options);
        string endpoint = MultiEndpointFor(options.Transport, "multi-stream",
            options);

        using var ctx = Zlink.CreateContext();
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreateStreamSocket();
        ApplyMultiSocketOptions(server, options);
        ApplyAutoHwmMsgUnit(ctx, options.Size);
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
        WriteStdoutLine($"READY,{endpoint}");

        var pending = new Queue<PendingStreamMessage>(pendingCapacity);
        object pendingLock = new();
        using var pendingSignal = new ManualResetEventSlim(false);
        var control = new ControlState();
        StartControlWatcher(control);

        server.OnPacket((routingId, header, payload) =>
        {
            PendingStreamMessage request = new();
            using (header)
            using (payload)
            {
                ReadOnlySpan<byte> body = payload.AsReadOnlySpan();
                if (IsStopTokenPayload(body))
                {
                    Interlocked.Exchange(ref control.StopRequested, 1);
                    return;
                }

                request.Assign(routingId, BuildStreamPacket(header, payload));
            }
            SendStatus sendStatus = TrySendPendingMessage(server, request);
            if (sendStatus == SendStatus.Done)
            {
                request.Clear();
                return;
            }

            if (sendStatus == SendStatus.Fatal)
            {
                request.Clear();
                Interlocked.Exchange(ref control.StopRequested, 1);
                return;
            }

            lock (pendingLock)
            {
                if (pending.Count >= pendingCapacity)
                {
                    request.Clear();
                    Interlocked.Exchange(ref control.StopRequested, 1);
                    return;
                }

                pending.Enqueue(request);
                pendingSignal.Set();
            }
        });

        int rc = 0;
        while (Volatile.Read(ref control.StopRequested) == 0)
        {
            if (!FlushPendingMessages(server, pending, pendingLock,
                    pendingSignal, ref rc))
            {
                break;
            }

            if (Volatile.Read(ref control.StopRequested) != 0)
                break;

            pendingSignal.Wait(50);
            pendingSignal.Reset();
        }

        return rc;
    }

    private static void StartControlWatcher(ControlState control)
    {
        Thread watcher = new(() =>
        {
            try
            {
                string? line;
                while ((line = Console.In.ReadLine()) != null)
                {
                    if (line == "STOP" || line == "QUIT")
                    {
                        Interlocked.Exchange(ref control.StopRequested, 1);
                        return;
                    }
                }
            }
            finally
            {
                Interlocked.Exchange(ref control.StopRequested, 1);
            }
        });
        watcher.IsBackground = true;
        watcher.Start();
    }

    private static int ResolvePendingCapacity(PerfOptions options)
    {
        int clients = options.Clients;
        ulong hwm = options.MultiHwm;
        ulong basis = Math.Max(64UL,
            Math.Max((ulong)Math.Max(1, clients), Math.Max(1UL, hwm)));
        if (basis > (ulong)int.MaxValue / 2UL)
            return int.MaxValue;
        return (int)(basis * 2UL);
    }

    private static bool FlushPendingMessages(ISocket server,
        Queue<PendingStreamMessage> pending, object pendingLock,
        ManualResetEventSlim pendingSignal, ref int rc)
    {
        while (true)
        {
            PendingStreamMessage? next = null;
            lock (pendingLock)
            {
                if (pending.Count > 0)
                    next = pending.Dequeue();
            }

            if (next == null)
                return true;

            SendStatus sendStatus = TrySendPendingMessage(server, next);
            if (sendStatus == SendStatus.Done)
            {
                next.Clear();
                continue;
            }

            if (sendStatus == SendStatus.Fatal)
            {
                next.Clear();
                rc = 1;
                return false;
            }

            lock (pendingLock)
            {
                pending.Enqueue(next);
            }
            pendingSignal.Set();
            return true;
        }
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

        byte[] packet = new byte[6 + headerBytes.Length + payloadBytes.Length];
        packet[0] = (byte)((headerBytes.Length >> 8) & 0xFF);
        packet[1] = (byte)(headerBytes.Length & 0xFF);
        packet[2] = (byte)((payloadBytes.Length >> 24) & 0xFF);
        packet[3] = (byte)((payloadBytes.Length >> 16) & 0xFF);
        packet[4] = (byte)((payloadBytes.Length >> 8) & 0xFF);
        packet[5] = (byte)(payloadBytes.Length & 0xFF);
        headerBytes.CopyTo(packet.AsSpan(6));
        payloadBytes.CopyTo(packet.AsSpan(6 + headerBytes.Length));
        return new Message(packet.AsSpan());
    }

    private static bool IsStopTokenPayload(ReadOnlySpan<byte> payload)
    {
        return payload.SequenceEqual(MultiStopToken);
    }

    private static SendStatus TrySendPendingMessage(ISocket server,
        PendingStreamMessage message)
    {
        while (message.Pending)
        {
            try
            {
                if (message.Payload == null)
                    return SendStatus.Fatal;

                if (((IStreamSocket)server).Send(message.RoutingId)
                        .Message(message.Payload).Flags(SendFlags.DontWait)
                        .Submit())
                {
                    message.Clear();
                    return SendStatus.Done;
                }
            }
            catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                            || IsInterrupted(ex.NativeErrno))
            {
                return SendStatus.Blocked;
            }
        }

        return SendStatus.Done;
    }

    private sealed class PendingStreamMessage
    {
        internal RoutingId RoutingId { get; private set; }
        internal Message? Payload { get; private set; }
        internal bool Pending => Payload != null;

        internal void Assign(RoutingId routingId, Message payload)
        {
            RoutingId = routingId;
            Payload = payload;
        }

        internal void Clear()
        {
            Payload?.Dispose();
            Payload = null;
            RoutingId = default;
        }
    }

    private sealed class ControlState
    {
        internal int StopRequested;
    }
}
