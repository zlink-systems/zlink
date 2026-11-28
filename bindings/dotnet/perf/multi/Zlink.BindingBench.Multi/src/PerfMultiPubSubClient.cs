using System;
using System.Collections.Generic;
using System.Diagnostics;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiPubSubClient
{
    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int rcvTimeoutMs = ResolveMultiRcvTimeoutMs(options);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        ulong monitorHwmBytes = ResolveMultiMonitorHwmBytes();
        int latencySampleCap = ResolveMultiLatencySampleCap(options);
        int clientCount = ResolveMultiClients(options);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        int latencyDurationSeconds = ResolveMultiLatencyDurationSeconds(options);
        string endpoint = options.Endpoint;

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        using var controlState = new RunnerControlState(size);
        ApplyMultiClientContextOptions(ctx, options);
        var clients = new List<ISocket>(clientCount);
        var monitors = new List<MonitorSocket>(clientCount);
        try
        {
            for (int i = 0; i < clientCount; i++)
            {
                var client = ctx.CreateSubSocket();
                ApplyMultiSocketOptions(client, options);
                ConfigureTlsClientIfNeeded(client, options.Transport);
                client.Options.ReceiveTimeout = TimeSpan.FromMilliseconds(rcvTimeoutMs);
                client.SetSubscription(string.Empty);
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

            WriteStdoutLine($"CLIENT_READY,{size}");

            if (!controlState.WaitForStart(readyTimeoutMs))
            {
                if (!controlState.StopRequested)
                    Console.Error.WriteLine("multi_client_error:no_start");
                return controlState.StopRequested ? 0 : 2;
            }

            var result = RunMultiPubSubClientLoop(pollManager, activeClients,
                size,
                latencySampleCap, durationSeconds, latencyDurationSeconds,
                controlState);
            if (result.measureCount <= 0)
                return 2;

            if (result.caughtUp && result.latencyCount > 0)
            {
                PrintResult(options.Pattern, options.Transport, size,
                    result.throughput, result.latencyNs,
                    result.latencyP95Ns, result.latencyP99Ns);
                WriteStdoutLine(FormattableString.Invariant($"PUBSUB_LATENCY_DIAG,{size},latency_p50_ms,{result.latencyP50Ns / 1_000_000.0:F6}"));
            }
            else
            {
                string prefix = $"dotnet,PUBSUB,{options.Transport},{size}";
                WriteStdoutLine(FormattableString.Invariant($"RESULT,{prefix},throughput,{result.throughput:F3}"));
                WriteStdoutLine(FormattableString.Invariant($"RESULT,{prefix},bandwidth,{result.throughput * size / 1_000_000.0:F3}"));
                foreach (string metric in new[] { "latency", "latency_p95", "latency_p99" })
                    WriteStdoutLine($"RESULT,{prefix},{metric},invalid:receiver_not_caught_up_by_deadline");
                WriteStdoutLine(FormattableString.Invariant($"REFERENCE,{prefix},latency_ref_p50,{result.referenceP50 / 1_000_000.0:F6}"));
                WriteStdoutLine(FormattableString.Invariant($"REFERENCE,{prefix},latency_ref_p99,{result.referenceP99 / 1_000_000.0:F6}"));
                WriteStdoutLine($"REFERENCE,{prefix},inflight_floor,{result.inflightFloor}");
                WriteStdoutLine(FormattableString.Invariant($"REFERENCE,{prefix},inflight_floor_ms,{(result.targetRate > 0.0 ? result.inflightFloor / result.targetRate * 1000.0 : 0.0):F6}"));
            }
            WriteStdoutLine($"PHASE_COMPLETE,{size},{result.pacedReceived}");
            WriteStdoutLine($"CLIENT_DONE,{size}");
            return 0;
        }
        finally
        {
            DisposeAllQuietly(monitors);
            DisposeAllQuietly(clients);
        }
    }

    private static (double throughput, double latencyNs, double latencyP95Ns,
        double latencyP99Ns, long measureCount, long latencyCount,
        long pacedReceived, bool caughtUp, double latencyP50Ns, double referenceP50,
        double referenceP99, long inflightFloor, double targetRate)
        RunMultiPubSubClientLoop(PollManager pollManager,
            List<ISocket> activeClients, int msgSize,
            int latencySampleCap, int durationSeconds,
            int latencyDurationSeconds, RunnerControlState controlState)
    {
        const uint expectedRunId = 1;
        var samples = new List<double>(latencySampleCap);
        var referenceSamples = new List<double>(latencySampleCap);
        long sampleSeen = 0;
        long referenceSeen = 0;
        double sampleSum = 0.0;
        double referenceSum = 0.0;
        uint rng = 0xA341316Cu;
        uint referenceRng = 0x183720AFu;
        var activeReceived = new long[activeClients.Count];
        var pacedReceived = new long[activeClients.Count];
        var latencyReceived = new long[activeClients.Count];
        long measureCount = 0;
        double targetRate = 0.0;
        ulong pacedEpochNs = 0;
        var catchUp = new CatchUpTracker(activeClients.Count);
        long inflightFloor = long.MaxValue;
        ulong nextProgressNs = 0;
        bool phaseSucceeded = true;
        var receiveMasks = new PollEventFlags[activeClients.Count];
        Array.Fill(receiveMasks, PollEventFlags.PollIn);

        var messages = new TopicMessage[activeClients.Count];
        for (int i = 0; i < messages.Length; i++)
            messages[i] = new TopicMessage();
        try
        {
            for (int phaseIndex = 0; phaseIndex < 2; phaseIndex++)
            {
                bool paced = phaseIndex == 1;
                var stopped = new bool[activeClients.Count];
                long startTicks = Stopwatch.GetTimestamp();
                long deadlineTicks = startTicks
                    + (long)(paced ? 2 * latencyDurationSeconds
                        : durationSeconds) * Stopwatch.Frequency;
                long drainDeadlineTicks = deadlineTicks
                    + 10 * Stopwatch.Frequency;
                long lastPayloadTicks = startTicks;
                string donePhase = paced ? "PHASE_DONE,3" : "PHASE_DONE,1";
                bool done = false;
                while (!controlState.StopRequested
                       && Stopwatch.GetTimestamp() < drainDeadlineTicks)
                {
                    bool receivedAny = false;
                    for (int i = 0; i < activeClients.Count; i++)
                    {
                        TopicMessage subscribed = messages[i];
                        for (int turn = 0; turn < MaxBurstPerSender && TrySubscribe(
                                 (ISubSocket)activeClients[i], subscribed,
                                 RecvFlags.DontWait); turn++)
                        {
                            receivedAny = true;
                            lastPayloadTicks = Stopwatch.GetTimestamp();
                            if (!PerfSocketIo.TryMeasurementPayload(
                                    subscribed.Parts, out Message payloadPart))
                                continue;
                            ReadOnlySpan<byte> body = payloadPart.AsReadOnlySpan();
                            if (!PerfRunner.TryDecodeMetricHeader(body,
                                    out PerfMetricHeader header)
                                || header.RunId != expectedRunId
                                || header.MsgSize != (uint)msgSize)
                                continue;
                            if (body.Length == PerfMetricHeaderSize
                                && header.Seq == 0 && header.SentTsNs == 0
                                && header.Phase == (uint)(paced
                                    ? PerfPhase.Latency : PerfPhase.Active))
                            {
                                stopped[i] = true;
                                continue;
                            }
                            if (body.Length != Math.Max(msgSize,
                                    PerfMetricHeaderSize))
                                continue;
                            ulong nowNs = EpochNs();
                            if (!paced && header.Phase == (uint)PerfPhase.Active
                                && lastPayloadTicks < deadlineTicks)
                            {
                                activeReceived[i]++;
                                measureCount++;
                            }
                            if (!paced || (header.Phase != (uint)PerfPhase.Warmup
                                             && header.Phase != (uint)PerfPhase.Latency))
                                continue;
                            pacedReceived[i]++;
                            if (pacedEpochNs == 0 && header.SentTsNs > 0)
                                pacedEpochNs = header.SentTsNs;
                            AddLatencySample(referenceSamples,
                                ref referenceSeen, ref referenceSum,
                                latencySampleCap, ref referenceRng, header, nowNs);
                            if (catchUp.CaughtUp && header.Phase == (uint)PerfPhase.Latency
                                && header.SentTsNs > 0
                                && nowNs >= header.SentTsNs)
                            {
                                latencyReceived[i]++;
                                AddLatencySample(samples, ref sampleSeen,
                                    ref sampleSum, latencySampleCap,
                                    ref rng, header, nowNs);
                            }
                        }
                    }
                    ulong tickNs = EpochNs();
                    if (paced && pacedEpochNs != 0 && !catchUp.CaughtUp
                        && tickNs - pacedEpochNs <= (ulong)latencyDurationSeconds * 1_000_000_000UL)
                    {
                        double elapsed = (tickNs - pacedEpochNs) / 1_000_000_000.0;
                        long scheduled = (long)(targetRate * elapsed);
                        bool caughtUp = catchUp.Observe(tickNs, pacedReceived, scheduled);
                        if (tickNs >= nextProgressNs)
                        {
                            inflightFloor = Math.Min(inflightFloor, catchUp.InFlight);
                            WriteStdoutLine(FormattableString.Invariant($"PUBSUB_CATCHUP_PROGRESS,{msgSize},{elapsed:G12},{catchUp.InFlight},{catchUp.MinReceived},{scheduled},{targetRate:G12}"));
                            nextProgressNs = tickNs + 1_000_000_000UL;
                        }
                        if (caughtUp)
                        {
                            WriteStdoutLine($"PHASE_CATCHUP,{msgSize}");
                            WriteStdoutLine(FormattableString.Invariant($"PUBSUB_CATCHUP_DIAG,{msgSize},{elapsed:G12},{catchUp.InFlight},window_start_in_flight,{catchUp.WindowStartInFlight}"));
                        }
                    }
                    done = controlState.TryGetPhase(donePhase, out _);
                    bool allStopped = true;
                    for (int i = 0; i < stopped.Length; i++)
                        allStopped &= stopped[i];
                    long nowTicks = Stopwatch.GetTimestamp();
                    if (done && (allStopped || (nowTicks >= deadlineTicks
                        && nowTicks - lastPayloadTicks
                            >= Stopwatch.Frequency / 20)))
                        break;
                    if (!receivedAny)
                        _ = PollSocketEvents(pollManager, activeClients,
                            receiveMasks, 10);
                }
                if (!done)
                {
                    phaseSucceeded = false;
                    break;
                }
                if (!paced)
                {
                    long smallest = Smallest(activeReceived);
                    if (smallest <= 0)
                    {
                        phaseSucceeded = false;
                        break;
                    }
                    targetRate = MultiTargetRate(smallest, durationSeconds);
                    WriteStdoutLine($"PHASE_DRAINED,{msgSize},{smallest}");
                }
            }
        }
        finally
        {
            foreach (TopicMessage message in messages)
                message.Dispose();
        }

        var latency = ComputeMultiLatencyStats(samples,
            sampleSeen, sampleSum);
        referenceSamples.Sort();
        double referenceP50 = Percentile(referenceSamples, 0.50);
        double referenceP99 = Percentile(referenceSamples, 0.99);
        return (measureCount / (double)durationSeconds, latency.mean,
            latency.p95, latency.p99,
            phaseSucceeded ? measureCount : 0, sampleSeen,
            Smallest(latencyReceived), catchUp.CaughtUp, Percentile(samples, 0.50),
            referenceP50,
            referenceP99, inflightFloor == long.MaxValue ? 0 : inflightFloor,
            targetRate);

        static long Smallest(long[] counts)
        {
            long min = long.MaxValue;
            foreach (long count in counts)
                min = Math.Min(min, count);
            return min == long.MaxValue ? 0 : min;
        }

        static double Percentile(IReadOnlyList<double> sorted, double q)
        {
            if (sorted.Count == 0)
                return 0.0;
            double pos = (sorted.Count - 1) * q;
            int lo = (int)pos;
            int hi = Math.Min(lo + 1, sorted.Count - 1);
            return sorted[lo] + (sorted[hi] - sorted[lo]) * (pos - lo);
        }
    }

    internal sealed class CatchUpTracker(int subscriberCount)
    {
        private const ulong WindowNs = 500_000_000UL;
        private const long ReceiveQuantum = 1;
        private readonly long[] _windowReceived = new long[subscriberCount];
        private ulong _windowStartNs;
        private long _windowScheduled;

        internal long MinReceived { get; private set; }
        internal long InFlight { get; private set; }
        internal long WindowStartInFlight { get; private set; }
        internal bool CaughtUp { get; private set; }

        internal bool Observe(ulong nowNs, ReadOnlySpan<long> received, long scheduled)
        {
            if (CaughtUp)
                return true;

            long smallest = received[0];
            for (int i = 1; i < received.Length; i++)
                smallest = Math.Min(smallest, received[i]);
            MinReceived = smallest;
            InFlight = Math.Max(0, scheduled - MinReceived);

            if (_windowStartNs != 0 && InFlight <= WindowStartInFlight)
            {
                if (nowNs - _windowStartNs < WindowNs)
                    return false;

                // The slowest subscriber can change within the window.
                long minimumDelta = received[0] - _windowReceived[0];
                for (int i = 1; i < received.Length; i++)
                    minimumDelta = Math.Min(minimumDelta, received[i] - _windowReceived[i]);
                if (minimumDelta + ReceiveQuantum >= scheduled - _windowScheduled)
                {
                    CaughtUp = true;
                    return true;
                }
            }

            _windowStartNs = nowNs;
            WindowStartInFlight = InFlight;
            _windowScheduled = scheduled;
            received.CopyTo(_windowReceived);
            return false;
        }
    }

    private static void AddLatencySample(List<double> samples,
        ref long sampleSeen, ref double sampleSum, int latencySampleCap,
        ref uint rng, PerfMetricHeader header, ulong recvNs)
    {
        if (header.SentTsNs == 0)
            return;
        if (recvNs < header.SentTsNs)
            return;
        ReservoirSampleMulti(samples, recvNs - header.SentTsNs,
            ref sampleSeen, ref sampleSum, latencySampleCap, ref rng);
    }

    private static bool TrySubscribe(ISubSocket socket, TopicMessage result,
        RecvFlags flags)
    {
        try
        {
            return socket.Subscribe(result, flags);
        }
        catch (ZlinkException ex) when (IsWouldBlock(ex.NativeErrno)
                                        || IsInterrupted(ex.NativeErrno))
        {
            return false;
        }
    }
}
