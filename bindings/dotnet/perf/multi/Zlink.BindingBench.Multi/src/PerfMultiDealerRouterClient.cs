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
        int sendDrainTimeoutMs = ResolveMultiSendDrainTimeoutMs();
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
                sendDrainTimeoutMs).ConfigureAwait(false);

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
            int durationSeconds, int sendDrainTimeoutMs)
    {
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
        // C model (perf_multi_client_helpers.hpp drain_timeout_ms): the
        // teardown window is max(PERF_MULTI_SEND_DRAIN_TIMEOUT_MS, 3 s per
        // active second) because small-message runs can fill every per-client
        // Core queue; the backlog is not a workload cap.
        long drainDeadlineTicks = PerfMultiEchoReplyDrain.DeadlineAfter(
            benchDeadlineTicks,
            Math.Max(sendDrainTimeoutMs, Math.Max(1, durationSeconds) * 3000));
        int roundStart = 0;
        var admissionSignal = new PerfMultiAdmissionSignal();
        var replies = new PerfMultiEchoReplyDrain();

        while (Stopwatch.GetTimestamp() < benchDeadlineTicks)
        {
            // Keep submitting on each socket until Core reports backpressure.
            // Only that socket pauses on its admission stage; echoed replies
            // are drained independently and never gate the next send.
            bool submittedAny = false;
            int start = roundStart;
            for (int attempts = 0; attempts < slots.Length; attempts++)
            {
                if (Stopwatch.GetTimestamp() >= benchDeadlineTicks)
                    break;

                int slotIndex = (start + attempts) % slots.Length;
                DealerRouterClientSlot slot = slots[slotIndex];
                slot.ThrowAdmissionError();
                if (slot.AdmissionPending)
                    continue;
                while (Stopwatch.GetTimestamp() < benchDeadlineTicks)
                {
                    ulong currentSeq = unchecked((ulong)++seq);
                    StampMetricHeader(slot.Payload.AsSpan(), runId,
                        PerfPhase.Active, msgSize, currentSeq, EpochNs());
                    submittedAny = true;
                    if (StartAdmission(slot, admissionSignal, replies))
                        break;
                }
            }
            if (slots.Length > 0)
                roundStart = (start + 1) % slots.Length;

            // POLLIN never owns the admission wait. Drain ready replies only;
            // if no work progressed, wait on an actual admission completion.
            int readyCount = PollSocketEvents(pollManager, sockets, eventMasks,
                0);
            if (readyCount <= 0)
            {
                if (!submittedAny && HasPendingAdmissions(slots))
                {
                    if (!await admissionSignal.WaitAsync(benchDeadlineTicks)
                            .ConfigureAwait(false))
                        break;
                }
                continue;
            }

            for (int i = 0; i < readyCount; i++)
                HandleClientEvent(pollManager, slots,
                    ReadySocketIndexAt(pollManager, i),
                    ReadySocketMaskAt(pollManager, i), msgSize,
                    runId, PerfPhase.Active, metrics,
                    activeDeadlineTicks: benchDeadlineTicks, replies);
        }

        // Keep receiving every admitted echo inside the configured drain
        // deadline while the binding runtime completes async admissions.
        await replies.WaitAsync(drainDeadlineTicks, admissionSignal,
            static () => { },
            () => HasPendingAdmissions(slots),
            timeoutMs => PollSocketEvents(pollManager, sockets, eventMasks,
                timeoutMs),
            readyCount =>
            {
                for (int i = 0; i < readyCount; i++)
                    HandleClientEvent(pollManager, slots,
                        ReadySocketIndexAt(pollManager, i),
                        ReadySocketMaskAt(pollManager, i), msgSize,
                        runId, PerfPhase.Active, metrics,
                        benchDeadlineTicks, replies);
            }).ConfigureAwait(false);
        for (int i = 0; i < slots.Length; i++)
            slots[i].ThrowAdmissionError();

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

    private static bool StartAdmission(DealerRouterClientSlot slot,
        PerfMultiAdmissionSignal admissionSignal,
        PerfMultiEchoReplyDrain replies)
    {
        Message message = Message.Allocate(slot.Payload.Length);
        slot.Payload.AsSpan().CopyTo(message.AsSpan());
        replies.Submitted();
        try
        {
            SendSubmission submission = PerfSocketIo.SendMeasurementAsync(
                (IDealerSocket)slot.Socket, message, SendFlags.None);
            if (submission.Result == SubmitResult.Ok)
            {
                message.Dispose();
                return false;
            }

            slot.BeginAdmission();
            Task tracked = AwaitAdmissionAndDisposeAsync(submission.Admitted,
                message, replies, slot);
            admissionSignal.Track(tracked);
            return true;
        }
        catch
        {
            replies.AdmissionRejected();
            message.Dispose();
            throw;
        }
    }

    private static bool HasPendingAdmissions(DealerRouterClientSlot[] slots)
    {
        for (int i = 0; i < slots.Length; i++)
            if (slots[i].AdmissionPending)
                return true;
        return false;
    }

    private static async Task AwaitAdmissionAndDisposeAsync(Task admission,
        Message message, PerfMultiEchoReplyDrain replies,
        DealerRouterClientSlot slot)
    {
        Exception? failure = null;
        try
        {
            await admission.ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            replies.AdmissionRejected();
            failure = exception;
        }
        finally
        {
            message.Dispose();
            slot.CompleteAdmission(failure);
        }
    }

    private static void HandleClientEvent(
        PollManager pollManager,
        DealerRouterClientSlot[] slots,
        int slotIndex, PollEventFlags readyMask, int msgSize, uint runId,
        PerfPhase phase, DealerRouterMetrics metrics, long activeDeadlineTicks,
        PerfMultiEchoReplyDrain replies)
    {
        _ = pollManager;
        DealerRouterClientSlot slot = slots[slotIndex];

        if ((readyMask & PollEventFlags.PollIn) == 0)
            return;

        IDealerSocket dealerSock = (IDealerSocket)slot.Socket;
        Received receivedMessage = slot.ReusableReceived;
        while (true)
        {
            if (!dealerSock.Recv(receivedMessage, RecvFlags.DontWait))
                break;

            long recvTicks = Stopwatch.GetTimestamp();
            if (phase == PerfPhase.Active)
            {
                // Every valid echo retires teardown work, while recvTicks keeps
                // post-window replies out of both throughput and latency.
                if (PerfSocketIo.TryMeasurementPayload(receivedMessage.Parts,
                        out Message payloadPart)
                    && payloadPart.AsReadOnlySpan().Length
                        == Math.Max(msgSize, PerfMetricHeaderSize)
                    && PerfRunner.TryDecodeMetricHeader(
                        payloadPart.AsReadOnlySpan(),
                        out PerfMetricHeader header)
                    && header.RunId == runId
                    && header.MsgSize == (uint)msgSize
                    && header.Phase == (uint)phase)
                {
                    replies.Received();
                    if (recvTicks >= activeDeadlineTicks)
                        continue;
                    metrics.MeasureCount++;
                    if (metrics.LatencySamples != null && header.SentTsNs > 0)
                    {
                        ulong nowNs = EpochNsFromTimestamp(recvTicks);
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
        private int _admissionPending;
        private Exception? _admissionError;

        internal bool AdmissionPending =>
            Volatile.Read(ref _admissionPending) != 0;

        internal void BeginAdmission()
        {
            if (Interlocked.Exchange(ref _admissionPending, 1) != 0)
                throw new InvalidOperationException(
                    "The socket already has a pending admission.");
        }

        internal void CompleteAdmission(Exception? failure)
        {
            if (failure is not null)
                Interlocked.CompareExchange(ref _admissionError, failure, null);
            Volatile.Write(ref _admissionPending, 0);
        }

        internal void ThrowAdmissionError()
        {
            Exception? failure = Interlocked.Exchange(ref _admissionError, null);
            if (failure is not null)
                throw failure;
        }
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
