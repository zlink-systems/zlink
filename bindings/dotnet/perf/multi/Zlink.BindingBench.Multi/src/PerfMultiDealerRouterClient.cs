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
        int latencySeconds = ResolveMultiLatencyDurationSeconds(options);
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
                slots, size, latencySampleCap, durationSeconds, latencySeconds,
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
            int durationSeconds, int latencySeconds, int sendDrainTimeoutMs)
    {
        var latSamples = new List<double>(latencySampleCap);
        var activeMetrics = new DealerRouterMetrics(null, latencySampleCap);
        long activeSends = await RunWindowAsync(pollManager, slots, msgSize,
            PerfPhase.Active, durationSeconds, durationSeconds,
            sendDrainTimeoutMs, activeMetrics, null).ConfigureAwait(false);
        if (activeMetrics.MeasureCount <= 0 || activeSends <= 0)
            return (0, 0, 0, 0, 0, 0);
        double throughput = activeMetrics.MeasureCount / (double)durationSeconds;
        var schedule = new MultiPacedSchedule(
            MultiTargetRate(activeMetrics.MeasureCount, durationSeconds));
        var latencyMetrics = new DealerRouterMetrics(latSamples, latencySampleCap);
        _ = await RunWindowAsync(pollManager, slots, msgSize,
            PerfPhase.Latency, durationSeconds, latencySeconds,
            sendDrainTimeoutMs, latencyMetrics, schedule).ConfigureAwait(false);
        PrintMultiBenchDiag("DEALER_ROUTER_SENDSEND", msgSize, activeSends,
            durationSeconds, schedule, activeMetrics.MeasureCount,
            latencyMetrics.PacedReceived / (double)latencySeconds);
        var latency = ComputeMultiLatencyStats(latSamples,
            latencyMetrics.SampleSeen, latencyMetrics.LatencySum);
        double p95 = Math.Max(latency.p95, latency.mean);
        return (throughput, latency.mean, p95,
            Math.Max(latency.p99, p95), activeMetrics.MeasureCount,
            latencyMetrics.SampleSeen);
    }

    private static async Task<long> RunWindowAsync(PollManager pollManager,
        DealerRouterClientSlot[] slots, int msgSize, PerfPhase phase,
        int durationSeconds, int latencySeconds, int sendDrainTimeoutMs,
        DealerRouterMetrics metrics, MultiPacedSchedule? schedule)
    {
        const uint runId = 1;
        long seq = 0;
        long accepted = 0;

        var sockets = CollectSockets(slots);
        var eventMasks = new PollEventFlags[slots.Length];
        Array.Fill(eventMasks,
            SocketPollIn | PollEventFlags.PollCompletion);

        // Register the public completion owner before the first async submit.
        // The same poller turn services every client socket and POLLIN.
        _ = PollSocketEvents(pollManager, sockets, eventMasks, 0);

        long benchDeadlineTicks = schedule?.EndTicks(latencySeconds)
            ?? Stopwatch.GetTimestamp() + durationSeconds * Stopwatch.Frequency;
        ulong benchDeadlineNs = EpochNsFromTimestamp(benchDeadlineTicks);
        // C model (perf_multi_client_helpers.hpp drain_timeout_ms): the
        // teardown window is max(PERF_MULTI_SEND_DRAIN_TIMEOUT_MS, 3 s per
        // active second) because small-message runs can fill every per-client
        // Core queue; the backlog is not a workload cap.
        long drainDeadlineTicks = PerfMultiEchoReplyDrain.DeadlineAfter(
            benchDeadlineTicks,
            Math.Max(sendDrainTimeoutMs, Math.Max(1, durationSeconds) * 3000));
        var rotation = new MultiSenderRotation(slots.Length);
        var replies = new PerfMultiEchoReplyDrain();
        void RecordAccepted(ulong dueNs)
        {
            long nowTicks = Stopwatch.GetTimestamp();
            if (schedule != null)
            {
                lock (schedule)
                    schedule.Accepted(EpochNsFromTimestamp(nowTicks), dueNs);
            }
            if (nowTicks < benchDeadlineTicks)
                Interlocked.Increment(ref accepted);
        }

        (bool Submitted, bool VisitedAll) SubmitDue()
        {
            bool submitted = false;
            int visited = 0;
            for (; visited < slots.Length;)
            {
                ulong nowNs = EpochNs();
                if (nowNs >= benchDeadlineNs || (schedule != null && !schedule.Due(nowNs)))
                    break;

                DealerRouterClientSlot slot = slots[rotation.Index];
                slot.ThrowAdmissionError();
                if (slot.AdmissionPending)
                {
                    rotation.Advance();
                    visited++;
                    continue;
                }
                ulong currentSeq = unchecked((ulong)++seq);
                StampMetricHeader(slot.Payload.AsSpan(), runId,
                    phase, msgSize, currentSeq, nowNs);
                if (StartAdmission(slot, replies, schedule, RecordAccepted))
                {
                    submitted = true;
                    if (rotation.Accepted())
                        visited++;
                }
                else
                {
                    rotation.Advance();
                    visited++;
                }
            }
            return (submitted, visited == slots.Length);
        }

        var replyTurn = schedule == null ? null : new MultiPacedReplyTurn(
            () => SubmitDue());
        while (Stopwatch.GetTimestamp() < benchDeadlineTicks)
        {
            var (submitted, visitedAll) = SubmitDue();
            ulong nowNs = EpochNs();
            int completionWaitMs = submitted ? 0
                : schedule?.PollWaitMilliseconds(nowNs, benchDeadlineNs, visitedAll)
                    ?? MultiPacedSchedule.MillisecondsUntil(nowNs, benchDeadlineNs);
            long waitStart = completionWaitMs > 0 && schedule != null
                ? Stopwatch.GetTimestamp() : 0;
            int readyCount = PollSocketEvents(pollManager, sockets,
                eventMasks, completionWaitMs);
            if (waitStart != 0)
                schedule!.RecordPollWait(completionWaitMs, waitStart);
            if (readyCount <= 0)
                continue;

            for (int i = 0; i < readyCount; i++)
                HandleClientEvent(pollManager, slots,
                    ReadySocketIndexAt(pollManager, i),
                    ReadySocketMaskAt(pollManager, i), msgSize,
                    runId, phase, metrics, benchDeadlineTicks, replies,
                    schedule, latencySeconds, replyTurn);
        }

        // Keep receiving every admitted echo inside the configured drain
        // deadline while the binding runtime completes async admissions.
        await replies.WaitAsync(drainDeadlineTicks,
            () => HasPendingAdmissions(slots),
            timeoutMs => PollSocketEvents(pollManager, sockets,
                eventMasks, timeoutMs),
            readyCount =>
            {
                for (int i = 0; i < readyCount; i++)
                    HandleClientEvent(pollManager, slots,
                        ReadySocketIndexAt(pollManager, i),
                        ReadySocketMaskAt(pollManager, i), msgSize,
                        runId, phase, metrics, benchDeadlineTicks, replies,
                        schedule, latencySeconds);
            }).ConfigureAwait(false);
        for (int i = 0; i < slots.Length; i++)
            slots[i].ThrowAdmissionError();

        return Volatile.Read(ref accepted);
    }

    private static bool StartAdmission(DealerRouterClientSlot slot,
        PerfMultiEchoReplyDrain replies, MultiPacedSchedule? schedule,
        Action<ulong> onAccepted)
    {
        Message message = Message.Allocate(slot.Payload.Length);
        slot.Payload.AsSpan().CopyTo(message.AsSpan());
        replies.Submitted();
        try
        {
            SendSubmission submission = PerfSocketIo.SendMeasurementAsync(
                (IDealerSocket)slot.Socket, message, SendFlags.None);
            ulong dueNs = schedule?.Submitted() ?? 0;
            if (submission.Result == SubmitResult.Ok)
            {
                onAccepted(dueNs);
                message.Dispose();
                return true;
            }

            slot.BeginAdmission();
            _ = AwaitAdmissionAndDisposeAsync(submission.Admitted,
                message, replies, slot, onAccepted, dueNs);
            return false;
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
        DealerRouterClientSlot slot, Action<ulong> onAccepted, ulong dueNs)
    {
        Exception? failure = null;
        try
        {
            await admission.ConfigureAwait(false);
            onAccepted(dueNs);
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
        PerfMultiEchoReplyDrain replies, MultiPacedSchedule? schedule,
        int latencySeconds, MultiPacedReplyTurn? replyTurn = null)
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
            replyTurn?.Received();
            if (phase == PerfPhase.Active || phase == PerfPhase.Latency)
            {
                // Every valid echo retires teardown work. Active replies use
                // the receive deadline; paced samples use the send stamp.
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
                    ulong nowNs = EpochNsFromTimestamp(recvTicks);
                    if (phase == PerfPhase.Active)
                    {
                        if (recvTicks < activeDeadlineTicks)
                            metrics.MeasureCount++;
                        continue;
                    }
                    if (schedule == null)
                        continue;
                    if (MultiLatencySample(nowNs, schedule.EpochNs,
                            latencySeconds))
                        metrics.PacedReceived++;
                    if (!MultiLatencySample(header.SentTsNs,
                            schedule.EpochNs, latencySeconds))
                        continue;
                    if (metrics.LatencySamples != null && header.SentTsNs > 0)
                    {
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
        internal long PacedReceived { get; set; }
        internal List<double>? LatencySamples { get; }
        internal int LatencySampleCap { get; }
        internal long SampleSeen { get; set; }
        internal double LatencySum { get; set; }
        internal uint Rng { get; set; }
    }

}
