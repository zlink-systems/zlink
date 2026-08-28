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
            durationSeconds, controlState);
        if (result.measureCount <= 0)
            return 2;

        PrintResult(options.Pattern, options.Transport, size, result.throughput,
            result.latencyNs, result.latencyP95Ns, result.latencyP99Ns);
        return 0;
    }

    private static (double throughput, double latencyNs, double latencyP95Ns,
        double latencyP99Ns, long measureCount)
        RunReceivePhase(IDealerSocket server, int msgSize, int latencySampleCap,
            int durationSeconds, RunnerControlState controlState)
    {
        const uint expectedRunId = 1;
        var latSamples = new LatencySampleBuffer(latencySampleCap);
        long measureCount = 0;
        using var received = Received.Create();

        // PERF_MULTI_TEST_POLICY: throughput and latency are collected only
        // during the configured active duration, matching C. After that
        // window, C drains queued payloads and stop tokens until idle before
        // the size process closes. The .NET runner has one process per size,
        // but keeps the same cleanup boundary so later client stop-token sends
        // do not race an early server close.
        if (!ReceiveActiveWindow(server, received, msgSize, expectedRunId,
                PerfPhase.Active, latSamples, ref measureCount,
                durationSeconds, controlState))
        {
            return (0.0, 0.0, 0.0, 0.0, 0);
        }

        double configuredSeconds = Math.Max(1.0, durationSeconds);
        double throughput = measureCount / configuredSeconds;
        // PERF_POLICY: report measured latency only. C
        // normalize_latency_stats reports zeros when no samples and never
        // fabricates a duration-derived latency.
        var latency = latSamples.ComputeStats();
        double latencyNs = latency.mean;
        double latencyP95Ns = Math.Max(latency.p95, latencyNs);
        double latencyP99Ns = Math.Max(latency.p99, latencyP95Ns);

        return (throughput, latencyNs, latencyP95Ns, latencyP99Ns, measureCount);
    }

    private static bool ReceiveActiveWindow(IDealerSocket server,
        Received received, int msgSize, uint expectedRunId,
        PerfPhase expectedPhase, LatencySampleBuffer latSamples,
        ref long messageCount, int durationSeconds,
        RunnerControlState controlState)
    {
        using var activeTimer = Zlink.CreateTimer();
        using var poller = Zlink.CreatePoller();
        var events = new PollEvent[2];
        long activeDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1, durationSeconds) * Stopwatch.Frequency;
        poller.Add(server, PollEventFlags.PollIn, ServerSocketTag);
        poller.Add(activeTimer, ActiveDeadlineTag);
        activeTimer.Start(TimeSpan.FromSeconds(Math.Max(1, durationSeconds)),
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

                // C receives one message after readiness, then checks the
                // active boundary before a DONT_WAIT drain. Counting a
                // queued tail after this point would include post-window
                // traffic in the next result.
                if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                    goto active_window_complete;

                DrainAvailable(server, received, msgSize, expectedRunId,
                    expectedPhase, latSamples, ref messageCount,
                    ref stopTokenCount, collectMetrics: true);

                if (Stopwatch.GetTimestamp() >= activeDeadlineTicks)
                    goto active_window_complete;
            }
        }

    active_window_complete:
        if (controlState.StopRequested)
            return true;

        const double drainWaitSeconds = 2.0;
        const int idleWaitMs = 50;
        bool drainComplete = DrainPhaseUntilIdle(poller, events, server,
            received, msgSize, expectedRunId, expectedPhase, latSamples,
            ref messageCount,
            ref stopTokenCount, controlState, drainWaitSeconds, idleWaitMs);
        _ = stopTokenCount;
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

    private static bool DrainPhaseUntilIdle(IPoller poller, PollEvent[] events,
        IDealerSocket server, Received received, int msgSize,
        uint expectedRunId, PerfPhase expectedPhase,
        LatencySampleBuffer latSamples, ref long messageCount,
        ref int stopTokenCount, RunnerControlState controlState,
        double maxWaitSeconds, int idleWaitMs)
    {
        long drainDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1.0, maxWaitSeconds) * Stopwatch.Frequency;
        long idleDeadlineTicks = Stopwatch.GetTimestamp()
            + (long)Math.Max(1, idleWaitMs) * Stopwatch.Frequency / 1000;

        while (!controlState.StopRequested
               && Stopwatch.GetTimestamp() < drainDeadlineTicks)
        {
            ReceiveStatus receiveStatus = ReceiveOneAvailable(server,
                received, msgSize, expectedRunId, expectedPhase,
                latSamples,
                ref messageCount, collectMetrics: false);
            if (receiveStatus == ReceiveStatus.NoData)
            {
                long nowTicks = Stopwatch.GetTimestamp();
                if (nowTicks >= idleDeadlineTicks)
                    return true;

                long remainingTicks = Math.Min(
                    idleDeadlineTicks - nowTicks,
                    drainDeadlineTicks - nowTicks);
                int waitMs = (int)Math.Max(1,
                    Math.Min(idleWaitMs,
                        (remainingTicks * 1000L + Stopwatch.Frequency - 1)
                            / Stopwatch.Frequency));
                _ = poller.Wait(events, TimeSpan.FromMilliseconds(waitMs));
                continue;
            }

            if (receiveStatus == ReceiveStatus.StopToken)
                stopTokenCount++;
            idleDeadlineTicks = Stopwatch.GetTimestamp()
                + (long)Math.Max(1, idleWaitMs) * Stopwatch.Frequency / 1000;
        }

        return controlState.StopRequested;
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

        if (!TryDecodeActiveHeader(body, msgSize, expectedRunId,
                expectedPhase, out ulong sentTsNs))
            return false;

        if (!collectMetrics)
            return false;

        messageCount++;
        if (sentTsNs > 0)
        {
            ulong nowNs = EpochNs();
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
        if (payload.Length < PerfMetricHeaderSize)
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

        internal LatencySampleBuffer(int sampleCap)
        {
            _sampleCap = Math.Max(0, sampleCap);
            _samples = new double[_sampleCap];
        }

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
