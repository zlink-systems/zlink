using System;
using System.Collections.Generic;
using System.Diagnostics;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiRouterRouterClient
{
    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        int sndTimeoutMs = ResolveMultiSndTimeoutMs(options);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        int latencySampleCap = ResolveMultiLatencySampleCap(options);
        int clientCount = ResolveMultiClients(options);
        int pollTimeoutMs = ResolveMultiClientPollTimeoutMs(options);
        string endpoint = options.Endpoint;
        ReadOnlySpan<byte> serverRoutingId = "SERVER"u8;

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        ApplyMultiClientContextOptions(ctx, options);
        var clients = new List<ISocket>(clientCount);
        var monitors = new List<MonitorSocket>(clientCount);
        try
        {
            for (int i = 0; i < clientCount; i++)
            {
                var client = ctx.CreateRouterSocket();
                ApplyMultiSocketOptions(client, options);
                ConfigureTlsClientIfNeeded(client, options.Transport);
                client.Options.SendTimeout = TimeSpan.FromMilliseconds(sndTimeoutMs);
                client.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeoutMs);
                // Match the C router client: set both the client identity and
                // the peer routing id before Connect so ConnectionReady means
                // the same routed peer setup in both harnesses.
                client.SetRoutingId(RoutingId.From(
                    System.Text.Encoding.ASCII.GetBytes($"client_{i}")));
                client.Options.SetConnectRoutingId(
                    RoutingId.From(serverRoutingId));
                var monitor = client.MonitorOpen(SocketEvent.ConnectionReady);
                client.Connect(endpoint);
                clients.Add(client);
                monitors.Add(monitor);
            }

            List<ISocket> activeClients = WaitClientConnectReadyAll(
                pollManager, clients, monitors, readyTimeoutMs);
            if (activeClients.Count != clients.Count)
            {
                Console.Error.WriteLine("multi_client_error:no_ready_connections");
                return 2;
            }
            DisposeAllQuietly(monitors);
            monitors.Clear();

            for (int i = 0; i < clients.Count; i++)
                ApplyAutoHwmMsgUnit(ctx, size);
            RecalculateAutoHwm(ctx);
            if (clients.Count > 0)
                PrintAutoHwmSnapshot(clients[0], "endpoint",
                    options.Transport, size);

            var slots = CreateSlots(activeClients, serverRoutingId, size);
            (double throughput, double latencyNs, double latencyP95Ns,
                double latencyP99Ns, long measureCount) result;
            try
            {
                result = RunMultiRouterRouterClientLoop(pollManager, slots,
                    size, latencySampleCap, pollTimeoutMs, durationSeconds,
                    readyTimeoutMs);
            }
            finally
            {
                DisposeSlots(slots);
            }

            // PERF_MULTI: echo (relay) clients send NO wire stop token. C
            // perf_multi_router_router_client.cpp drives run_echo_duration
            // (common perf_multi_client_helpers.hpp) which never emits a stop
            // token; the relay/echo server blindly echoes and shuts down via
            // the runner terminating it after the client is done. Sending a
            // stop token here is a .NET-only divergence and is removed for
            // parity with C.

            if (result.measureCount <= 0)
                return 2;

            PrintResult(options.Pattern, options.Transport, size, result.throughput,
                result.latencyNs, result.latencyP95Ns, result.latencyP99Ns);
            return 0;
        }
        finally
        {
            DisposeAllQuietly(monitors);
            DisposeAllQuietly(clients);
        }
    }

    private static RouterRouterClientSlot[] CreateSlots(
        List<ISocket> activeClients, ReadOnlySpan<byte> serverRoutingId,
        int msgSize)
    {
        var slots = new RouterRouterClientSlot[activeClients.Count];
        for (int i = 0; i < activeClients.Count; i++)
        {
            var payload = new byte[Math.Max(msgSize, PerfMetricHeaderSize)];
            slots[i] = new RouterRouterClientSlot(activeClients[i],
                RoutingId.From(serverRoutingId), payload);
        }

        return slots;
    }

    private static void DisposeSlots(RouterRouterClientSlot[] slots)
    {
        for (int i = 0; i < slots.Length; i++)
            slots[i].ReusableReceived.Dispose();
    }

    private static (double throughput, double latencyNs, double latencyP95Ns,
        double latencyP99Ns, long measureCount)
        RunMultiRouterRouterClientLoop(PollManager pollManager,
            RouterRouterClientSlot[] slots, int msgSize, int latencySampleCap,
            int pollTimeoutMs, int durationSeconds, int readyTimeoutMs)
    {
        _ = readyTimeoutMs;
        const uint runId = 1;
        var latSamples = new List<double>(latencySampleCap);
        ulong seq = 1;
        int rrIndex = 0;
        var metrics = new RouterRouterMetrics(latSamples, latencySampleCap);

        var sockets = CollectSockets(slots);
        var eventMasks = new PollEventFlags[slots.Length];
        ResetPollMasks(slots, eventMasks);

        long benchStartTicks = Stopwatch.GetTimestamp();
        long benchDeadlineTicks = benchStartTicks
            + (long)Math.Max(1, durationSeconds) * Stopwatch.Frequency;
        while (Stopwatch.GetTimestamp() < benchDeadlineTicks)
        {
            TryScheduleIdleSends(slots, eventMasks, msgSize, runId, PerfPhase.Active,
                ref seq, ref rrIndex);

            // PERF_MULTI_TEST_POLICY § 1.3.1: signal-driven wait.
            int readyCount = PollSocketEvents(pollManager, sockets, eventMasks,
                pollTimeoutMs);
            if (readyCount <= 0)
            {
                continue;
            }

            for (int i = 0; i < readyCount; i++)
                HandleClientEvent(pollManager, slots,
                    ReadySocketIndexAt(pollManager, i),
                    ReadySocketMaskAt(pollManager, i), eventMasks, msgSize,
                    runId, PerfPhase.Active, ref seq, metrics, allowSend: true,
                    activeDeadlineTicks: benchDeadlineTicks);
        }

        long benchEndTicks = Stopwatch.GetTimestamp();
        double elapsedSeconds = (benchEndTicks - benchStartTicks)
            / (double)Stopwatch.Frequency;
        double configuredSeconds = Math.Max(1.0, durationSeconds);
        double throughput = metrics.MeasureCount / configuredSeconds;
        // PERF_POLICY: report measured latency only. C
        // normalize_latency_stats reports zeros when no samples and never
        // fabricates a duration-derived latency.
        var latency = ComputeLatencyStats(latSamples);
        double latencyNs = latency.mean;
        double latencyP95Ns = Math.Max(latency.p95, latencyNs);
        double latencyP99Ns = Math.Max(latency.p99, latencyP95Ns);

        return (throughput, latencyNs, latencyP95Ns, latencyP99Ns,
            metrics.MeasureCount);
    }

    private static void TryScheduleIdleSends(RouterRouterClientSlot[] slots,
        PollEventFlags[] eventMasks, int msgSize, uint runId, PerfPhase phase, ref ulong seq,
        ref int rrIndex)
    {
        int startIndex = rrIndex;
        for (int i = 0; i < slots.Length; i++)
        {
            int slotIndex = (startIndex + i) % slots.Length;
            RouterRouterClientSlot slot = slots[slotIndex];
            if (slot.WaitingForReply || slot.WaitingForWritable)
                continue;

            if (TrySend(slot, msgSize, runId, phase, ref seq))
            {
                slot.WaitingForReply = true;
                slot.WaitingForWritable = false;
                UpdatePollMask(slot, eventMasks, slotIndex);
                continue;
            }

            slot.WaitingForWritable = true;
            UpdatePollMask(slot, eventMasks, slotIndex);
        }

        if (slots.Length > 0)
            rrIndex = (startIndex + 1) % slots.Length;
    }

    private static void HandleClientEvent(PollManager pollManager,
        RouterRouterClientSlot[] slots,
        int slotIndex, PollEventFlags readyMask, PollEventFlags[] eventMasks,
        int msgSize, uint runId, PerfPhase phase, ref ulong seq,
        RouterRouterMetrics metrics, bool allowSend, long activeDeadlineTicks)
    {
        _ = activeDeadlineTicks;
        _ = pollManager;
        RouterRouterClientSlot slot = slots[slotIndex];

        if ((readyMask & PollEventFlags.PollOut) != 0
            && slot.WaitingForWritable
            && !slot.WaitingForReply)
        {
            if (TrySend(slot, msgSize, runId, phase, ref seq))
            {
                slot.WaitingForReply = true;
                slot.WaitingForWritable = false;
                UpdatePollMask(slot, eventMasks, slotIndex);
            }
        }

        if ((readyMask & PollEventFlags.PollIn) == 0)
            return;

        IRouterSocket routerSock = slot.Socket;
        Received received = slot.ReusableReceived;
        while (true)
        {
            if (!routerSock.Recv(received, RecvFlags.DontWait))
                break;

            if (!received.IsSinglePart)
                continue;

            if (!slot.WaitingForReply)
                continue;

            slot.WaitingForReply = false;
            if (phase == PerfPhase.Active)
            {
                ReadOnlySpan<byte> body = received.FirstPart().AsReadOnlySpan();
                // Match C reference: outer while loop bounds the active
                // window; dropping replies that arrive after activeDeadline
                // would lower throughput vs C for replies whose sends were
                // inside the active phase.
                if (PerfShared.TryDecodeMetricHeader(body, out PerfMetricHeader header)
                    && header.RunId == runId
                    && header.MsgSize == (uint)msgSize
                    && header.Phase == (uint)phase)
                {
                    metrics.MeasureCount++;
                    if (metrics.LatencySamples != null && header.SentTsNs > 0)
                    {
                        ulong nowNs = EpochNs();
                        if (nowNs >= header.SentTsNs)
                        {
                            double sampleLatencyNs = (nowNs - header.SentTsNs)
                                / 2.0;
                            long sampleSeen = metrics.SampleSeen;
                            uint rng = metrics.Rng;
                            ReservoirSample(metrics.LatencySamples,
                                sampleLatencyNs, ref sampleSeen,
                                metrics.LatencySampleCap, ref rng);
                            metrics.SampleSeen = sampleSeen;
                            metrics.Rng = rng;
                        }
                    }
                }
            }

            if (!allowSend)
                continue;

            slot.WaitingForWritable = false;
            UpdatePollMask(slot, eventMasks, slotIndex);
        }
    }

    private static List<ISocket> CollectSockets(
        RouterRouterClientSlot[] slots)
    {
        var sockets = new List<ISocket>(slots.Length);
        for (int i = 0; i < slots.Length; i++)
            sockets.Add(slots[i].Socket);
        return sockets;
    }

    private static void ResetPollMasks(RouterRouterClientSlot[] slots,
        PollEventFlags[] eventMasks)
    {
        for (int i = 0; i < slots.Length; i++)
            UpdatePollMask(slots[i], eventMasks, i);
    }

    private static void UpdatePollMask(RouterRouterClientSlot slot,
        PollEventFlags[] eventMasks, int index)
    {
        PollEventFlags events = SocketPollIn;
        if (slot.WaitingForWritable && !slot.WaitingForReply)
            events |= SocketPollOut;
        eventMasks[index] = events;
    }

    private static bool TrySend(RouterRouterClientSlot slot, int msgSize,
        uint runId, PerfPhase phase, ref ulong seq)
    {
        // Match C: stamp immediately before every send attempt, including a
        // POLLOUT retry. A blocked attempt keeps the current sequence; only a
        // successful send advances it.
        StampMetricHeader(slot.Payload.AsSpan(), runId, phase, msgSize,
            seq, EpochNs());
        using Message message = Message.Allocate(slot.Payload.Length);
        slot.Payload.AsSpan(0, PerfMetricHeaderSize).CopyTo(message.AsSpan());
        bool sent = slot.Socket.Send(slot.ServerRoutingId)
            .Message(message)
            .Flags(SendFlags.DontWait)
            .Submit();
        if (sent)
            seq++;
        return sent;
    }

    private static int RemainingMilliseconds(long deadlineTicks)
    {
        long nowTicks = Stopwatch.GetTimestamp();
        if (deadlineTicks <= nowTicks)
            return 0;

        double remainingMs = (deadlineTicks - nowTicks) * 1000.0
            / Stopwatch.Frequency;
        if (remainingMs >= int.MaxValue)
            return int.MaxValue;
        return (int)Math.Ceiling(remainingMs);
    }

    private sealed class RouterRouterClientSlot
    {
        internal RouterRouterClientSlot(ISocket socket, RoutingId serverRoutingId,
            byte[] payload)
        {
            Socket = (IRouterSocket)socket;
            ServerRoutingId = serverRoutingId;
            Payload = payload;
            ReusableReceived = Received.Create();
        }

        internal IRouterSocket Socket { get; }
        internal RoutingId ServerRoutingId { get; }
        internal byte[] Payload { get; }
        // Caller-provided storage reused across every recv on this slot.
        internal Received ReusableReceived { get; }
        internal bool WaitingForWritable { get; set; }
        internal bool WaitingForReply { get; set; }
    }

    private sealed class RouterRouterMetrics
    {
        internal RouterRouterMetrics(List<double>? latencySamples,
            int latencySampleCap)
        {
            LatencySamples = latencySamples;
            LatencySampleCap = latencySampleCap;
            SampleSeen = 0;
            Rng = 0xA341316Cu;
        }

        internal long MeasureCount { get; set; }
        internal List<double>? LatencySamples { get; }
        internal int LatencySampleCap { get; }
        internal long SampleSeen { get; set; }
        internal uint Rng { get; set; }
    }

}
