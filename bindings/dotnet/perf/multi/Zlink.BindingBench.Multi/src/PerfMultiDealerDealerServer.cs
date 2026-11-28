using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Systems.Zlink;
using static PerfRunner;

internal static class PerfMultiDealerDealerServer
{
    private const int ServerSocketTag = 0;
    private const int ActiveDeadlineTag = int.MaxValue;

    private enum ReceiveStatus
    {
        NoData,
        Message,
        StopToken,
    }

    internal static int Run(PerfOptions options)
    {
        int size = Math.Max(1, options.Size);
        int readyTimeoutMs = ResolveMultiConnectReadyTimeoutMs(options);
        ulong monitorHwmBytes = ResolveMultiMonitorHwmBytes();
        int clientCount = ResolveMultiClients(options);
        int durationSeconds = ResolveMultiDurationSeconds(options);
        int latencyDurationSeconds = ResolveMultiLatencyDurationSeconds(options);
        int latencySampleCap = ResolveMultiLatencySampleCap(options);
        string endpoint = MultiEndpointFor(options.Transport,
            "multi-dealer-dealer", options);

        using var ctx = Zlink.CreateContext();
        using var pollManager = new PollManager();
        using var controlState = new RunnerControlState(size);
        ApplyMultiServerContextOptions(ctx, options);
        using var server = ctx.CreateDealerSocket();
        ApplyMultiSocketOptions(server, options);
        ConfigureTlsServerIfNeeded(server, options.Transport);
        using var monitor = server.MonitorOpen(SocketEvent.ConnectionReady,
            monitorHwmBytes);

        server.Bind(endpoint);
        endpoint = server.Options.LastEndpoint;
        WriteStdoutLine($"READY,{endpoint}");

        if (!WaitConnectReadyCount(monitor, clientCount, readyTimeoutMs))
            return 2;

        RecalculateAutoHwm(ctx);
        PrintAutoHwmSnapshot(server, "server", options.Transport, size);

        if (!controlState.WaitForStart(readyTimeoutMs))
        {
            if (!controlState.StopRequested)
                Console.Error.WriteLine("multi_server_error:no_start");
            return controlState.StopRequested ? 0 : 2;
        }

        var result = RunReceivePhase(server, size, latencySampleCap,
            durationSeconds, latencyDurationSeconds, clientCount,
            controlState);
        if (result.measureCount <= 0 || result.latencyCount <= 0)
            return 2;

        PrintResult(options.Pattern, options.Transport, size, result.throughput,
            result.latencyNs, result.latencyP95Ns, result.latencyP99Ns);
        WriteStdoutLine($"PHASE_COMPLETE,{size},{result.pacedReceived}");
        return 0;
    }

    private static (double throughput, double latencyNs, double latencyP95Ns,
        double latencyP99Ns, long measureCount, long latencyCount,
        long pacedReceived)
        RunReceivePhase(IDealerSocket server, int msgSize,
            int latencySampleCap, int durationSeconds,
            int latencyDurationSeconds, int clientCount,
            RunnerControlState controlState)
    {
        const uint expectedRunId = 1;
        var latSamples = new LatencySampleBuffer(latencySampleCap);
        long measureCount = 0;
        using var received = Received.Create();

        if (!ReceiveActiveWindow(server, received, msgSize,
                expectedRunId, PerfPhase.Active, latSamples, ref measureCount,
                durationSeconds, clientCount, controlState))
        {
            return (0.0, 0.0, 0.0, 0.0, 0, 0, 0);
        }

        double configuredSeconds = Math.Max(1.0, durationSeconds);
        double throughput = measureCount / configuredSeconds;
        WriteStdoutLine($"PHASE_DRAINED,{msgSize},{measureCount}");
        long pacedReceived = 0;
        if (!ReceiveActiveWindow(server, received, msgSize,
                expectedRunId, PerfPhase.Latency, latSamples,
                ref pacedReceived, latencyDurationSeconds + MultiSettleNs / 1_000_000_000.0,
                clientCount, controlState))
            return (0.0, 0.0, 0.0, 0.0, 0, 0, 0);

        var latency = latSamples.ComputeStats();
        double latencyNs = latency.mean;
        double latencyP95Ns = Math.Max(latency.p95, latencyNs);
        double latencyP99Ns = Math.Max(latency.p99, latencyP95Ns);

        return (throughput, latencyNs, latencyP95Ns, latencyP99Ns,
            measureCount, latSamples.Count, pacedReceived);
    }

    private static bool ReceiveActiveWindow(IDealerSocket server, Received received, int msgSize,
        uint expectedRunId, PerfPhase expectedPhase,
        LatencySampleBuffer latSamples, ref long messageCount,
        double durationSeconds, int expectedStopCount,
        RunnerControlState controlState)
    {
        using var activeTimer = Zlink.CreateTimer();
        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[2];
        long activeDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)(Math.Max(0.5, durationSeconds) * Stopwatch.Frequency);
        if (expectedPhase == PerfPhase.Latency)
            latSamples.PacedSampleStartNs = EpochNs() + MultiSettleNs;
        poller.Add(server, PollEventFlags.PollIn, ServerSocketTag);
        poller.Add(activeTimer, ActiveDeadlineTag);
        activeTimer.Start(TimeSpan.FromSeconds(Math.Max(0.5, durationSeconds)),
            1);

        int stopTokenCount = 0;
        while (!controlState.StopRequested)
        {
            int written = poller.Wait(events, TimeSpan.FromMilliseconds(-1));
            if (written == 0)
            {
                if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                    break;
                continue;
            }

            for (int i = 0; i < written; i++)
            {
                if (events[i].Slot == (nuint)ActiveDeadlineTag)
                {
                    _ = activeTimer.Recv();
                    goto active_window_complete;
                }

                if (events[i].Slot != (nuint)ServerSocketTag
                    || (events[i].Revents & PollEventFlags.PollIn) == 0)
                    continue;

                ReceiveStatus receiveStatus = ReceiveOneAvailable(server,
                    received, msgSize, expectedRunId, expectedPhase,
                    latSamples, ref messageCount,
                    collectMetrics: true);
                if (receiveStatus == ReceiveStatus.NoData)
                    continue;

                if (receiveStatus == ReceiveStatus.StopToken)
                    stopTokenCount++;
                if (stopTokenCount >= expectedStopCount)
                    goto active_window_complete;

                // C receives one message after readiness, then checks the
                // active boundary before a DONT_WAIT drain. Counting a
                // queued tail after this point would include post-window
                // traffic in the next result.
                if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                    goto active_window_complete;

                DrainAvailable(server, received, msgSize, expectedRunId,
                    expectedPhase, latSamples, ref messageCount,
                    ref stopTokenCount, collectMetrics: true);
                if (stopTokenCount >= expectedStopCount)
                    goto active_window_complete;

                if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                    goto active_window_complete;
            }
        }

    active_window_complete:
        if (controlState.StopRequested)
            return true;
        poller.Remove(activeTimer);

        double drainWaitSeconds = msgSize >= 65536
            ? Math.Max(10.0, durationSeconds * 2.0)
            : Math.Max(2.0, durationSeconds);
        bool drainComplete = DrainPhaseUntilStops(poller, events, server,
            received, msgSize, expectedRunId, expectedPhase, latSamples,
            ref messageCount,
            ref stopTokenCount, expectedStopCount, controlState,
            drainWaitSeconds);
        return drainComplete;
    }

    private static ReceiveStatus ReceiveOneAvailable(IDealerSocket server,
        Received received, int msgSize, uint expectedRunId,
        PerfPhase expectedPhase, LatencySampleBuffer latSamples,
        ref long messageCount, bool collectMetrics)
    {
        if (!server.Recv(received, RecvFlags.DontWait))
            return ReceiveStatus.NoData;

        bool stopToken = ProcessReceived(server, received, msgSize,
            expectedRunId, expectedPhase, latSamples, ref messageCount,
            collectMetrics);
        return stopToken ? ReceiveStatus.StopToken : ReceiveStatus.Message;
    }

    private static void DrainAvailable(IDealerSocket server, Received received,
        int msgSize, uint expectedRunId, PerfPhase expectedPhase,
        LatencySampleBuffer latSamples, ref long messageCount,
        ref int stopTokenCount, bool collectMetrics)
    {
        while (true)
        {
            ReceiveStatus receiveStatus = ReceiveOneAvailable(server,
                received, msgSize, expectedRunId, expectedPhase,
                latSamples, ref messageCount,
                collectMetrics);
            if (receiveStatus == ReceiveStatus.NoData)
                return;
            if (receiveStatus == ReceiveStatus.StopToken)
                stopTokenCount++;
        }
    }

    private static bool DrainPhaseUntilStops(IPoller poller, PollEvent[] events,
        IDealerSocket server, Received received, int msgSize,
        uint expectedRunId, PerfPhase expectedPhase,
        LatencySampleBuffer latSamples, ref long messageCount,
        ref int stopTokenCount, int expectedStopCount,
        RunnerControlState controlState, double maxWaitSeconds)
    {
        long drainDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1.0, maxWaitSeconds) * Stopwatch.Frequency;

        while (!controlState.StopRequested
               && Stopwatch.GetTimestamp() < drainDeadlineTicks)
        {
            if (stopTokenCount >= expectedStopCount)
                return true;
            ReceiveStatus receiveStatus = ReceiveOneAvailable(server,
                received, msgSize, expectedRunId, expectedPhase,
                latSamples,
                ref messageCount, collectMetrics: false);
            if (receiveStatus == ReceiveStatus.NoData)
            {
                _ = poller.Wait(events, TimeSpan.FromMilliseconds(10));
                continue;
            }

            if (receiveStatus == ReceiveStatus.StopToken)
                stopTokenCount++;
        }

        return stopTokenCount >= expectedStopCount;
    }

    private static bool ProcessReceived(IDealerSocket server,
        Received received, int msgSize, uint expectedRunId,
        PerfPhase expectedPhase, LatencySampleBuffer latSamples,
        ref long messageCount, bool collectMetrics)
    {
        if (received.Parts.Count == 1
            && IsStopTokenPayload(received.FirstPart().AsReadOnlySpan()))
            return true;

        if (!PerfSocketIo.TryMeasurementPayload(received.Parts,
                out Message payloadPart))
            return false;
        ReadOnlySpan<byte> body = payloadPart.AsReadOnlySpan();

        bool matched = TryDecodeActiveHeader(body, msgSize,
            expectedRunId, expectedPhase, out ulong sentTsNs);
        bool warmup = expectedPhase == PerfPhase.Latency
            && TryDecodeActiveHeader(body, msgSize, expectedRunId,
                PerfPhase.Warmup, out _);
        if (!matched && !warmup)
            return false;

        if (!collectMetrics && expectedPhase != PerfPhase.Latency)
            return false;

        ulong nowNs = EpochNs();
        if (collectMetrics && (expectedPhase == PerfPhase.Active
            || nowNs >= latSamples.PacedSampleStartNs))
            messageCount++;
        if (matched && expectedPhase == PerfPhase.Latency
            && sentTsNs > 0)
        {
            if (nowNs >= sentTsNs)
                latSamples.Add(nowNs - sentTsNs);
        }

        return false;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static bool TryDecodeActiveHeader(ReadOnlySpan<byte> payload,
        int expectedMsgSize, uint expectedRunId, PerfPhase expectedPhase,
        out ulong sentTsNs)
    {
        sentTsNs = 0;
        if (payload.Length != Math.Max(expectedMsgSize,
                PerfMetricHeaderSize))
            return false;

        ref byte head = ref MemoryMarshal.GetReference(payload);
        if (Unsafe.ReadUnaligned<uint>(ref head) != PerfShared.PerfMetricMagic)
            return false;
        if (Unsafe.ReadUnaligned<uint>(ref Unsafe.Add(ref head, 4))
            != expectedRunId)
            return false;
        if (Unsafe.Add(ref head, 8) != (byte)expectedPhase)
            return false;
        if (Unsafe.ReadUnaligned<uint>(ref Unsafe.Add(ref head, 9))
            != (uint)expectedMsgSize)
            return false;

        sentTsNs = (ulong)Unsafe.ReadUnaligned<long>(
            ref Unsafe.Add(ref head, 21));
        return true;
    }

    private sealed class LatencySampleBuffer
    {
        private double[] _samples;
        private readonly int _sampleCap;
        private int _sampleCount;
        private ulong _samplesSeen;
        private double _sum;
        private ulong _rng = 0xA341316Cu;
        internal ulong PacedSampleStartNs;

        internal LatencySampleBuffer(int sampleCap)
        {
            _sampleCap = Math.Max(0, sampleCap);
            _samples = new double[_sampleCap];
        }

        internal long Count => (long)_samplesSeen;

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        internal void Add(double value)
        {
            double sample = value >= 0.0 ? value : 0.0;
            _sum += sample;
            _samplesSeen++;
            if (_sampleCap == 0)
                return;
            if (_sampleCount < _sampleCap)
            {
                _samples[_sampleCount++] = sample;
                return;
            }
            _rng = _rng * 1664525u + 1013904223u;
            ulong slot = _rng % _samplesSeen;
            if (slot < (ulong)_sampleCount)
                _samples[(int)slot] = sample;
        }

        internal (double mean, double p95, double p99) ComputeStats()
        {
            if (_samplesSeen == 0)
                return (0.0, 0.0, 0.0);

            double mean = _sum / _samplesSeen;
            if (_sampleCount == 0)
                return (mean, mean, mean);
            Array.Sort(_samples, 0, _sampleCount);
            return (mean, PercentileFromSorted(0.95),
                PercentileFromSorted(0.99));
        }

        private double PercentileFromSorted(double q)
        {
            if (_sampleCount == 0)
                return 0.0;
            if (q <= 0.0)
                return _samples[0];
            if (q >= 1.0)
                return _samples[_sampleCount - 1];

            double pos = (_sampleCount - 1) * q;
            int lo = (int)pos;
            int hi = lo + 1 < _sampleCount ? lo + 1 : lo;
            double frac = pos - lo;
            return _samples[lo] + (_samples[hi] - _samples[lo]) * frac;
        }
    }
}
