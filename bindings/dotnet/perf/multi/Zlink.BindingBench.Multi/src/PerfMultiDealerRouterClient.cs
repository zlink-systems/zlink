using System;
using System.Collections.Generic;
using System.Diagnostics;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiDealerRouterClient
{
    internal static async Task<int> Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        int sndTimeoutMs = ResolveMultiSndTimeoutMs(options);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        ulong monitorHwmBytes = ResolveMultiMonitorHwmBytes();
        int latencySampleCap = ResolveMultiLatencySampleCap(options);
        int clientCount = ResolveMultiClients(options);
        string endpoint = options.Endpoint;

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        ApplyMultiClientContextOptions(ctx, options);
        var clients = new List<ISocket>(clientCount);
        var monitors = new List<MonitorSocket>(clientCount);
        try
        {
            for (int i = 0; i < clientCount; i++)
            {
                var client = ctx.CreateDealerSocket();
                ApplyMultiSocketOptions(client, options);
                ConfigureTlsClientIfNeeded(client, options.Transport);
                client.Options.SendTimeout = TimeSpan.FromMilliseconds(sndTimeoutMs);
                client.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeoutMs);
                client.SetRoutingId(RoutingId.From(
                    System.Text.Encoding.ASCII.GetBytes($"CLIENT-{i}")));
                var monitor = client.MonitorOpen(SocketEvent.ConnectionReady,
                    monitorHwmBytes);
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
                RecalculateAutoHwm(ctx);
            if (clients.Count > 0)
                PrintAutoHwmSnapshot(clients[0], "endpoint",
                    options.Transport, size);

            var slots = CreateSlots(activeClients, size);
            var result = await RunMultiDealerRouterClientLoop(pollManager,
                slots, size, latencySampleCap, durationSeconds,
                readyTimeoutMs).ConfigureAwait(false);

            // PERF_MULTI: echo (relay) clients send NO wire stop token. C
            // perf_multi_dealer_router_client.cpp drives run_echo_duration
            // (common perf_multi_client_helpers.hpp) which never emits a stop
            // token; the relay/echo server (perf_multi_relay_server.hpp,
            // included by perf_multi_dealer_router_server.cpp) blindly echoes
            // and shuts down via the runner terminating it after the client
            // is done. Sending a stop token here is a .NET-only divergence
            // and is removed for parity with C.

            if (result.measureCount <= 0 || result.latencyCount <= 0)
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

    private static DealerRouterClientSlot[] CreateSlots(
        List<ISocket> activeClients, int msgSize)
    {
        var slots = new DealerRouterClientSlot[activeClients.Count];
        for (int i = 0; i < activeClients.Count; i++)
        {
            var payload = new byte[Math.Max(msgSize, PerfMetricHeaderSize)];
            slots[i] = new DealerRouterClientSlot(activeClients[i], payload);
        }

        return slots;
    }

    private static async Task<(double throughput, double latencyNs,
        double latencyP95Ns, double latencyP99Ns, long measureCount,
        long latencyCount)>
        RunMultiDealerRouterClientLoop(PollManager pollManager,
            DealerRouterClientSlot[] slots, int msgSize, int latencySampleCap,
            int durationSeconds, int readyTimeoutMs)
    {
        _ = readyTimeoutMs;
        const uint runId = 1;
        var latSamples = new List<double>(latencySampleCap);
        long seq = 0;
        var metrics = new DealerRouterMetrics(latSamples, latencySampleCap);

        var sockets = CollectSockets(slots);
        var eventMasks = new PollEventFlags[slots.Length];
        Array.Fill(eventMasks, SocketPollIn);

        long benchStartTicks = Stopwatch.GetTimestamp();
        long benchDeadlineTicks = benchStartTicks
            + (long)Math.Max(1, durationSeconds) * Stopwatch.Frequency;

        async Task SendLoopAsync(DealerRouterClientSlot slot)
        {
            // Async admission can complete inline while Core has credit. Yield
            // once before the send loop so constructing the sender Tasks cannot
            // consume the whole active window before the receive poll starts.
            await Task.Yield();
            IDealerSocket socket = (IDealerSocket)slot.Socket;
            while (Stopwatch.GetTimestamp() < benchDeadlineTicks)
            {
                ulong currentSeq = unchecked((ulong)Interlocked.Increment(ref seq));
                StampMetricHeader(slot.Payload.AsSpan(), runId, PerfPhase.Active,
                    msgSize, currentSeq, EpochNs());
                using Message message = Message.Allocate(slot.Payload.Length);
                slot.Payload.AsSpan().CopyTo(message.AsSpan());
                await PerfSocketIo.SendMeasurementAsync(socket, message,
                    SendFlags.None).ConfigureAwait(false);
            }
        }

        var sendTasks = new Task[slots.Length];
        for (int i = 0; i < slots.Length; i++)
            sendTasks[i] = SendLoopAsync(slots[i]);

        while (Stopwatch.GetTimestamp() < benchDeadlineTicks)
        {
            int pollTimeoutMs = Math.Min(50, RemainingMilliseconds(benchDeadlineTicks));
            if (pollTimeoutMs <= 0)
                break;
            int readyCount = PollSocketEvents(pollManager, sockets, eventMasks,
                pollTimeoutMs);
            if (readyCount <= 0)
            {
                continue;
            }

            for (int i = 0; i < readyCount; i++)
                HandleClientEvent(pollManager, slots,
                    ReadySocketIndexAt(pollManager, i),
                    ReadySocketMaskAt(pollManager, i), msgSize,
                    runId, PerfPhase.Active, metrics,
                    activeDeadlineTicks: benchDeadlineTicks);
        }
        await Task.WhenAll(sendTasks).ConfigureAwait(false);

        long benchEndTicks = Stopwatch.GetTimestamp();

        double elapsedSeconds = (benchEndTicks - benchStartTicks)
            / (double)Stopwatch.Frequency;
        double configuredSeconds = Math.Max(1.0, durationSeconds);
        double throughput = metrics.MeasureCount / configuredSeconds;
        // PERF_POLICY: report measured latency only. C
        // normalize_latency_stats reports zeros when no samples and never
        // fabricates a duration-derived latency.
        var latency = ComputeMultiLatencyStats(latSamples,
            metrics.SampleSeen, metrics.LatencySum);
        double latencyNs = latency.mean;
        double latencyP95Ns = Math.Max(latency.p95, latencyNs);
        double latencyP99Ns = Math.Max(latency.p99, latencyP95Ns);

        return (throughput, latencyNs, latencyP95Ns, latencyP99Ns,
            metrics.MeasureCount, metrics.SampleSeen);
    }

    private static void HandleClientEvent(
        PollManager pollManager,
        DealerRouterClientSlot[] slots,
        int slotIndex, PollEventFlags readyMask, int msgSize, uint runId,
        PerfPhase phase, DealerRouterMetrics metrics, long activeDeadlineTicks)
    {
        _ = pollManager;
        DealerRouterClientSlot slot = slots[slotIndex];
        if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
            return;

        if ((readyMask & PollEventFlags.PollIn) == 0)
            return;

        IDealerSocket dealerSock = (IDealerSocket)slot.Socket;
        Received receivedMessage = slot.ReusableReceived;
        while (true)
        {
            if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                break;
            if (!dealerSock.Recv(receivedMessage, RecvFlags.DontWait))
                break;

            if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                break;

            if (phase == PerfPhase.Active)
            {
                // The active deadline checks immediately around Recv keep
                // post-window replies out of both throughput and latency.
                if (PerfSocketIo.TryMeasurementPayload(receivedMessage.Parts,
                        out Message payloadPart)
                    && PerfRunner.TryDecodeMetricHeader(
                        payloadPart.AsReadOnlySpan(),
                        out PerfMetricHeader header)
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
                            double latencySum = metrics.LatencySum;
                            ReservoirSampleMulti(metrics.LatencySamples,
                                sampleLatencyNs, ref sampleSeen, ref latencySum,
                                metrics.LatencySampleCap, ref rng);
                            metrics.SampleSeen = sampleSeen;
                            metrics.LatencySum = latencySum;
                            metrics.Rng = rng;
                        }
                    }
                }
            }

        }
    }

    private static List<ISocket> CollectSockets(
        DealerRouterClientSlot[] slots)
    {
        var sockets = new List<ISocket>(slots.Length);
        for (int i = 0; i < slots.Length; i++)
            sockets.Add(slots[i].Socket);
        return sockets;
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

    private sealed class DealerRouterClientSlot
    {
        internal DealerRouterClientSlot(ISocket socket, byte[] payload)
        {
            Socket = socket;
            Payload = payload;
            ReusableReceived = Received.Create();
        }

        internal ISocket Socket { get; }
        internal byte[] Payload { get; }
        // Caller-provided storage reused across every recv on this slot.
        // The binding overwrites the internal state in place, avoiding the
        // per-recv Received allocation.
        internal Received ReusableReceived { get; }
    }

    private sealed class DealerRouterMetrics
    {
        internal DealerRouterMetrics(List<double>? latencySamples,
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
        internal double LatencySum { get; set; }
        internal uint Rng { get; set; }
    }

}
